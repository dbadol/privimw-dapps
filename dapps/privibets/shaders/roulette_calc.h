// PriviBets Roulette — shared max-exposure calculator (contract + app + native tests)
// American Roulette: 38 outcomes (0-37, where 37 = 00)
#pragma once

#include <stdint.h>

namespace BeamRoulette {

static const uint32_t s_WheelOutcomes = 38;

static const uint8_t s_RedNumbers[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
static const uint32_t s_RedCount = sizeof(s_RedNumbers) / sizeof(s_RedNumbers[0]);

inline bool IsRed(uint8_t n) {
    for (uint32_t i = 0; i < s_RedCount; i++) {
        if (s_RedNumbers[i] == n) return true;
    }
    return false;
}

// Same winning rules as contract ProcessSpinResult / app preview
inline bool IsBetWon(uint8_t betType, uint8_t betNumber, uint8_t result) {
    if (result == 0 || result == 37)
        return (betType == 0 && betNumber == result); // BetType::Straight

    switch (betType) {
        case 0:  return (result == betNumber);           // Straight
        case 1:  return IsRed(result);                   // Red
        case 2:  return (!IsRed(result));                // Black
        case 3:  return (result % 2 == 1);               // Odd
        case 4:  return (result % 2 == 0);               // Even
        case 5:  return (result >= 1 && result <= 18);   // Low
        case 6:  return (result >= 19 && result <= 36);  // High
        case 7:  return (result >= 1 && result <= 12);   // Dozen1
        case 8:  return (result >= 13 && result <= 24);  // Dozen2
        case 9:  return (result >= 25 && result <= 36);  // Dozen3
        case 10: return (result % 3 == 1);               // Column1
        case 11: return (result % 3 == 2);               // Column2
        case 12: return (result % 3 == 0);               // Column3
        default: return false;
    }
}

inline bool CheckedLegPayout(uint64_t amount, uint64_t mult, uint64_t& payout) {
    if (mult == 0) return false;
    if (amount > UINT64_MAX / mult) return false;
    payout = (amount * mult) / 100;
    return true;
}

inline bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& sum) {
    if (a > UINT64_MAX - b) return false;
    sum = a + b;
    return true;
}

struct SpinExposure {
    uint64_t m_MaxPayout;
    uint64_t m_PayoutByResult[s_WheelOutcomes];
};

// Worst-case house payout for one spin: max over wheel results of sum(leg payouts).
// Returns false on arithmetic overflow.
inline bool ComputeSpinExposure(
    uint8_t numBets,
    const uint8_t* types,
    const uint8_t* numbers,
    const uint64_t* amounts,
    const uint64_t* multipliers,
    SpinExposure& out)
{
    out.m_MaxPayout = 0;
    for (uint32_t r = 0; r < s_WheelOutcomes; r++)
        out.m_PayoutByResult[r] = 0;

    for (uint32_t r = 0; r < s_WheelOutcomes; r++) {
        const uint8_t result = (uint8_t)r;
        uint64_t totalForResult = 0;

        for (uint8_t i = 0; i < numBets; i++) {
            if (!IsBetWon(types[i], numbers[i], result))
                continue;

            uint64_t legPayout = 0;
            if (!CheckedLegPayout(amounts[i], multipliers[i], legPayout))
                return false;
            if (!CheckedAdd(totalForResult, legPayout, totalForResult))
                return false;
        }

        out.m_PayoutByResult[r] = totalForResult;
        if (totalForResult > out.m_MaxPayout)
            out.m_MaxPayout = totalForResult;
    }

    return true;
}

// Sum of per-leg standalone max payouts (legacy conservative reserve — for tests/comparison)
inline bool ComputeSummedLegMaxPayout(
    uint8_t numBets,
    const uint64_t* amounts,
    const uint64_t* multipliers,
    uint64_t& summedMax)
{
    summedMax = 0;
    for (uint8_t i = 0; i < numBets; i++) {
        uint64_t legMax = 0;
        if (!CheckedLegPayout(amounts[i], multipliers[i], legMax))
            return false;
        if (!CheckedAdd(summedMax, legMax, summedMax))
            return false;
    }
    return true;
}

} // namespace BeamRoulette
