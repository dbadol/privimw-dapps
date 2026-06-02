// PriviBets Roulette Contract Shader
// American Roulette: 38 outcomes (0-37, where 37 = 00)
#include "common.h"
#include "roulette.h"
#include "roulette_calc.h"

namespace BeamRoulette {

// Global state (singleton)
static State g_State;

// Load state on first access
State& GetState() {
    if (!g_State.m_Initialized) {
        StateKey k;
        if (!Env::LoadVar_T(k, g_State)) {
            Env::Halt();
        }
        g_State.m_Initialized = 1;
    }
    return g_State;
}

// Save state
void SaveState() {
    StateKey k;
    Env::SaveVar_T(k, g_State);
}

// Calculate available pool balance (safe, no underflow)
// Available = total_in_contract - total_withdrawn - pending_max_payout - pending_payouts
uint64_t GetAvailableBalance() {
    State& s = GetState();
    uint64_t total = s.m_TotalDeposited + s.m_TotalBets;
    if (s.m_TotalPayouts > total) return 0;
    total -= s.m_TotalPayouts;
    if (s.m_TotalWithdrawn > total) return 0;
    total -= s.m_TotalWithdrawn;

    uint64_t reserved = s.m_PendingMaxPayout + s.m_PendingPayouts;
    if (reserved > total) return 0;
    return total - reserved;
}

// Calculate shared spin result from stored placement hash + reveal height + spin ID.
// Returns 0-37 (37 = 00). Same formula in contract and app shader (no get_HdrInfo needed).
// Security: placementHash is the block hash at placement time — unknowable when
// constructing the TX (circular dependency). All bets in one spin share the same result.
uint8_t CalculateSpinResult(const HashValue& placementHash, Height revealAt, uint64_t spinId) {
    HashProcessor::Sha256 hp;
    hp.Write(placementHash);
    hp.Write(&revealAt, sizeof(revealAt));
    hp.Write(&spinId, sizeof(spinId));

    HashValue resultHash;
    hp >> resultHash;

    uint16_t rawValue = (static_cast<uint16_t>(resultHash.m_p[0]) << 8) | resultHash.m_p[1];
    return rawValue % 38;  // 0-37 (37 = 00)
}

// Get multiplier for a bet type from state
uint64_t GetMultiplier(const State& s, uint8_t betType) {
    switch (betType) {
        case BetType::Straight: return s.m_StraightMult;
        case BetType::Red:
        case BetType::Black:
        case BetType::Odd:
        case BetType::Even:
        case BetType::Low:
        case BetType::High:    return s.m_EvenMoneyMult;
        case BetType::Dozen1:
        case BetType::Dozen2:
        case BetType::Dozen3:
        case BetType::Column1:
        case BetType::Column2:
        case BetType::Column3: return s.m_DozenColMult;
        default: return 0;
    }
}

// Process spin result: reveal and check each bet position
void ProcessSpinResult(Spin& spin, uint8_t result, State& s) {
    spin.m_Result = result;
    spin.m_TotalPayout = 0;
    bool anyWon = false;

    for (uint8_t i = 0; i < spin.m_NumBets; i++) {
        BetPosition& bp = spin.m_Bets[i];
        if (IsBetWon(bp.m_Type, bp.m_Number, result)) {
            bp.m_Won = 1;
            bp.m_Payout = (bp.m_Amount * bp.m_Multiplier) / 100;
            spin.m_TotalPayout += bp.m_Payout;
            anyWon = true;
        } else {
            bp.m_Won = 0;
            bp.m_Payout = 0;
        }
    }

    // Reserved max must cover any single-wheel outcome total (solvency invariant)
    if (spin.m_TotalPayout > spin.m_MaxPayout) Env::Halt();

    if (anyWon) {
        spin.m_Status = SpinStatus::Won;
        uint64_t newPending = 0;
        if (!CheckedAdd(s.m_PendingPayouts, spin.m_TotalPayout, newPending)) Env::Halt();
        s.m_PendingPayouts = newPending;
    } else {
        spin.m_Status = SpinStatus::Lost;
    }

    // Release from pending tracking (safe decrement)
    if (spin.m_TotalWagered <= s.m_PendingBets)
        s.m_PendingBets -= spin.m_TotalWagered;
    else
        s.m_PendingBets = 0;

    if (spin.m_MaxPayout <= s.m_PendingMaxPayout)
        s.m_PendingMaxPayout -= spin.m_MaxPayout;
    else
        s.m_PendingMaxPayout = 0;
}

// Accumulate claim accounting: marks spin Claimed, returns payout. Caller does FundsUnlock.
void AccumulateClaim(Spin& spin, State& s, uint64_t& totalPayout) {
    if (spin.m_Status == SpinStatus::Won && spin.m_TotalPayout > 0) {
        totalPayout += spin.m_TotalPayout;
        s.m_TotalPayouts += spin.m_TotalPayout;
        if (spin.m_TotalPayout <= s.m_PendingPayouts)
            s.m_PendingPayouts -= spin.m_TotalPayout;
        else
            s.m_PendingPayouts = 0;
        spin.m_Status = SpinStatus::Claimed;
    }
}

// Auto-resolve expired pending spins (piggybacked onto user interactions)
void AutoResolveExpired(State& s, uint32_t maxCount, uint32_t maxScan = 20) {
    Height hCurrent = Env::get_Height();
    uint32_t resolved = 0;
    uint32_t scanned = 0;

    for (uint64_t spinId = s.m_FirstUnresolvedSpinId; spinId < s.m_NextSpinId && resolved < maxCount && scanned < maxScan; spinId++) {
        scanned++;
        SpinKey sk;
        sk.m_SpinId = spinId;

        Spin spin;
        if (!Env::LoadVar_T(sk, spin)) continue;
        if (spin.m_Status != SpinStatus::Pending) continue;
        if (hCurrent < spin.m_RevealAt) break; // remaining spins are newer, stop

        uint8_t result = CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
        ProcessSpinResult(spin, result, s);

        Env::SaveVar_T(sk, spin);
        resolved++;
    }
}

// Advance m_FirstUnresolvedSpinId past resolved spins
void AdvanceFirstUnresolved(State& s, uint32_t maxAdvance = 50) {
    uint32_t count = 0;
    while (s.m_FirstUnresolvedSpinId < s.m_NextSpinId && count < maxAdvance) {
        SpinKey sk;
        sk.m_SpinId = s.m_FirstUnresolvedSpinId;
        Spin spin;
        if (!Env::LoadVar_T(sk, spin)) {
            s.m_FirstUnresolvedSpinId++;
            count++;
            continue;
        }
        if (spin.m_Status == SpinStatus::Pending)
            break;
        s.m_FirstUnresolvedSpinId++;
        count++;
    }
}

} // namespace BeamRoulette

