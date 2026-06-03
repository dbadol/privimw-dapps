// PriviBets Roulette App Shader
// Client-side shader: schema, views, TX handlers, preview logic
#include "common.h"
#include "app_common_impl.h"
#include "roulette.h"
#include "roulette_calc.h"

void OnError(const char* sz)
{
    Env::DocGroup root("");
    Env::DocAddText("error", sz);
}

// Owner key derivation (different seed from d100)
struct OwnerKey {
    ShaderID m_SID;
    uint8_t m_pSeed[16];

    OwnerKey() {
        _POD_(m_SID) = BeamRoulette::s_SID;
        const char szSeed[] = "roulette-owner-k";  // 16 bytes — different from d100's "beambet-owner-ke"
        Env::Memcpy(m_pSeed, szSeed, sizeof(m_pSeed));
    }

    void DerivePk(PubKey& pk) const {
        Env::DerivePk(pk, this, sizeof(*this));
    }
};

// User key derivation (derived from CID — naturally different from d100)
struct UserKey {
    ContractID m_Cid;
    uint8_t m_Tag;

    void DerivePk(PubKey& pk) const {
        Env::DerivePk(pk, this, sizeof(*this));
    }
};

// Client-side spin result calculation (same formula as contract — no get_HdrInfo)
// Uses stored placement hash from Spin struct + reveal height + spin ID
static uint8_t CalculateSpinResult(const HashValue& placementHash, Height revealAt, uint64_t spinId) {
    HashProcessor::Sha256 hp;
    hp.Write(placementHash);
    hp.Write(&revealAt, sizeof(revealAt));
    hp.Write(&spinId, sizeof(spinId));

    HashValue resultHash;
    hp >> resultHash;

    uint16_t rawValue = (static_cast<uint16_t>(resultHash.m_p[0]) << 8) | resultHash.m_p[1];
    return rawValue % 38;
}

static uint64_t GetMultiplierForType(const BeamRoulette::State& s, uint8_t betType) {
    switch (betType) {
        case BeamRoulette::BetType::Straight: return s.m_StraightMult;
        case BeamRoulette::BetType::Red:
        case BeamRoulette::BetType::Black:
        case BeamRoulette::BetType::Odd:
        case BeamRoulette::BetType::Even:
        case BeamRoulette::BetType::Low:
        case BeamRoulette::BetType::High:    return s.m_EvenMoneyMult;
        case BeamRoulette::BetType::Dozen1:
        case BeamRoulette::BetType::Dozen2:
        case BeamRoulette::BetType::Dozen3:
        case BeamRoulette::BetType::Column1:
        case BeamRoulette::BetType::Column2:
        case BeamRoulette::BetType::Column3: return s.m_DozenColMult;
        default: return 0;
    }
}

static uint64_t MirrorAvailableBalance(const BeamRoulette::State& s) {
    uint64_t total = s.m_TotalDeposited + s.m_TotalBets;
    if (s.m_TotalPayouts > total) return 0;
    total -= s.m_TotalPayouts;
    if (s.m_TotalWithdrawn > total) return 0;
    total -= s.m_TotalWithdrawn;
    uint64_t reserved = s.m_PendingMaxPayout + s.m_PendingPayouts;
    if (reserved > total) return 0;
    return total - reserved;
}

// Simple string helpers for kernel descriptions
static void StrCopy(char* dst, uint32_t dstSize, const char* src) {
    uint32_t i = 0;
    while (src[i] && i < dstSize - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void AppendStr(char* dst, uint32_t dstSize, const char* src) {
    uint32_t d = 0;
    while (dst[d] && d < dstSize) d++;
    uint32_t i = 0;
    while (src[i] && d < dstSize - 1) { dst[d++] = src[i++]; }
    dst[d] = 0;
}

static void UIntToStr(uint32_t val, char* buf, uint32_t maxLen) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char temp[16];
    int i = 0;
    while (val > 0 && i < 15) { temp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0 && j < maxLen - 1) { buf[j++] = temp[--i]; }
    buf[j] = 0;
}

static void U64ToStr(uint64_t val, char* buf, uint32_t maxLen) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char temp[24];
    int i = 0;
    while (val > 0 && i < 23) { temp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0 && j < maxLen - 1) { buf[j++] = temp[--i]; }
    buf[j] = 0;
}

// Compact spin history entry (used by view_all)
struct SpinHistSlot {
    uint64_t spinId, totalWagered, totalPayout, createdHeight;
    uint32_t numBets, result, status;
    uint8_t betTypes[BeamRoulette::s_MaxBetsPerSpin];
    uint8_t betNumbers[BeamRoulette::s_MaxBetsPerSpin];
    uint8_t betWon[BeamRoulette::s_MaxBetsPerSpin];
};

// Compact unclaimed spin entry
struct UnclaimedSlot {
    uint64_t spinId, totalWagered, totalPayout;
    uint32_t numBets, result;
    // Per-bet details stored separately if needed
};
static const uint32_t MAX_UNCLAIMED = 100;

