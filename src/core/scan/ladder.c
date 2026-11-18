/**
 * @file ladder.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief IR level/p/q block for the descriptor
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "ladder.h"

// fill value when the ladder is never consulted: focus blocks and non-ICE prescans
#define IR_DEFAULT_LEVEL 0xffu
// fixed p/r pair an ICE-on prescan uses instead of a ladder lookup
#define IR_PRESCAN_P     0.6
#define IR_PRESCAN_R     0.6

struct IRTriple
{
    double p;
    double q;
    double r;
};

static struct IRTriple tripleMake(double p, double q, double r)
{
    return (struct IRTriple){.p = p, .q = q, .r = r};
}

/**
 * @brief pick the IR triple (P, Q, R) for one job from Minolta's A-D tables.
 *
 * @param ladder      dispatch table, 'A'..'D'; scanIRParameters() chooses it
 * @param rung        scanCoarseDivisor(divisor): 1, 2, 4 or 8.
 * @param durationSec estimated capture time, passes * (width * 0.0003 + (maxExposure + 1) * 0.0048).
 * @return P, Q, R in x87 load order: wire +0x42 = 10P, +0x44 = 10Q, +0x41 = 5R. R is a headroom budget that counts down
           with @p durationSec, Q a small fixed IR gain, and P is zero unless @p rung is 4 or 8.

 * @details
 * FUN_10009b90 has four dispatch sites, one per table. A (0x9cb0) and B (0x9dba) are for a 120 carrier at <= 3200 dpi;
 * C (0x9ec5) and D (0xa0c0) serve 35 mm and any other scan above 3200 dpi. within a pair the first table is a
 * single-pass scan, the second is IR-on or a repeat pass (e.g. multisampling).
 *
 * each table branches on @p rung first, then walks ascending duration thresholds (4..16 s) and returns the triple at
   the first one @p durationSec is under.
 *
 *
 * @note Minolta's 0.0012 coefficient branch is unreachable: bit 0x10 of +0x40 is always set.
 */
static struct IRTriple irTriple(char ladder, int rung, double durationSec)
{
    switch (ladder)
    {
        case 'A': // 0x10009cb0
        {
            if (rung == 1)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                return tripleMake(0.0, 2.0, 5.0);
            }
            if (rung == 2)
            {
                if (durationSec < 12.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 14.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                return tripleMake(2.0, 1.2, 7.4);
            }
            if (rung == 4)
            {
                return tripleMake(1.4, 1.0, 8.0);
            }
            return tripleMake(1.4, 1.0, 6.0);
        }
        case 'B': // 0x10009dba
        {
            if (rung == 1)
            {
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.0, 11.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 2.0);
            }
            if (rung == 2)
            {
                if (durationSec < 4.0)
                {
                    return tripleMake(2.0, 1.2, 7.4 - durationSec);
                }
                return tripleMake(2.0, 1.2, 3.0);
            }
            if (rung == 4)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(1.4, 1.0, 8.0 - durationSec);
                }
                return tripleMake(1.4, 1.0, 0.2);
            }
            if (durationSec < 5.0)
            {
                return tripleMake(1.4, 1.0, 5.0 - durationSec);
            }
            return tripleMake(1.4, 1.0, 0.2);
        }
        case 'C': // 0x10009ec5
        {
            if (rung == 1)
            {
                if (durationSec < 6.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                if (durationSec < 7.0)
                {
                    return tripleMake(0.0, 3.6, 3.6);
                }
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 2.4, 5.6);
                }
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.4, 6.6);
                }
                return tripleMake(0.0, 3.0, 7.0);
            }
            if (rung == 2)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                if (durationSec < 12.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                return tripleMake(0.0, 2.0, 8.0);
            }
            if (rung == 4)
            {
                if (durationSec < 16.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 19.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                if (durationSec < 22.0)
                {
                    return tripleMake(0.0, 2.8, 2.8);
                }
                return tripleMake(0.0, 2.0, 10.0);
            }
            return tripleMake(1.5, 1.0, 6.0);
        }
        case 'D': // 0x1000a0c0
        {
            if (rung == 1)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 3.0, 12.0 - durationSec);
                }
                return tripleMake(0.0, 3.0, 4.0);
            }
            if (rung == 2)
            {
                if (durationSec < 4.0)
                {
                    return tripleMake(0.0, 2.0, 8.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 4.0);
            }
            if (rung == 4)
            {
                if (durationSec < 10.0)
                {
                    return tripleMake(0.0, 2.0, 10.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 0.2);
            }
            if (durationSec < 6.0)
            {
                return tripleMake(0.0, 2.0, 6.0 - durationSec);
            }
            return tripleMake(1.5, 1.0, 0.2);
        }
        default:
        {
            return tripleMake(0.0, 0.0, 0.0);
        }
    }
}

struct DescriptorIR scanIRParameters(const struct Window *pWindow, const struct DescriptorParameters *pParams,
                                     unsigned passes, uint16_t maxExposure)
{
    struct DescriptorIR ir = {.level = IR_DEFAULT_LEVEL, .p = 0, .q = 0};

    if ((pParams->job == SCAN_JOB_FOCUS) || (pParams->job == SCAN_JOB_FOCUS_OPEN))
    {
        return ir; // focus never reaches 9b90 and keeps the fill default
    }
    if ((pWindow->class != 0) && (pWindow->variant != 3))
    {
        return ir; // non-IR prescan keeps the fill default
    }

    double pVal = 0.0;
    double qVal = IR_PRESCAN_P;
    double rVal = IR_PRESCAN_R;

    if (pWindow->class == 0)
    {
        // 120 goes through the A/B tables, every other carrier through C/D; IR or a repeat pass
        // moves to the second table of the pair
        const bool is120  = (pParams->carrier == SCAN_CARRIER_120);
        const bool repeat = (pWindow->variant == 3) || (passes > 1);
        const char ladder = is120 ? (repeat ? 'B' : 'A') : (repeat ? 'D' : 'C');

        double durationSec =
            (double)passes * ((double)pWindow->pFormat->width * 0.0003 + ((double)maxExposure + 1.0) * 0.0048);
        struct IRTriple triple = irTriple(ladder, (int)scanCoarseDivisor(pWindow->divisor), durationSec);

        pVal = triple.p;
        qVal = triple.q;
        rVal = triple.r;
    }

    ir.level = (uint8_t)((int)(5.0 * rVal) & 0xff);
    ir.p     = (uint16_t)(int)(10.0 * pVal);
    ir.q     = (uint16_t)(int)(10.0 * qVal);
    return ir;
}