// ============================================================================
// Constructor
// ============================================================================
BEAM_EXPORT void Ctor(const BeamRoulette::Params& r)
{
    _POD_(BeamRoulette::g_State).SetZero();
    BeamRoulette::g_State.m_MinBet = BeamRoulette::s_DefaultMinBet;
    BeamRoulette::g_State.m_MaxBet = BeamRoulette::s_DefaultMaxBet;
    BeamRoulette::g_State.m_StraightMult = BeamRoulette::s_DefaultStraightMult;
    BeamRoulette::g_State.m_EvenMoneyMult = BeamRoulette::s_DefaultEvenMoneyMult;
    BeamRoulette::g_State.m_DozenColMult = BeamRoulette::s_DefaultDozenColMult;
    BeamRoulette::g_State.m_RevealEpoch = BeamRoulette::s_RevealEpoch;
    BeamRoulette::g_State.m_AssetId = 0;
    BeamRoulette::g_State.m_NextSpinId = 1;
    BeamRoulette::g_State.m_FirstUnresolvedSpinId = 1;

    _POD_(BeamRoulette::g_State.m_OwnerPk) = r.m_OwnerPk;

    BeamRoulette::g_State.m_Initialized = 1;
    BeamRoulette::SaveState();
}

// ============================================================================
// Destructor - Owner only, requires no active obligations
// ============================================================================
BEAM_EXPORT void Dtor(void*)
{
    BeamRoulette::State& s = BeamRoulette::GetState();

    if (s.m_PendingBets > 0) Env::Halt();
    if (s.m_PendingMaxPayout > 0) Env::Halt();
    if (s.m_PendingPayouts > 0) Env::Halt();

    Env::AddSig(s.m_OwnerPk);
}