// ============================================================================
// Schema: Method_0
// ============================================================================
BEAM_EXPORT void Method_0()
{
    Env::DocGroup root("");
    {
        Env::DocGroup gr("roles");
        {
            Env::DocGroup grRole("manager");
            {
                Env::DocGroup grMethod("create_contract");
            }
            {
                Env::DocGroup grMethod("view_contracts");
            }
            {
                Env::DocGroup grMethod("view_pool");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("view_all_spins");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("reveal_spin");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("spin_id", "uint64");
            }
            {
                Env::DocGroup grMethod("resolve_spins");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("count", "uint32");
            }
            {
                Env::DocGroup grMethod("deposit");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("amount", "uint64");
            }
            {
                Env::DocGroup grMethod("withdraw");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("amount", "uint64");
            }
            {
                Env::DocGroup grMethod("set_owner");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("owner_pk", "PubKey");
            }
            {
                Env::DocGroup grMethod("set_config");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("min_bet", "uint64");
                Env::DocAddText("max_bet", "uint64");
                Env::DocAddText("straight_mult", "uint64");
                Env::DocAddText("even_money_mult", "uint64");
                Env::DocAddText("dozen_col_mult", "uint64");
                Env::DocAddText("reveal_epoch", "uint64");
                Env::DocAddText("paused", "uint32");
                Env::DocAddText("asset_id", "AssetID");
            }
        }
        {
            Env::DocGroup grRole("user");
            {
                Env::DocGroup grMethod("view_params");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("place_bets");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("asset_id", "AssetID");
                Env::DocAddText("num_bets", "uint32");
                Env::DocAddText("types", "string");      // comma-separated: "1,0,7"
                Env::DocAddText("numbers", "string");     // comma-separated: "0,17,0"
                Env::DocAddText("amounts", "string");     // comma-separated: "100000000,200000000,500000000"
            }
            {
                Env::DocGroup grMethod("preview_place_bets");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("num_bets", "uint32");
                Env::DocAddText("types", "string");
                Env::DocAddText("numbers", "string");
                Env::DocAddText("amounts", "string");
            }
            {
                Env::DocGroup grMethod("check_results");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("check_single");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("spin_id", "uint64");
            }
            {
                Env::DocGroup grMethod("view_user_pk");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("my_spins");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("view_all");
                Env::DocAddText("cid", "ContractID");
            }
            {
                Env::DocGroup grMethod("view_recent_results");
                Env::DocAddText("cid", "ContractID");
                Env::DocAddText("count", "uint32");
            }
        }
    }
}

// ============================================================================
// Helper: Output bet positions for a spin
// ============================================================================
static void OutputBetPositions(const BeamRoulette::Spin& spin) {
    Env::DocArray betsArr("bets");
    for (uint8_t i = 0; i < spin.m_NumBets; i++) {
        const BeamRoulette::BetPosition& bp = spin.m_Bets[i];
        Env::DocGroup betGr("");
        Env::DocAddNum("type", (uint32_t)bp.m_Type);
        Env::DocAddNum("number", (uint32_t)bp.m_Number);
        Env::DocAddNum("amount", bp.m_Amount);
        Env::DocAddNum("multiplier", bp.m_Multiplier);
        Env::DocAddNum("payout", bp.m_Payout);
        Env::DocAddNum("won", (uint32_t)bp.m_Won);
    }
}

// ============================================================================
// Action Handlers
// ============================================================================

void On_create_contract(const ContractID& cid)
{
    (void)cid;
    BeamRoulette::Params params;
    OwnerKey ok;
    ok.DerivePk(params.m_OwnerPk);

    Env::GenerateKernel(nullptr, 0, &params, sizeof(params), nullptr, 0, nullptr, 0, "create Roulette contract", 0);
}

void On_view_contracts(const ContractID& cid)
{
    (void)cid;
    Env::DocArray gr("contracts");

    WalkerContracts wlk;
    for (wlk.Enum(BeamRoulette::s_SID); wlk.MoveNext(); )
    {
        Env::DocGroup root("");
        Env::DocAddBlob_T("cid", wlk.m_Key.m_KeyInContract.m_Cid);
        Env::DocAddNum("height", wlk.m_Height);
    }
}

void On_view_pool(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> k;
    k.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(k, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    uint64_t total = s.m_TotalDeposited + s.m_TotalBets;
    if (s.m_TotalPayouts > total) total = 0;
    else total -= s.m_TotalPayouts;
    if (s.m_TotalWithdrawn > total) total = 0;
    else total -= s.m_TotalWithdrawn;
    uint64_t reserved = s.m_PendingMaxPayout + s.m_PendingPayouts;
    uint64_t available = (reserved <= total) ? (total - reserved) : 0;

    Env::DocGroup gr("pool");
    Env::DocAddNum("total_deposited", s.m_TotalDeposited);
    Env::DocAddNum("total_withdrawn", s.m_TotalWithdrawn);
    Env::DocAddNum("total_bets", s.m_TotalBets);
    Env::DocAddNum("total_payouts", s.m_TotalPayouts);
    Env::DocAddNum("pending_bets", s.m_PendingBets);
    Env::DocAddNum("pending_max_payout", s.m_PendingMaxPayout);
    Env::DocAddNum("pending_payouts", s.m_PendingPayouts);
    Env::DocAddNum("available_balance", available);
    Env::DocAddNum("asset_id", s.m_AssetId);
    Env::DocAddNum("paused", (uint32_t)s.m_Paused);
}

void On_view_params(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> k;
    k.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(k, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    Env::DocGroup gr("params");
    Env::DocAddNum("min_bet", s.m_MinBet);
    Env::DocAddNum("max_bet", s.m_MaxBet);
    Env::DocAddNum("straight_mult", s.m_StraightMult);
    Env::DocAddNum("even_money_mult", s.m_EvenMoneyMult);
    Env::DocAddNum("dozen_col_mult", s.m_DozenColMult);
    Env::DocAddNum("reveal_epoch", s.m_RevealEpoch);
    Env::DocAddNum("paused", (uint32_t)s.m_Paused);
    Env::DocAddNum("asset_id", s.m_AssetId);
}

// Parse comma-separated uint values from a string into an array
// Parse semicolon-separated arrays (semicolons used because commas conflict with --shader_args delimiter)
static uint32_t ParseSSV_u8(const char* str, uint8_t* out, uint32_t maxCount) {
    uint32_t count = 0;
    uint32_t val = 0;
    bool hasVal = false;
    for (const char* p = str; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            hasVal = true;
        } else {
            if (hasVal && count < maxCount) {
                out[count++] = (uint8_t)val;
                val = 0;
                hasVal = false;
            }
            if (*p == 0) break;
        }
    }
    return count;
}

static uint32_t ParseSSV_u64(const char* str, uint64_t* out, uint32_t maxCount) {
    uint32_t count = 0;
    uint64_t val = 0;
    bool hasVal = false;
    for (const char* p = str; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            hasVal = true;
        } else {
            if (hasVal && count < maxCount) {
                out[count++] = val;
                val = 0;
                hasVal = false;
            }
            if (*p == 0) break;
        }
    }
    return count;
}

