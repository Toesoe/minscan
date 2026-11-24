/**
 * @file pipeline.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief post-acquisition image chain in vendor order
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 * @note order is coreg -> base gain (4-plane only) -> IR normalise -> ICE -> balance. callers own the frame and the
 *       0x83 base table; nothing here allocates or frees
 */

#include "image.h"
#include "src/core/error.h"

// the 4-plane readout carries a CCD-edge gain gradient that the 3-plane readout does not
static const double baseStrength[3] = {0.103, 0.103, 0.091};

int imagePostprocess(struct Frame *pFrame, const struct ImagePostParams *pParams)
{
    if ((pFrame == nullptr) || (pParams == nullptr))
    {
        return MS_ERR_RANGE;
    }

    int rc = imageCoregPlanes(pFrame, pParams->is35mm);
    if (rc != MS_OK)
    {
        return rc;
    }

    if (pFrame->numPlanes == 4)
    {
        // flatten RGB against the raw IR column profile before ICE; the 3-plane
        // path is already flat and must stay untouched
        if (pParams->pBase != nullptr)
        {
            rc = imageBaseGain(pFrame, pParams->pBase, pParams->baseCols, baseStrength);
            if (rc != MS_OK)
            {
                return rc;
            }
        }

        // the IR plane is self-normalised for every 4-plane frame: its column
        // profile is illumination, and ICE turns residual banding into color
        // artefacts. runs on the linear path too; the RGB planes stay raw
        rc = imageNormalizeIR(pFrame);
        if (rc != MS_OK)
        {
            return rc;
        }
    }

    if (pParams->ice)
    {
        rc = imageICEApply(pFrame, (unsigned)((pParams->is35mm ? 4800u : 3200u) / pParams->div));
        if (rc != MS_OK)
        {
            return rc;
        }
    }

    if (pFrame->numPlanes >= 3)
    {
        // vendor 16-bit linear = coregistered wire + per-channel scale. runs
        // after ICE so the repair fill sees raw wire levels
        rc = imageBalanceRgb(pFrame, pParams->is35mm ? 0.941 : 0.967, pParams->is35mm ? 0.940 : 0.953,
                             pParams->is35mm ? 1.0 : 1.088);
        if (rc != MS_OK)
        {
            return rc;
        }
    }

    return MS_OK;
}