// ============================================================================
// Method 2: Place Bets (Anyone can call — up to 10 bet positions per spin)
// ============================================================================
BEAM_EXPORT void Method_2(const BeamRoulette::Method::PlaceBets& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();

    // Contract must not be paused
    if (s.m_Paused) Env::Halt();

    // Asset must match
    if (r.m_AssetId != s.m_AssetId) Env::Halt();

    // Validate number of bets
    if (r.m_NumBets < 1 || r.m_NumBets > BeamRoulette::s_MaxBetsPerSpin) Env::Halt();

    // Create spin record
    BeamRoulette::Spin spin;
    _POD_(spin).SetZero();
    spin.m_SpinId = s.m_NextSpinId++;
    _POD_(spin.m_UserPk) = r.m_UserPk;
    spin.m_NumBets = r.m_NumBets;
    spin.m_Status = BeamRoulette::SpinStatus::Pending;
    spin.m_CreatedHeight = Env::get_Height();
    spin.m_RevealAt = spin.m_CreatedHeight + s.m_RevealEpoch;

    // Store placement block hash for deterministic result calculation
    // Unknowable when constructing the TX (circular dependency on block hash)
    BlockHeader::Info hdr;
    hdr.m_Height = spin.m_CreatedHeight;
    Env::get_HdrInfo(hdr);
    _POD_(spin.m_PlacementHash) = hdr.m_Hash;

    uint64_t totalWagered = 0;
    uint8_t legTypes[10];
    uint8_t legNumbers[10];
    uint64_t legAmounts[10];
    uint64_t legMults[10];

    // Validate and populate each bet position
    for (uint8_t i = 0; i < r.m_NumBets; i++) {
        uint8_t betType = r.m_Types[i];
        uint8_t betNumber = r.m_Numbers[i];
        uint64_t amount = r.m_Amounts[i];

        // Validate bet type (0-12)
        if (betType > 12) Env::Halt();

        // Straight bets: number must be 0-37 (37=00)
        if (betType == BeamRoulette::BetType::Straight) {
            if (betNumber > 37) Env::Halt();
        }

        // Amount must be within limits
        if (amount < s.m_MinBet || amount > s.m_MaxBet) Env::Halt();

        // Get multiplier for this bet type
        uint64_t mult = BeamRoulette::GetMultiplier(s, betType);
        if (mult == 0) Env::Halt();

        // Overflow check: amount * mult must not overflow
        if (amount > static_cast<uint64_t>(-1) / mult) Env::Halt();

        // Populate bet position
        BeamRoulette::BetPosition& bp = spin.m_Bets[i];
        bp.m_Type = betType;
        bp.m_Number = betNumber;
        bp.m_Amount = amount;
        bp.m_Multiplier = mult;
        bp.m_Payout = 0;
        bp.m_Won = 0;

        legTypes[i] = betType;
        legNumbers[i] = betNumber;
        legAmounts[i] = amount;
        legMults[i] = mult;

        if (!BeamRoulette::CheckedAdd(totalWagered, amount, totalWagered)) Env::Halt();
    }

    BeamRoulette::SpinExposure exposure;
    if (!BeamRoulette::ComputeSpinExposure(r.m_NumBets, legTypes, legNumbers, legAmounts, legMults, exposure))
        Env::Halt();

    // Solvency check: pool must cover true worst-case payout for this spin
    if (BeamRoulette::GetAvailableBalance() < exposure.m_MaxPayout) Env::Halt();

    // Lock user's total wager into the contract
    Env::FundsLock(r.m_AssetId, totalWagered);

    // Update state accounting
    spin.m_TotalWagered = totalWagered;
    spin.m_MaxPayout = exposure.m_MaxPayout;

    uint64_t newTotalBets = 0;
    uint64_t newPendingBets = 0;
    uint64_t newPendingMax = 0;
    if (!BeamRoulette::CheckedAdd(s.m_TotalBets, totalWagered, newTotalBets)) Env::Halt();
    if (!BeamRoulette::CheckedAdd(s.m_PendingBets, totalWagered, newPendingBets)) Env::Halt();
    if (!BeamRoulette::CheckedAdd(s.m_PendingMaxPayout, exposure.m_MaxPayout, newPendingMax)) Env::Halt();
    s.m_TotalBets = newTotalBets;
    s.m_PendingBets = newPendingBets;
    s.m_PendingMaxPayout = newPendingMax;

    // Save spin
    BeamRoulette::SpinKey sk;
    sk.m_SpinId = spin.m_SpinId;
    Env::SaveVar_T(sk, spin);

    // Auto-resolve up to 5 expired spins while we're here
    BeamRoulette::AutoResolveExpired(s, 5);
    BeamRoulette::AdvanceFirstUnresolved(s, 10);

    BeamRoulette::SaveState();
}