void On_place_bets(const ContractID& cid)
{
    BeamRoulette::Method::PlaceBets args;
    _POD_(args).SetZero();

    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    uk.DerivePk(args.m_UserPk);

    Env::DocGet("asset_id", args.m_AssetId);

    uint32_t numBets = 0;
    Env::DocGet("num_bets", numBets);
    if (numBets < 1 || numBets > BeamRoulette::s_MaxBetsPerSpin) {
        OnError("num_bets out of range");
        return;
    }
    args.m_NumBets = (uint8_t)numBets;

    // Parse comma-separated arrays
    char typesStr[128] = {0};
    char numbersStr[128] = {0};
    char amountsStr[512] = {0};
    Env::DocGetText("types", typesStr, sizeof(typesStr));
    Env::DocGetText("numbers", numbersStr, sizeof(numbersStr));
    Env::DocGetText("amounts", amountsStr, sizeof(amountsStr));

    uint8_t parsedTypes[BeamRoulette::s_MaxBetsPerSpin];
    uint8_t parsedNumbers[BeamRoulette::s_MaxBetsPerSpin];
    uint64_t parsedAmounts[BeamRoulette::s_MaxBetsPerSpin];
    uint32_t nTypes = ParseSSV_u8(typesStr, parsedTypes, BeamRoulette::s_MaxBetsPerSpin);
    uint32_t nNumbers = ParseSSV_u8(numbersStr, parsedNumbers, BeamRoulette::s_MaxBetsPerSpin);
    uint32_t nAmounts = ParseSSV_u64(amountsStr, parsedAmounts, BeamRoulette::s_MaxBetsPerSpin);

    if (nTypes < numBets || nNumbers < numBets || nAmounts < numBets) {
        OnError("Array size mismatch");
        return;
    }

    uint64_t totalAmount = 0;
    for (uint32_t i = 0; i < numBets; i++) {
        args.m_Types[i] = parsedTypes[i];
        args.m_Numbers[i] = parsedNumbers[i];
        args.m_Amounts[i] = parsedAmounts[i];
        totalAmount += parsedAmounts[i];
    }

    FundsChange fc;
    fc.m_Aid = args.m_AssetId;
    fc.m_Amount = totalAmount;
    fc.m_Consume = 1;

    // nCharge: state load/save + spin save + FundsLock + get_Height/get_HdrInfo + per-position validation
    // + AutoResolveExpired (scan up to 20, resolve up to 5, Spin=~436 bytes) + AdvanceFirstUnresolved (10 loads)
    uint32_t nCharge = 1000000 + numBets * 30000;

    Env::GenerateKernel(&cid, BeamRoulette::Method::PlaceBets::s_iMethod, &args, sizeof(args), &fc, 1, nullptr, 0, "Roulette: place bets", nCharge);
}

// Preflight: exact max exposure + per-pocket payout grid (same math as contract placement)
void On_preview_place_bets(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    uint32_t numBets = 0;
    Env::DocGet("num_bets", numBets);
    if (numBets < 1 || numBets > BeamRoulette::s_MaxBetsPerSpin) {
        OnError("num_bets out of range");
        return;
    }

    char typesStr[128] = {0};
    char numbersStr[128] = {0};
    char amountsStr[512] = {0};
    Env::DocGetText("types", typesStr, sizeof(typesStr));
    Env::DocGetText("numbers", numbersStr, sizeof(numbersStr));
    Env::DocGetText("amounts", amountsStr, sizeof(amountsStr));

    uint8_t legTypes[BeamRoulette::s_MaxBetsPerSpin];
    uint8_t legNumbers[BeamRoulette::s_MaxBetsPerSpin];
    uint64_t legAmounts[BeamRoulette::s_MaxBetsPerSpin];
    uint64_t legMults[BeamRoulette::s_MaxBetsPerSpin];
    uint32_t nTypes = ParseSSV_u8(typesStr, legTypes, BeamRoulette::s_MaxBetsPerSpin);
    uint32_t nNumbers = ParseSSV_u8(numbersStr, legNumbers, BeamRoulette::s_MaxBetsPerSpin);
    uint32_t nAmounts = ParseSSV_u64(amountsStr, legAmounts, BeamRoulette::s_MaxBetsPerSpin);

    if (nTypes < numBets || nNumbers < numBets || nAmounts < numBets) {
        OnError("Array size mismatch");
        return;
    }

    uint64_t totalWagered = 0;
    uint64_t summedLegMax = 0;
    for (uint32_t i = 0; i < numBets; i++) {
        uint8_t betType = legTypes[i];
        if (betType > 12) {
            OnError("Invalid bet type");
            return;
        }
        if (betType == BeamRoulette::BetType::Straight && legNumbers[i] > 37) {
            OnError("Invalid straight number");
            return;
        }
        if (legAmounts[i] < s.m_MinBet || legAmounts[i] > s.m_MaxBet) {
            OnError("Bet amount out of limits");
            return;
        }
        uint64_t mult = GetMultiplierForType(s, betType);
        if (mult == 0) {
            OnError("Invalid multiplier");
            return;
        }
        legMults[i] = mult;
        if (!BeamRoulette::CheckedAdd(totalWagered, legAmounts[i], totalWagered)) {
            OnError("Wager overflow");
            return;
        }
    }

    if (!BeamRoulette::ComputeSummedLegMaxPayout((uint8_t)numBets, legAmounts, legMults, summedLegMax)) {
        OnError("Payout overflow");
        return;
    }

    BeamRoulette::SpinExposure exposure;
    if (!BeamRoulette::ComputeSpinExposure((uint8_t)numBets, legTypes, legNumbers, legAmounts, legMults, exposure)) {
        OnError("Exposure overflow");
        return;
    }

    uint64_t available = MirrorAvailableBalance(s);

    Env::DocGroup gr("preview");
    Env::DocAddNum("total_wagered", totalWagered);
    Env::DocAddNum("max_payout", exposure.m_MaxPayout);
    Env::DocAddNum("summed_leg_max_payout", summedLegMax);
    Env::DocAddNum("available_balance", available);
    Env::DocAddNum("solvency_ok", (uint32_t)(available >= exposure.m_MaxPayout ? 1 : 0));

    {
        Env::DocArray arr("payout_by_result");
        for (uint32_t r = 0; r < BeamRoulette::s_WheelOutcomes; r++) {
            Env::DocGroup pocket("");
            Env::DocAddNum("result", r);
            Env::DocAddNum("payout", exposure.m_PayoutByResult[r]);
        }
    }
}

