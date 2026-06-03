// PriviBets Roulette Contract Header
// American Roulette: 38 outcomes (0-37, where 37 = 00)
#pragma once

namespace BeamRoulette {

// Shader ID (hash of compiled roulette.wasm) - update after each recompile
static const ShaderID s_SID = {0xe3,0x0c,0xf7,0x20,0x28,0x7f,0x02,0xe4,0xf4,0x6b,0x70,0x94,0xc1,0x46,0x22,0xe4,0x29,0x77,0x86,0x1c,0x5c,0xe9,0x4d,0x17,0x66,0x66,0xeb,0x55,0x01,0x97,0x58,0x9b};

// Multiplier constants (x100 scale)
static const uint64_t s_DefaultStraightMult = 3600ULL;     // 36x (35:1)
static const uint64_t s_DefaultEvenMoneyMult = 200ULL;     // 2x  (1:1)
static const uint64_t s_DefaultDozenColMult = 300ULL;      // 3x  (2:1)

// Bet limits
static const uint64_t s_DefaultMinBet = 1000000ULL;        // 0.01 BEAM
static const uint64_t s_DefaultMaxBet = 10000000000ULL;    // 100 BEAM

// Reveal timing
static const uint64_t s_RevealEpoch = 3ULL;                // 3 blocks minimum wait (default)
static const uint64_t s_MinRevealEpoch = 2ULL;             // Security floor: never less than 2

// Multi-bet limits
static const uint32_t s_MaxBetsPerSpin = 20;              // Max bet positions per TX

// Bet types (13 standard American Roulette bets)
struct BetType {
    enum _ {
        Straight = 0,   // Single number (35:1)
        Red      = 1,   // Red numbers (1:1)
        Black    = 2,   // Black numbers (1:1)
        Odd      = 3,   // Odd numbers (1:1)
        Even     = 4,   // Even numbers (1:1)
        Low      = 5,   // 1-18 (1:1)
        High     = 6,   // 19-36 (1:1)
        Dozen1   = 7,   // 1-12 (2:1)
        Dozen2   = 8,   // 13-24 (2:1)
        Dozen3   = 9,   // 25-36 (2:1)
        Column1  = 10,  // 1,4,7,...,34 (2:1)
        Column2  = 11,  // 2,5,8,...,35 (2:1)
        Column3  = 12   // 3,6,9,...,36 (2:1)
    };
};

// Spin status
struct SpinStatus {
    enum _ {
        Pending  = 0,
        Won      = 1,   // At least one bet position won
        Lost     = 2,   // All bet positions lost
        Claimed  = 3    // Winnings have been claimed
    };
};

// Contract state
struct State {
    PubKey m_OwnerPk;               // Owner's public key
    uint64_t m_MinBet;              // Per-position minimum bet in groth
    uint64_t m_MaxBet;              // Per-position maximum bet in groth
    uint64_t m_StraightMult;        // Straight multiplier (x100: 3600 = 36x)
    uint64_t m_EvenMoneyMult;       // Even-money multiplier (x100: 200 = 2x)
    uint64_t m_DozenColMult;        // Dozen/Column multiplier (x100: 300 = 3x)
    AssetID m_AssetId;              // Asset ID (0 = BEAM)
    uint64_t m_TotalDeposited;      // Total owner deposits (cumulative, never decreases)
    uint64_t m_TotalWithdrawn;      // Total owner withdrawals (cumulative, never decreases)
    uint64_t m_TotalBets;           // Total user wagers received (cumulative)
    uint64_t m_TotalPayouts;        // Total payouts made to users (cumulative)
    uint64_t m_PendingBets;         // Sum of locked wagers for pending spins
    uint64_t m_PendingMaxPayout;    // Sum of per-spin max payouts (max over 38 wheel outcomes)
    uint64_t m_PendingPayouts;      // Sum of payouts for Won-but-not-yet-Claimed spins
    uint64_t m_NextSpinId;          // Next spin ID to assign
    uint64_t m_FirstUnresolvedSpinId; // Scan optimization: skip resolved spins
    uint64_t m_RevealEpoch;         // Blocks to wait before reveal
    uint8_t m_Paused;               // Is contract paused?
    uint8_t m_Initialized;          // Is contract initialized?
};

// Single bet position within a spin
struct BetPosition {
    uint8_t m_Type;                 // BetType enum (0-12)
    uint8_t m_Number;               // For Straight: 0-37 (37=00); ignored for other types
    uint64_t m_Amount;              // Wager on this position (in groth)
    uint64_t m_Multiplier;          // Multiplier locked at placement (x100 scale)
    uint64_t m_Payout;              // Actual payout if won (set on reveal)
    uint8_t m_Won;                  // 0 or 1 (set on reveal)
};

// Spin record — one per transaction, contains up to s_MaxBetsPerSpin bet positions
struct Spin {
    uint64_t m_SpinId;
    PubKey m_UserPk;                // User's public key
    HashValue m_PlacementHash;      // Block hash at placement — entropy for result
    uint8_t m_NumBets;              // 1-s_MaxBetsPerSpin active bet positions
    uint8_t m_Result;               // 0-37 (37=00), set on reveal
    uint8_t m_Status;               // SpinStatus enum
    uint64_t m_TotalWagered;        // Sum of all bet amounts
    uint64_t m_TotalPayout;         // Sum of winning payouts (set on reveal)
    uint64_t m_MaxPayout;           // Max total payout over wheel outcomes (reserved at placement)
    uint64_t m_CreatedHeight;       // Block height when spin was placed
    Height m_RevealAt;              // Block height when result is determined
    BetPosition m_Bets[s_MaxBetsPerSpin];  // Fixed array; unused slots zero-initialized
};

// Constructor parameters
struct Params {
    PubKey m_OwnerPk;
};

// Method parameter structures
namespace Method {

// Place 1-s_MaxBetsPerSpin bet positions in one spin (user calls)
struct PlaceBets {
    static const uint32_t s_iMethod = 2;
    PubKey m_UserPk;                // User's public key (derived from CID in app shader)
    AssetID m_AssetId;
    uint8_t m_NumBets;              // 1-s_MaxBetsPerSpin
    // Parallel arrays (BVM-friendly flat layout):
    uint8_t m_Types[s_MaxBetsPerSpin];    // BetType enum per position
    uint8_t m_Numbers[s_MaxBetsPerSpin];  // Straight number per position (0-37)
    uint64_t m_Amounts[s_MaxBetsPerSpin]; // Wager per position (in groth)
};

// Check and claim all user's pending/won spins (user calls)
struct CheckResults {
    static const uint32_t s_iMethod = 3;
    PubKey m_UserPk;
};

// Owner deposit into pool
struct Deposit {
    static const uint32_t s_iMethod = 4;
    uint64_t m_Amount;
};

// Owner withdraw from pool
struct Withdraw {
    static const uint32_t s_iMethod = 5;
    uint64_t m_Amount;
};

// Transfer ownership
struct SetOwner {
    static const uint32_t s_iMethod = 6;
    PubKey m_OwnerPk;
};

// Update contract configuration (owner only)
struct SetConfig {
    static const uint32_t s_iMethod = 7;
    uint64_t m_MinBet;
    uint64_t m_MaxBet;
    uint64_t m_StraightMult;
    uint64_t m_EvenMoneyMult;
    uint64_t m_DozenColMult;
    uint64_t m_RevealEpoch;
    uint8_t m_Paused;
    AssetID m_AssetId;
};

// Owner emergency reveal (deterministic — same formula as user reveal)
struct RevealSpin {
    static const uint32_t s_iMethod = 8;
    uint64_t m_SpinId;
};

// Check and claim a single spin by ID (user calls)
struct CheckSingleSpin {
    static const uint32_t s_iMethod = 9;
    PubKey m_UserPk;
    uint64_t m_SpinId;
};

// Resolve expired pending spins (anyone can call — no funds movement, just reveals)
struct ResolveExpiredSpins {
    static const uint32_t s_iMethod = 10;
    uint32_t m_MaxCount;            // Capped at 200 internally
};

} // namespace Method

// Storage tags
struct Tags {
    static const uint8_t s_State = 0;
    static const uint8_t s_Spin = 1;
};

// Storage key structures — MUST be packed to avoid padding in BVM storage keys.
// Without packing, uint8_t + uint64_t has 7 bytes of uninitialized padding,
// causing key mismatches between contract SaveVar_T and app VarReader::Read_T.
#pragma pack(push, 1)
struct StateKey {
    uint8_t m_Tag;
    StateKey() : m_Tag(Tags::s_State) {}
};

struct SpinKey {
    uint8_t m_Tag;
    uint64_t m_SpinId;
    SpinKey() : m_Tag(Tags::s_Spin), m_SpinId(0) {}
};
#pragma pack(pop)

} // namespace BeamRoulette