// ============================================================================
// Method 3: Check Results (User calls to reveal AND claim all their spins)
// ============================================================================
BEAM_EXPORT void Method_3(const BeamRoulette::Method::CheckResults& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();
    Height hCurrent = Env::get_Height();

    const PubKey& userPk = r.m_UserPk;

    uint64_t totalPayout = 0;
    AssetID assetId = s.m_AssetId;
    uint32_t processedCount = 0;
    uint32_t scanned = 0;
    static const uint32_t s_MaxScan = 500;

    // Scan from 1 (not FirstUnresolvedSpinId) so we find Won-but-unclaimed spins
    // that the cursor may have advanced past. FirstUnresolvedSpinId is only for
    // AutoResolveExpired's benefit — user-facing queries must find ALL claimable entries.
    for (uint64_t spinId = 1; spinId < s.m_NextSpinId && processedCount < 50 && scanned < s_MaxScan; spinId++) {
        scanned++;
        BeamRoulette::SpinKey sk;
        sk.m_SpinId = spinId;

        BeamRoulette::Spin spin;
        if (!Env::LoadVar_T(sk, spin)) continue;

        if (_POD_(spin.m_UserPk) != userPk) continue;

        if (spin.m_Status == BeamRoulette::SpinStatus::Pending) {
            if (hCurrent < spin.m_RevealAt) continue;

            uint8_t result = BeamRoulette::CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
            BeamRoulette::ProcessSpinResult(spin, result, s);
        }
        else if (spin.m_Status == BeamRoulette::SpinStatus::Won) {
            // Already revealed — just claim below
        }
        else {
            continue;
        }

        BeamRoulette::AccumulateClaim(spin, s, totalPayout);

        Env::SaveVar_T(sk, spin);
        processedCount++;
    }

    BeamRoulette::AdvanceFirstUnresolved(s);
    BeamRoulette::SaveState();

    // User must prove ownership
    Env::AddSig(r.m_UserPk);

    if (totalPayout > 0) {
        Env::FundsUnlock(assetId, totalPayout);
    }
}

// ============================================================================
// Method 4: Deposit (Owner only)
// ============================================================================
BEAM_EXPORT void Method_4(const BeamRoulette::Method::Deposit& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();
    if (r.m_Amount == 0) Env::Halt();

    Env::FundsLock(s.m_AssetId, r.m_Amount);
    s.m_TotalDeposited += r.m_Amount;
    BeamRoulette::SaveState();

    Env::AddSig(s.m_OwnerPk);
}

// ============================================================================
// Method 5: Withdraw (Owner only)
// ============================================================================
BEAM_EXPORT void Method_5(const BeamRoulette::Method::Withdraw& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();
    if (r.m_Amount == 0) Env::Halt();

    uint64_t avail = BeamRoulette::GetAvailableBalance();
    if (r.m_Amount > avail) Env::Halt();

    Env::FundsUnlock(s.m_AssetId, r.m_Amount);

    // Track cumulative withdrawals (total_deposited stays as cumulative deposits)
    s.m_TotalWithdrawn += r.m_Amount;

    BeamRoulette::SaveState();

    Env::AddSig(s.m_OwnerPk);
}

// ============================================================================
// Method 6: Set Owner (Owner only)
// ============================================================================
BEAM_EXPORT void Method_6(const BeamRoulette::Method::SetOwner& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();

    PubKey currentOwner = s.m_OwnerPk;
    _POD_(s.m_OwnerPk) = r.m_OwnerPk;
    BeamRoulette::SaveState();

    Env::AddSig(currentOwner);
}

// ============================================================================
// Method 7: Set Config (Owner only)
// ============================================================================
BEAM_EXPORT void Method_7(const BeamRoulette::Method::SetConfig& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();

    if (r.m_MinBet > 0) s.m_MinBet = r.m_MinBet;
    if (r.m_MaxBet > 0) s.m_MaxBet = r.m_MaxBet;
    if (r.m_StraightMult > 0) s.m_StraightMult = r.m_StraightMult;
    if (r.m_EvenMoneyMult > 0) s.m_EvenMoneyMult = r.m_EvenMoneyMult;
    if (r.m_DozenColMult > 0) s.m_DozenColMult = r.m_DozenColMult;
    if (r.m_RevealEpoch > 0) {
        if (r.m_RevealEpoch < BeamRoulette::s_MinRevealEpoch) Env::Halt();
        s.m_RevealEpoch = r.m_RevealEpoch;
    }
    s.m_Paused = r.m_Paused;

    // Min must not exceed max
    if (s.m_MinBet > s.m_MaxBet) Env::Halt();

    // Overflow protection: maxBet * highest multiplier must not overflow uint64_t
    if (s.m_MaxBet > 0) {
        if (s.m_StraightMult > 0 && s.m_MaxBet > static_cast<uint64_t>(-1) / s.m_StraightMult)
            Env::Halt();
        // EvenMoney and DozenCol are always lower than Straight, but check anyway
        if (s.m_EvenMoneyMult > 0 && s.m_MaxBet > static_cast<uint64_t>(-1) / s.m_EvenMoneyMult)
            Env::Halt();
        if (s.m_DozenColMult > 0 && s.m_MaxBet > static_cast<uint64_t>(-1) / s.m_DozenColMult)
            Env::Halt();
    }

    // Prevent changing asset while obligations exist
    if (r.m_AssetId != s.m_AssetId && (s.m_PendingBets > 0 || s.m_PendingPayouts > 0))
        Env::Halt();
    s.m_AssetId = r.m_AssetId;

    BeamRoulette::SaveState();

    Env::AddSig(s.m_OwnerPk);
}