// Claim all claimable spins by generating one CheckSingleSpin kernel per spin.
// BVM cost is O(claims), not O(total contract size), because each kernel loads one spin by ID.
void On_check_results(const ContractID& cid)
{
    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    PubKey userPk;
    uk.DerivePk(userPk);

    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    Height hCurrent = Env::get_Height();

    // Client-side scan: find all claimable spins for this user (free, not on-chain).
    static const uint32_t s_MaxScan = 500;
    static const uint32_t s_MaxClaims = 50;
    uint64_t claimableIds[50];
    uint64_t payouts[50];
    uint32_t claimableCount = 0;

    if (s.m_NextSpinId > 1)
    {
        Env::Key_T<BeamRoulette::SpinKey> k0, k1;
        k0.m_Prefix.m_Cid = cid;
        k0.m_KeyInContract.m_SpinId = 1;
        k1.m_Prefix.m_Cid = cid;
        k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

        Env::VarReader scanner(k0, k1);
        Env::Key_T<BeamRoulette::SpinKey> key;
        BeamRoulette::Spin spin;
        uint32_t scanned = 0;
        while (scanner.MoveNext_T(key, spin) && claimableCount < s_MaxClaims && scanned < s_MaxScan)
        {
            scanned++;
            if (_POD_(spin.m_UserPk) != userPk) continue;

            if (spin.m_Status == BeamRoulette::SpinStatus::Pending)
            {
                if (hCurrent < spin.m_RevealAt) continue;
            }
            else if (spin.m_Status == BeamRoulette::SpinStatus::Won)
            {
                // Already revealed, just needs claim
            }
            else
            {
                continue; // Lost or already Claimed
            }

            uint64_t payout = 0;
            if (spin.m_Status == BeamRoulette::SpinStatus::Pending)
            {
                uint8_t result = CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
                for (uint8_t i = 0; i < spin.m_NumBets; i++) {
                    const BeamRoulette::BetPosition& bp = spin.m_Bets[i];
                    if (BeamRoulette::IsBetWon(bp.m_Type, bp.m_Number, result)) {
                        payout += (bp.m_Amount * bp.m_Multiplier) / 100;
                    }
                }
            }
            else
            {
                payout = spin.m_TotalPayout;
            }

            claimableIds[claimableCount] = spin.m_SpinId;
            payouts[claimableCount] = payout;
            claimableCount++;
        }
    }

    if (claimableCount == 0)
    {
        OnError("No spins to claim");
        return;
    }

    Env::KeyID kid(&uk, sizeof(uk));

    // One kernel per spin using Method_9 (CheckSingleSpin). BVM cost is O(1) per kernel.
    for (uint32_t i = 0; i < claimableCount; i++)
    {
        BeamRoulette::Method::CheckSingleSpin args;
        args.m_SpinId = claimableIds[i];
        _POD_(args.m_UserPk) = userPk;

        // Build per-spin description so wallet shows unique text per kernel
        char idStr[24];
        U64ToStr(claimableIds[i], idStr, sizeof(idStr));
        char desc[64] = {0};
        StrCopy(desc, sizeof(desc), "Roulette: claim spin #");
        AppendStr(desc, sizeof(desc), idStr);

        if (payouts[i] > 0)
        {
            FundsChange fc;
            fc.m_Aid = s.m_AssetId;
            fc.m_Amount = payouts[i];
            fc.m_Consume = 0;
            Env::GenerateKernel(&cid, BeamRoulette::Method::CheckSingleSpin::s_iMethod, &args, sizeof(args), &fc, 1, &kid, 1, desc, 500000);
        }
        else
        {
            Env::GenerateKernel(&cid, BeamRoulette::Method::CheckSingleSpin::s_iMethod, &args, sizeof(args), nullptr, 0, &kid, 1, desc, 500000);
        }
    }
}

void On_check_single(const ContractID& cid)
{
    BeamRoulette::Method::CheckSingleSpin args;
    Env::DocGet("spin_id", args.m_SpinId);

    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    uk.DerivePk(args.m_UserPk);

    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    Env::Key_T<BeamRoulette::SpinKey> spinKey;
    spinKey.m_Prefix.m_Cid = cid;
    spinKey.m_KeyInContract.m_SpinId = args.m_SpinId;

    BeamRoulette::Spin spin;
    if (!Env::VarReader::Read_T(spinKey, spin))
    {
        OnError("Spin not found");
        return;
    }

    if (_POD_(spin.m_UserPk) != args.m_UserPk)
    {
        OnError("Not your spin");
        return;
    }

    uint64_t payout = 0;
    Height hCurrent = Env::get_Height();

    if (spin.m_Status == BeamRoulette::SpinStatus::Pending)
    {
        if (hCurrent < spin.m_RevealAt)
        {
            OnError("Spin not ready for reveal yet");
            return;
        }

        uint8_t result = CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
        for (uint8_t i = 0; i < spin.m_NumBets; i++) {
            const BeamRoulette::BetPosition& bp = spin.m_Bets[i];
            if (BeamRoulette::IsBetWon(bp.m_Type, bp.m_Number, result)) {
                payout += (bp.m_Amount * bp.m_Multiplier) / 100;
            }
        }
    }
    else if (spin.m_Status == BeamRoulette::SpinStatus::Won && spin.m_TotalPayout > 0)
    {
        payout = spin.m_TotalPayout;
    }
    else
    {
        OnError("Spin is not pending or won");
        return;
    }

    Env::KeyID kid(&uk, sizeof(uk));

    if (payout > 0)
    {
        FundsChange fc;
        fc.m_Aid = s.m_AssetId;
        fc.m_Amount = payout;
        fc.m_Consume = 0;
        Env::GenerateKernel(&cid, BeamRoulette::Method::CheckSingleSpin::s_iMethod, &args, sizeof(args), &fc, 1, &kid, 1, "Roulette: check single spin", 450000);
    }
    else
    {
        Env::GenerateKernel(&cid, BeamRoulette::Method::CheckSingleSpin::s_iMethod, &args, sizeof(args), nullptr, 0, &kid, 1, "Roulette: check single spin", 450000);
    }
}