// ============================================================================
// Method 8: Owner emergency reveal (deterministic — same formula as user reveal)
// ============================================================================
BEAM_EXPORT void Method_8(const BeamRoulette::Method::RevealSpin& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();

    BeamRoulette::SpinKey sk;
    sk.m_SpinId = r.m_SpinId;

    BeamRoulette::Spin spin;
    if (!Env::LoadVar_T(sk, spin)) Env::Halt();
    if (spin.m_Status != BeamRoulette::SpinStatus::Pending) Env::Halt();

    Height hCurrent = Env::get_Height();
    if (hCurrent < spin.m_RevealAt) Env::Halt();

    uint8_t result = BeamRoulette::CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
    BeamRoulette::ProcessSpinResult(spin, result, s);
    // Note: does NOT claim — user must call check_results to receive winnings

    Env::SaveVar_T(sk, spin);
    BeamRoulette::SaveState();

    Env::AddSig(s.m_OwnerPk);
}

// ============================================================================
// Method 9: Check Single Spin (User calls to reveal AND claim one spin by ID)
// ============================================================================
BEAM_EXPORT void Method_9(const BeamRoulette::Method::CheckSingleSpin& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();
    Height hCurrent = Env::get_Height();

    BeamRoulette::SpinKey sk;
    sk.m_SpinId = r.m_SpinId;

    BeamRoulette::Spin spin;
    if (!Env::LoadVar_T(sk, spin)) Env::Halt();

    // Only the spin owner can claim
    if (_POD_(spin.m_UserPk) != r.m_UserPk) Env::Halt();

    AssetID assetId = s.m_AssetId;

    if (spin.m_Status == BeamRoulette::SpinStatus::Pending) {
        if (hCurrent < spin.m_RevealAt) Env::Halt();

        uint8_t result = BeamRoulette::CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
        BeamRoulette::ProcessSpinResult(spin, result, s);
    }
    else if (spin.m_Status == BeamRoulette::SpinStatus::Won) {
        // Already revealed — just claim
    }
    else {
        Env::Halt(); // Lost or already Claimed
    }

    uint64_t totalPayout = 0;
    BeamRoulette::AccumulateClaim(spin, s, totalPayout);

    Env::SaveVar_T(sk, spin);
    BeamRoulette::AdvanceFirstUnresolved(s, 10);
    BeamRoulette::SaveState();

    Env::AddSig(r.m_UserPk);

    if (totalPayout > 0) {
        Env::FundsUnlock(assetId, totalPayout);
    }
}

// ============================================================================
// Method 10: Resolve Expired Spins (Anyone can call — reveals results, no payouts)
// ============================================================================
BEAM_EXPORT void Method_10(const BeamRoulette::Method::ResolveExpiredSpins& r)
{
    BeamRoulette::State& s = BeamRoulette::GetState();
    Height hCurrent = Env::get_Height();

    uint32_t maxCount = r.m_MaxCount;
    if (maxCount == 0 || maxCount > 200) maxCount = 50;

    uint32_t resolved = 0;
    uint32_t scanned = 0;
    uint32_t maxScan = maxCount + 50; // Allow scanning past non-Pending entries

    for (uint64_t spinId = s.m_FirstUnresolvedSpinId; spinId < s.m_NextSpinId && resolved < maxCount && scanned < maxScan; spinId++) {
        scanned++;
        BeamRoulette::SpinKey sk;
        sk.m_SpinId = spinId;

        BeamRoulette::Spin spin;
        if (!Env::LoadVar_T(sk, spin)) continue;
        if (spin.m_Status != BeamRoulette::SpinStatus::Pending) continue;
        if (hCurrent < spin.m_RevealAt) break; // Sequential — if this isn't expired, later ones can't be

        uint8_t result = BeamRoulette::CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
        BeamRoulette::ProcessSpinResult(spin, result, s);

        Env::SaveVar_T(sk, spin);
        resolved++;
    }

    BeamRoulette::AdvanceFirstUnresolved(s);
    BeamRoulette::SaveState();
}