void On_view_user_pk(const ContractID& cid)
{
    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    PubKey userPk;
    uk.DerivePk(userPk);
    Env::DocAddBlob_T("user_pk", userPk);
}

void On_my_spins(const ContractID& cid)
{
    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    PubKey userPk;
    uk.DerivePk(userPk);

    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    Height currentHeight = Env::get_Height();

    Env::DocArray gr("spins");

    if (s.m_FirstUnresolvedSpinId < s.m_NextSpinId)
    {
        Env::Key_T<BeamRoulette::SpinKey> k0, k1;
        k0.m_Prefix.m_Cid = cid;
        k0.m_KeyInContract.m_SpinId = s.m_FirstUnresolvedSpinId;
        k1.m_Prefix.m_Cid = cid;
        k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

        Env::VarReader scanner(k0, k1);
        Env::Key_T<BeamRoulette::SpinKey> key;
        BeamRoulette::Spin spin;
        while (scanner.MoveNext_T(key, spin))
        {
            if (_POD_(spin.m_UserPk) != userPk) continue;
            if (spin.m_Status != BeamRoulette::SpinStatus::Pending) continue;

            uint64_t blocksRemaining = 0;
            if (spin.m_RevealAt > currentHeight)
                blocksRemaining = spin.m_RevealAt - currentHeight;

            Env::DocGroup spinGr("");
            Env::DocAddNum("spin_id", spin.m_SpinId);
            Env::DocAddNum("num_bets", (uint32_t)spin.m_NumBets);
            Env::DocAddNum("total_wagered", spin.m_TotalWagered);
            Env::DocAddNum("max_payout", spin.m_MaxPayout);
            Env::DocAddNum("created_height", spin.m_CreatedHeight);
            Env::DocAddNum("reveal_at", spin.m_RevealAt);
            Env::DocAddNum("blocks_remaining", blocksRemaining);
            Env::DocAddNum("can_reveal", (uint32_t)(blocksRemaining == 0 ? 1 : 0));

            // Output bet positions
            OutputBetPositions(spin);

            // Preview result for ready spins
            if (blocksRemaining == 0) {
                uint8_t result = CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
                Env::DocAddNum("preview_result", (uint32_t)result);

                uint64_t previewPayout = 0;
                uint32_t winsCount = 0;
                {
                    Env::DocArray prevArr("preview_bets");
                    for (uint8_t i = 0; i < spin.m_NumBets; i++) {
                        const BeamRoulette::BetPosition& bp = spin.m_Bets[i];
                        bool won = BeamRoulette::IsBetWon(bp.m_Type, bp.m_Number, result);
                        uint64_t payout = won ? (bp.m_Amount * bp.m_Multiplier) / 100 : 0;
                        if (won) { previewPayout += payout; winsCount++; }

                        Env::DocGroup bpGr("");
                        Env::DocAddNum("won", (uint32_t)(won ? 1 : 0));
                        Env::DocAddNum("payout", payout);
                    }
                }
                Env::DocAddNum("preview_total_payout", previewPayout);
                Env::DocAddNum("preview_wins_count", winsCount);
            }
        }
    }
}

// Combined view: params + pool + pending spins + unclaimed + history + recent results
void On_view_all(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    // === PARAMS ===
    {
        Env::DocGroup gr("params");
        Env::DocAddNum("min_bet", s.m_MinBet);
        Env::DocAddNum("max_bet", s.m_MaxBet);
        Env::DocAddNum("straight_mult", s.m_StraightMult);
        Env::DocAddNum("even_money_mult", s.m_EvenMoneyMult);
        Env::DocAddNum("dozen_col_mult", s.m_DozenColMult);
        Env::DocAddNum("reveal_epoch", s.m_RevealEpoch);
        Env::DocAddNum("paused", (uint32_t)s.m_Paused);
        Env::DocAddNum("asset_id", s.m_AssetId);
    }

    // === POOL ===
    {
        uint64_t total = s.m_TotalDeposited + s.m_TotalBets;
        if (s.m_TotalPayouts > total) total = 0;
        else total -= s.m_TotalPayouts;
        if (s.m_TotalWithdrawn > total) total = 0;
        else total -= s.m_TotalWithdrawn;
        uint64_t reserved = s.m_PendingMaxPayout + s.m_PendingPayouts;
        uint64_t available = (reserved <= total) ? (total - reserved) : 0;

        Env::DocGroup gr("pool");
        Env::DocAddNum("total_deposited", s.m_TotalDeposited);
        Env::DocAddNum("total_withdrawn", s.m_TotalWithdrawn);
        Env::DocAddNum("total_bets", s.m_TotalBets);
        Env::DocAddNum("total_payouts", s.m_TotalPayouts);
        Env::DocAddNum("pending_bets", s.m_PendingBets);
        Env::DocAddNum("pending_max_payout", s.m_PendingMaxPayout);
        Env::DocAddNum("pending_payouts", s.m_PendingPayouts);
        Env::DocAddNum("available_balance", available);
        Env::DocAddNum("asset_id", s.m_AssetId);
        Env::DocAddNum("paused", (uint32_t)s.m_Paused);
    }

    // Derive user PK once
    UserKey uk;
    _POD_(uk).SetZero();
    uk.m_Cid = cid;
    PubKey userPk;
    uk.DerivePk(userPk);

    Height currentHeight = Env::get_Height();

    // Allocate history and unclaimed buffers
    // History: no cap, sized by total spin count (upper bound for user's spins)
    uint32_t maxHistSlots = (s.m_NextSpinId > 0) ? (uint32_t)s.m_NextSpinId : 1;
    SpinHistSlot* histBuf = (SpinHistSlot*) Env::Heap_Alloc(sizeof(SpinHistSlot) * maxHistSlots);
    uint32_t histCount = 0;

    // For unclaimed — store spin IDs, wagered, payout (details come from re-reading)
    uint64_t* unclaimedIds = (uint64_t*) Env::Heap_Alloc(sizeof(uint64_t) * MAX_UNCLAIMED);
    uint32_t unclaimedCount = 0;

    // === PENDING SPINS (output directly) ===
    {
        Env::DocArray gr("pending");
        if (s.m_NextSpinId > 0)
        {
            // Scan from 1 for full user history
            uint64_t startId = 1;
            if (startId < 1) startId = 1;

            Env::Key_T<BeamRoulette::SpinKey> k0, k1;
            k0.m_Prefix.m_Cid = cid;
            k0.m_KeyInContract.m_SpinId = startId;
            k1.m_Prefix.m_Cid = cid;
            k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

            Env::VarReader scanner(k0, k1);
            Env::Key_T<BeamRoulette::SpinKey> key;
            BeamRoulette::Spin spin;
            while (scanner.MoveNext_T(key, spin))
            {
                if (_POD_(spin.m_UserPk) != userPk) continue;

                if (spin.m_Status == BeamRoulette::SpinStatus::Pending)
                {
                    uint64_t blocksRemaining = 0;
                    if (spin.m_RevealAt > currentHeight)
                        blocksRemaining = spin.m_RevealAt - currentHeight;

                    Env::DocGroup spinGr("");
                    Env::DocAddNum("spin_id", spin.m_SpinId);
                    Env::DocAddNum("num_bets", (uint32_t)spin.m_NumBets);
                    Env::DocAddNum("total_wagered", spin.m_TotalWagered);
                    Env::DocAddNum("max_payout", spin.m_MaxPayout);
                    Env::DocAddNum("created_height", spin.m_CreatedHeight);
                    Env::DocAddNum("reveal_at", spin.m_RevealAt);
                    Env::DocAddNum("blocks_remaining", blocksRemaining);
                    Env::DocAddNum("can_reveal", (uint32_t)(blocksRemaining == 0 ? 1 : 0));

                    OutputBetPositions(spin);

                    // Preview result for ready spins
                    if (blocksRemaining == 0) {
                        uint8_t result = CalculateSpinResult(spin.m_PlacementHash, spin.m_RevealAt, spin.m_SpinId);
                        Env::DocAddNum("preview_result", (uint32_t)result);

                        uint64_t previewPayout = 0;
                        uint32_t winsCount = 0;
                        {
                            Env::DocArray prevArr("preview_bets");
                            for (uint8_t i = 0; i < spin.m_NumBets; i++) {
                                const BeamRoulette::BetPosition& bp = spin.m_Bets[i];
                                bool won = BeamRoulette::IsBetWon(bp.m_Type, bp.m_Number, result);
                                uint64_t payout = won ? (bp.m_Amount * bp.m_Multiplier) / 100 : 0;
                                if (won) { previewPayout += payout; winsCount++; }

                                Env::DocGroup bpGr("");
                                Env::DocAddNum("won", (uint32_t)(won ? 1 : 0));
                                Env::DocAddNum("payout", payout);
                            }
                        }
                        Env::DocAddNum("preview_total_payout", previewPayout);
                        Env::DocAddNum("preview_wins_count", winsCount);
                    }
                }
                else if (spin.m_Status == BeamRoulette::SpinStatus::Won)
                {
                    // Buffer unclaimed spin ID
                    if (unclaimedCount < MAX_UNCLAIMED)
                        unclaimedIds[unclaimedCount++] = spin.m_SpinId;
                }
                else
                {
                    // Lost/Claimed — add to history buffer (no cap)
                    SpinHistSlot& h = histBuf[histCount++];
                    h.spinId = spin.m_SpinId;
                    h.totalWagered = spin.m_TotalWagered;
                    h.totalPayout = spin.m_TotalPayout;
                    h.createdHeight = spin.m_CreatedHeight;
                    h.numBets = (uint32_t)spin.m_NumBets;
                    h.result = (uint32_t)spin.m_Result;
                    h.status = (uint32_t)spin.m_Status;
                    // Copy per-bet details for history display
                    for (uint8_t bi = 0; bi < BeamRoulette::s_MaxBetsPerSpin; bi++) {
                        if (bi < spin.m_NumBets) {
                            h.betTypes[bi] = spin.m_Bets[bi].m_Type;
                            h.betNumbers[bi] = spin.m_Bets[bi].m_Number;
                            h.betWon[bi] = spin.m_Bets[bi].m_Won;
                        } else {
                            h.betTypes[bi] = 0;
                            h.betNumbers[bi] = 0;
                            h.betWon[bi] = 0;
                        }
                    }
                }
            }
        }
    }

    // === UNCLAIMED WINS — re-read and output with full bet details ===
    {
        Env::DocArray gr("unclaimed");
        for (uint32_t i = 0; i < unclaimedCount; i++)
        {
            Env::Key_T<BeamRoulette::SpinKey> rk;
            rk.m_Prefix.m_Cid = cid;
            rk.m_KeyInContract.m_SpinId = unclaimedIds[i];

            BeamRoulette::Spin spin;
            if (!Env::VarReader::Read_T(rk, spin)) continue;

            Env::DocGroup spinGr("");
            Env::DocAddNum("spin_id", spin.m_SpinId);
            Env::DocAddNum("result", (uint32_t)spin.m_Result);
            Env::DocAddNum("total_wagered", spin.m_TotalWagered);
            Env::DocAddNum("total_payout", spin.m_TotalPayout);
            Env::DocAddNum("num_bets", (uint32_t)spin.m_NumBets);

            OutputBetPositions(spin);
        }
    }

    // === HISTORY — output newest first (reverse order) ===
    {
        Env::DocArray gr("history");
        for (uint32_t i = 0; i < histCount; i++)
        {
            SpinHistSlot& h = histBuf[histCount - 1 - i];

            Env::DocGroup spinGr("");
            Env::DocAddNum("spin_id", h.spinId);
            Env::DocAddNum("result", h.result);
            Env::DocAddNum("total_wagered", h.totalWagered);
            Env::DocAddNum("total_payout", h.totalPayout);
            Env::DocAddNum("num_bets", h.numBets);
            Env::DocAddNum("status", h.status);

            const char* statusText = "unknown";
            if (h.status == BeamRoulette::SpinStatus::Lost) statusText = "lost";
            else if (h.status == BeamRoulette::SpinStatus::Claimed) statusText = "claimed";
            Env::DocAddText("status_text", statusText);

            // Per-bet details
            {
                Env::DocArray betsArr("bets");
                for (uint32_t bi = 0; bi < h.numBets; bi++) {
                    Env::DocGroup betGr("");
                    Env::DocAddNum("type", (uint32_t)h.betTypes[bi]);
                    Env::DocAddNum("number", (uint32_t)h.betNumbers[bi]);
                    Env::DocAddNum("won", (uint32_t)h.betWon[bi]);
                }
            }
        }
    }

    // === RECENT RESULTS — last 9 globally resolved spins ===
    {
        Env::DocArray gr("recent_results");
        if (s.m_NextSpinId > 1)
        {
            uint64_t startId = (s.m_NextSpinId > 50) ? (s.m_NextSpinId - 50) : 1;

            Env::Key_T<BeamRoulette::SpinKey> k0, k1;
            k0.m_Prefix.m_Cid = cid;
            k0.m_KeyInContract.m_SpinId = startId;
            k1.m_Prefix.m_Cid = cid;
            k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

            // Circular buffer for last 9
            uint32_t recentIds[9];
            uint8_t recentResults[9];
            uint32_t recentCount = 0, recentWrite = 0;

            Env::VarReader scanner(k0, k1);
            Env::Key_T<BeamRoulette::SpinKey> key;
            BeamRoulette::Spin spin;
            while (scanner.MoveNext_T(key, spin))
            {
                if (spin.m_Status == BeamRoulette::SpinStatus::Pending) continue;

                recentIds[recentWrite] = (uint32_t)spin.m_SpinId;
                recentResults[recentWrite] = spin.m_Result;
                recentWrite = (recentWrite + 1) % 9;
                if (recentCount < 9) recentCount++;
            }

            // Output newest first
            for (uint32_t i = 0; i < recentCount; i++)
            {
                uint32_t idx = (recentWrite + 9 - 1 - i) % 9;
                Env::DocGroup resGr("");
                Env::DocAddNum("spin_id", (uint64_t)recentIds[idx]);
                Env::DocAddNum("result", (uint32_t)recentResults[idx]);
            }
        }
    }

    Env::Heap_Free(unclaimedIds);
    Env::Heap_Free(histBuf);
}

void On_view_recent_results(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    uint32_t count = 50;
    Env::DocGet("count", count);
    if (count == 0 || count > 100) count = 50;

    Env::DocArray gr("results");

    if (s.m_NextSpinId > 1)
    {
        uint64_t startId = (s.m_NextSpinId > count) ? (s.m_NextSpinId - count) : 1;

        Env::Key_T<BeamRoulette::SpinKey> k0, k1;
        k0.m_Prefix.m_Cid = cid;
        k0.m_KeyInContract.m_SpinId = startId;
        k1.m_Prefix.m_Cid = cid;
        k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

        Env::VarReader scanner(k0, k1);
        Env::Key_T<BeamRoulette::SpinKey> key;
        BeamRoulette::Spin spin;
        while (scanner.MoveNext_T(key, spin))
        {
            if (spin.m_Status == BeamRoulette::SpinStatus::Pending) continue;

            Env::DocGroup spinGr("");
            Env::DocAddNum("spin_id", spin.m_SpinId);
            Env::DocAddNum("result", (uint32_t)spin.m_Result);
            Env::DocAddNum("status", (uint32_t)spin.m_Status);
        }
    }
}

void On_view_all_spins(const ContractID& cid)
{
    Env::Key_T<BeamRoulette::StateKey> sk;
    sk.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(sk, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    Height hCurrent = Env::get_Height();

    Env::DocAddNum("total_spins_ever", s.m_NextSpinId - 1);

    Env::DocArray gr("spins");

    if (s.m_NextSpinId > 1)
    {
        Env::Key_T<BeamRoulette::SpinKey> k0, k1;
        k0.m_Prefix.m_Cid = cid;
        k0.m_KeyInContract.m_SpinId = 1;
        k1.m_Prefix.m_Cid = cid;
        k1.m_KeyInContract.m_SpinId = s.m_NextSpinId - 1;

        Env::VarReader scanner(k0, k1);
        Env::Key_T<BeamRoulette::SpinKey> key;
        BeamRoulette::Spin spin;
        while (scanner.MoveNext_T(key, spin))
        {
            Env::DocGroup spinGr("");
            Env::DocAddNum("spin_id", spin.m_SpinId);
            Env::DocAddBlob_T("user_pk", spin.m_UserPk);
            Env::DocAddNum("num_bets", (uint32_t)spin.m_NumBets);
            Env::DocAddNum("result", (uint32_t)spin.m_Result);
            Env::DocAddNum("status", (uint32_t)spin.m_Status);
            Env::DocAddNum("total_wagered", spin.m_TotalWagered);
            Env::DocAddNum("total_payout", spin.m_TotalPayout);
            Env::DocAddNum("max_payout", spin.m_MaxPayout);
            Env::DocAddNum("created_height", spin.m_CreatedHeight);
            Env::DocAddNum("reveal_at", spin.m_RevealAt);

            if (spin.m_Status == BeamRoulette::SpinStatus::Pending)
            {
                uint64_t blocksRemaining = (hCurrent < spin.m_RevealAt) ? (spin.m_RevealAt - hCurrent) : 0;
                Env::DocAddNum("blocks_remaining", blocksRemaining);
                Env::DocAddNum("can_reveal", (uint32_t)(blocksRemaining == 0 ? 1 : 0));
            }

            const char* statusText = "pending";
            if (spin.m_Status == BeamRoulette::SpinStatus::Won)     statusText = "won";
            else if (spin.m_Status == BeamRoulette::SpinStatus::Lost)    statusText = "lost";
            else if (spin.m_Status == BeamRoulette::SpinStatus::Claimed) statusText = "claimed";
            Env::DocAddText("status_text", statusText);

            OutputBetPositions(spin);
        }
    }
}

void On_reveal_spin(const ContractID& cid)
{
    BeamRoulette::Method::RevealSpin args;
    Env::DocGet("spin_id", args.m_SpinId);

    OwnerKey ok;
    Env::KeyID kid(&ok, sizeof(ok));

    Env::GenerateKernel(&cid, BeamRoulette::Method::RevealSpin::s_iMethod, &args, sizeof(args), nullptr, 0, &kid, 1, "Roulette: reveal spin", 450000);
}

void On_resolve_spins(const ContractID& cid)
{
    BeamRoulette::Method::ResolveExpiredSpins args;
    uint32_t tempCount = 50;
    Env::DocGet("count", tempCount);
    if (tempCount == 0 || tempCount > 200) tempCount = 50;
    args.m_MaxCount = tempCount;

    // Dynamic charge: Spin structs are large, per-spin cost is higher than d100
    // Extra margin for scanning past non-Pending entries (maxScan = maxCount + 50)
    uint32_t nCharge = 2000000 + tempCount * 100000;

    Env::GenerateKernel(&cid, BeamRoulette::Method::ResolveExpiredSpins::s_iMethod, &args, sizeof(args), nullptr, 0, nullptr, 0, "Roulette: resolve expired spins", nCharge);
}

void On_deposit(const ContractID& cid)
{
    BeamRoulette::Method::Deposit args;
    Env::DocGet("amount", args.m_Amount);

    Env::Key_T<BeamRoulette::StateKey> k;
    k.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(k, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    FundsChange fc;
    fc.m_Aid = s.m_AssetId;
    fc.m_Amount = args.m_Amount;
    fc.m_Consume = 1;

    OwnerKey ok;
    Env::KeyID kid(&ok, sizeof(ok));

    Env::GenerateKernel(&cid, BeamRoulette::Method::Deposit::s_iMethod, &args, sizeof(args), &fc, 1, &kid, 1, "Roulette: deposit", 55000);
}

void On_withdraw(const ContractID& cid)
{
    BeamRoulette::Method::Withdraw args;
    Env::DocGet("amount", args.m_Amount);

    Env::Key_T<BeamRoulette::StateKey> k;
    k.m_Prefix.m_Cid = cid;

    BeamRoulette::State s;
    if (!Env::VarReader::Read_T(k, s))
    {
        OnError("Failed to read contract state");
        return;
    }

    FundsChange fc;
    fc.m_Aid = s.m_AssetId;
    fc.m_Amount = args.m_Amount;
    fc.m_Consume = 0;

    OwnerKey ok;
    Env::KeyID kid(&ok, sizeof(ok));

    Env::GenerateKernel(&cid, BeamRoulette::Method::Withdraw::s_iMethod, &args, sizeof(args), &fc, 1, &kid, 1, "Roulette: withdraw", 55000);
}

void On_set_owner(const ContractID& cid)
{
    BeamRoulette::Method::SetOwner args;
    Env::DocGet("owner_pk", args.m_OwnerPk);

    OwnerKey ok;
    Env::KeyID kid(&ok, sizeof(ok));

    Env::GenerateKernel(&cid, BeamRoulette::Method::SetOwner::s_iMethod, &args, sizeof(args), nullptr, 0, &kid, 1, "Roulette: set owner", 55000);
}

void On_set_config(const ContractID& cid)
{
    BeamRoulette::Method::SetConfig args;
    uint32_t tempPaused = 0;

    Env::DocGet("min_bet", args.m_MinBet);
    Env::DocGet("max_bet", args.m_MaxBet);
    Env::DocGet("straight_mult", args.m_StraightMult);
    Env::DocGet("even_money_mult", args.m_EvenMoneyMult);
    Env::DocGet("dozen_col_mult", args.m_DozenColMult);
    Env::DocGet("reveal_epoch", args.m_RevealEpoch);
    Env::DocGet("paused", tempPaused);
    Env::DocGet("asset_id", args.m_AssetId);

    args.m_Paused = (uint8_t)tempPaused;

    OwnerKey ok;
    Env::KeyID kid(&ok, sizeof(ok));

    Env::GenerateKernel(&cid, BeamRoulette::Method::SetConfig::s_iMethod, &args, sizeof(args), nullptr, 0, &kid, 1, "Roulette: set config", 55000);
}

// ============================================================================
// Dispatcher: Method_1
// ============================================================================
BEAM_EXPORT void Method_1()
{
    Env::DocGroup root("");

    char szRole[16], szAction[32];

    if (!Env::DocGetText("role", szRole, sizeof(szRole)))
        return OnError("Role not specified");

    if (!Env::DocGetText("action", szAction, sizeof(szAction)))
        return OnError("Action not specified");

    ContractID cid;
    Env::DocGet("cid", cid);

    if (!Env::Strcmp(szRole, "manager"))
    {
        if (!Env::Strcmp(szAction, "create_contract"))
            return On_create_contract(cid);
        if (!Env::Strcmp(szAction, "view_contracts"))
            return On_view_contracts(cid);
        if (!Env::Strcmp(szAction, "view_pool"))
            return On_view_pool(cid);
        if (!Env::Strcmp(szAction, "view_all_spins"))
            return On_view_all_spins(cid);
        if (!Env::Strcmp(szAction, "reveal_spin"))
            return On_reveal_spin(cid);
        if (!Env::Strcmp(szAction, "resolve_spins"))
            return On_resolve_spins(cid);
        if (!Env::Strcmp(szAction, "deposit"))
            return On_deposit(cid);
        if (!Env::Strcmp(szAction, "withdraw"))
            return On_withdraw(cid);
        if (!Env::Strcmp(szAction, "set_owner"))
            return On_set_owner(cid);
        if (!Env::Strcmp(szAction, "set_config"))
            return On_set_config(cid);

        return OnError("Invalid action");
    }

    if (!Env::Strcmp(szRole, "user"))
    {
        if (!Env::Strcmp(szAction, "view_params"))
            return On_view_params(cid);
        if (!Env::Strcmp(szAction, "view_user_pk"))
            return On_view_user_pk(cid);
        if (!Env::Strcmp(szAction, "place_bets"))
            return On_place_bets(cid);
        if (!Env::Strcmp(szAction, "preview_place_bets"))
            return On_preview_place_bets(cid);
        if (!Env::Strcmp(szAction, "check_results"))
            return On_check_results(cid);
        if (!Env::Strcmp(szAction, "check_single"))
            return On_check_single(cid);
        if (!Env::Strcmp(szAction, "my_spins"))
            return On_my_spins(cid);
        if (!Env::Strcmp(szAction, "view_all"))
            return On_view_all(cid);
        if (!Env::Strcmp(szAction, "view_recent_results"))
            return On_view_recent_results(cid);

        return OnError("Invalid action");
    }

    OnError("Invalid role");
}
