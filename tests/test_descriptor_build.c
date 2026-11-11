#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "minscan/exposure.h"
#include "test.h"

#include <stdio.h>

#ifndef MS_TESTDATA_DIR
#define MS_TESTDATA_DIR "tests/testdata"
#endif

struct GoldenCase
{
    const char     *szFile;
    enum Media      media;
    enum Job        job;
    enum FilmType   film;
    unsigned        div;
    unsigned        slot;
    int16_t         focus;
    bool            ir;
    struct Exposure exposure;
};

// inputs the caller must supply; everything else in the 100 bytes is derived
// golden set: msscan captures plus the 2026-11-03 same-session slot capture
static const struct GoldenCase goldenCases[] = {
    // 6x9 colour (ICE)
    {"desc_scan_ice_div1.bin",              MS_MEDIA_120,  MS_JOB_SCAN,       MS_FILM_COLOUR_POS, 1, 0, 43,   true,  {813, 733, 795, 511}   },
    {"desc_scan_ice_div2.bin",              MS_MEDIA_120,  MS_JOB_SCAN,       MS_FILM_COLOUR_POS, 2, 0, 43,   true,  {2328, 2115, 2252, 511}},
    {"desc_prescan_ice_div8.bin",
     MS_MEDIA_120,                                         MS_JOB_PRESCAN,
     MS_FILM_COLOUR_POS,                                                                          8,
     0,                                                                                                 43,
     true,                                                                                                           {813, 733, 795, 511}   },
    {"desc_prescan_ice_10plus11.bin",
     MS_MEDIA_120,                                         MS_JOB_PRESCAN,
     MS_FILM_COLOUR_POS,                                                                          8,
     0,                                                                                                 0,
     true,                                                                                                           {832, 832, 832, 518}   },
    {"desc_prescan_noice_10plus11.bin",
     MS_MEDIA_120,                                         MS_JOB_PRESCAN,
     MS_FILM_COLOUR_POS,                                                                          8,
     0,                                                                                                 0,
     false,                                                                                                          {832, 832, 832, 0}     },
    {"desc_ice_0_10plus11.bin",             MS_MEDIA_120,  MS_JOB_PRESCAN,    MS_FILM_COLOUR_POS, 8, 0, 0,    true,  {832, 832, 832, 518}   },
    {"desc_ice_2_12.bin",                   MS_MEDIA_120,  MS_JOB_PRESCAN,    MS_FILM_COLOUR_POS, 8, 0, 0,    true,  {832, 832, 832, 518}   },

    // 6x9 mono
    {"desc_mono6x9_scan_div1.bin",          MS_MEDIA_120,  MS_JOB_SCAN,       MS_FILM_MONO,       1, 0, 0,    false, {897, 897, 897, 0}     },
    {"desc_mono6x9_scan_div2.bin",          MS_MEDIA_120,  MS_JOB_SCAN,       MS_FILM_MONO,       2, 0, 0,    false, {897, 897, 897, 0}     },
    {"desc_mono6x9_scan_div4.bin",          MS_MEDIA_120,  MS_JOB_SCAN,       MS_FILM_MONO,       4, 0, 0,    false, {897, 897, 897, 0}     },
    {"desc_mono6x9_prescan_div8.bin",       MS_MEDIA_120,  MS_JOB_PRESCAN,    MS_FILM_MONO,       8, 0, 0,    false, {510, 510, 510, 0}     },

    // 35 mm mono (slot B in these captures: odd frames)
    {"desc_scan_35mm.bin",                  MS_MEDIA_35MM, MS_JOB_SCAN,       MS_FILM_MONO,       4, 1, 0,    false, {2025, 2025, 2025, 0}  },
    {"desc_mono35mm_scan_div1.bin",         MS_MEDIA_35MM, MS_JOB_SCAN,       MS_FILM_MONO,       1, 1, 71,   false, {1951, 1951, 1951, 0}  },
    {"desc_prescan_35mm.bin",               MS_MEDIA_35MM, MS_JOB_PRESCAN,    MS_FILM_MONO,       8, 1, 0,    false, {640, 640, 640, 0}     },

    // 35 mm colour (slot A in these captures)
    {"desc_colour35mm_prescan_div8.bin",
     MS_MEDIA_35MM,                                        MS_JOB_PRESCAN,
     MS_FILM_COLOUR_POS,                                                                          8,
     0,                                                                                                 71,
     true,                                                                                                           {832, 832, 832, 586}   },
    {"desc_colour35mm_scan_div1.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_COLOUR_POS,                                                                          1,
     0,                                                                                                 71,
     true,                                                                                                           {1412, 1529, 1529, 586}},
    {"desc_colour35mm_scan_div4.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_COLOUR_POS,                                                                          4,
     0,                                                                                                 71,
     true,                                                                                                           {987, 919, 993, 586}   },

    // 35 mm same-session slot capture (2026-11-03, div4 ICE off)
    {"desc_colour35mm_scan_div4_slotA.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_COLOUR_POS,                                                                          4,
     0,                                                                                                 49,
     false,                                                                                                          {1519, 1519, 1519, 0}  },
    {"desc_colour35mm_scan_div4_slotB.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_COLOUR_POS,                                                                          4,
     1,                                                                                                 49,
     false,                                                                                                          {1488, 1488, 1488, 0}  },
    {"desc_mono35mm_scan_div4_slotA.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_MONO,                                                                                4,
     0,                                                                                                 49,
     false,                                                                                                          {4070, 4070, 4070, 0}  },
    {"desc_mono35mm_scan_div4_slotB.bin",
     MS_MEDIA_35MM,                                        MS_JOB_SCAN,
     MS_FILM_MONO,                                                                                4,
     1,                                                                                                 49,
     false,                                                                                                          {3884, 3884, 3884, 0}  },

    // arming (div is the session's scan div; slot does not affect the aim)
    {"desc_arm_35mm.bin",                   MS_MEDIA_35MM, MS_JOB_ARM,        MS_FILM_MONO,       4, 0, 0,    false, {2025, 2025, 2025, 0}  },
    {"desc_arm_ice_6x9.bin",                MS_MEDIA_120,  MS_JOB_ARM,        MS_FILM_COLOUR_POS, 1, 0, 43,   false, {813, 733, 795, 0}     },
    {"desc_colour35mm_arm2_div1.bin",
     MS_MEDIA_35MM,                                        MS_JOB_ARM,
     MS_FILM_COLOUR_POS,                                                                          1,
     0,                                                                                                 71,
     false,                                                                                                          {1412, 1529, 1529, 0}  },
    {"desc_colour35mm_arm_div1.bin",
     MS_MEDIA_35MM,                                        MS_JOB_ARM,
     MS_FILM_COLOUR_POS,                                                                          4,
     0,                                                                                                 71,
     false,                                                                                                          {987, 919, 993, 0}     },

    // focus / focus-open (class 3: the focus window is a host preset)
    {"desc_focus_6x9.bin",                  MS_MEDIA_120,  MS_JOB_FOCUS,      MS_FILM_MONO,       1, 0, -250, false, {914, 831, 652, 0}     },
    {"desc_focus_35mm.bin",                 MS_MEDIA_35MM, MS_JOB_FOCUS,      MS_FILM_MONO,       1, 0, 250,  false, {4095, 4095, 4095, 0}  },
    {"desc_focusopen_6x9.bin",              MS_MEDIA_120,  MS_JOB_FOCUS_OPEN, MS_FILM_MONO,       1, 0, 0,    false, {549, 499, 392, 0}     },
    {"desc_focusopen_35mm.bin",             MS_MEDIA_35MM, MS_JOB_FOCUS_OPEN, MS_FILM_MONO,       1, 0, 0,    false, {640, 640, 640, 0}     },
};

static int readGolden(const char *szFile, struct Descriptor *pDescriptorOut)
{
    char   szPath[512];
    FILE  *pFile;
    size_t read;

    snprintf(szPath, sizeof szPath, "%s/%s", MS_TESTDATA_DIR, szFile);

    pFile = fopen(szPath, "rb");
    if (pFile == NULL)
    {
        fprintf(stderr, "cannot open golden %s\n", szPath);
        return 1;
    }

    read = fread(pDescriptorOut->raw, 1, (size_t)MS_DESC_LEN, pFile);
    fclose(pFile);

    if (read != (size_t)MS_DESC_LEN)
    {
        fprintf(stderr, "%s: read %zu bytes, want %d\n", szPath, read, MS_DESC_LEN);
        return 1;
    }
    return 0;
}

static int expectRefused(const struct DescriptorParameters *pParams)
{
    struct Descriptor descriptor;

    memset(&descriptor, 0, sizeof descriptor);

    if (descriptorBuild(pParams, &descriptor) == MS_OK)
    {
        return 1;
    }
    for (size_t i = 0; i < (size_t)MS_DESC_LEN; i++)
    {
        if (descriptor.raw[i] != 0)
        {
            return 1;
        }
    }
    return 0;
}

static int checkRefusals(void)
{
    struct Exposure exposure = {800, 800, 800, 0};
    struct Exposure zero     = {0, 0, 0, 0};

    // mono + ICE
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_MONO,
                                                       .div   = 1,
                                                       .ir    = true,
                                                       .pExp  = &exposure}) == 0);
    // focus out of range
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_FOCUS,
                                                       .film  = MS_FILM_MONO,
                                                       .div   = 1,
                                                       .focus = 251,
                                                       .pExp  = &exposure}) == 0);
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_MONO,
                                                       .div   = 1,
                                                       .focus = -251,
                                                       .pExp  = &exposure}) == 0);
    // div 0
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .pExp  = &exposure}) == 0);
    // unknown job / media / slot
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = (enum Job)99,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .div   = 1,
                                                       .pExp  = &exposure}) == 0);
    CHECK(expectRefused(&(struct DescriptorParameters){.media = (enum Media)99,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .div   = 1,
                                                       .pExp  = &exposure}) == 0);
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_35MM,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .slot  = 2,
                                                       .div   = 1,
                                                       .pExp  = &exposure}) == 0);
    // 3/5 read passes are focus-only
    CHECK(expectRefused(&(struct DescriptorParameters){.media      = MS_MEDIA_120,
                                                       .job        = MS_JOB_SCAN,
                                                       .film       = MS_FILM_COLOUR_POS,
                                                       .div        = 1,
                                                       .readPasses = 3,
                                                       .pExp       = &exposure}) == 0);
    // no exposure input
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .div   = 1}) == 0);
    // explicit exposure over the prescan cap
    {
        struct Exposure over = {900, 900, 900, 0};

        CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                           .job   = MS_JOB_PRESCAN,
                                                           .film  = MS_FILM_COLOUR_POS,
                                                           .div   = 8,
                                                           .pExp  = &over}) == 0);
    }
    // zero exposure
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_SCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .div   = 1,
                                                       .pExp  = &zero}) == 0);
    // prescan must be div 8; focus must be div 1
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_PRESCAN,
                                                       .film  = MS_FILM_COLOUR_POS,
                                                       .div   = 1,
                                                       .pExp  = &exposure}) == 0);
    CHECK(expectRefused(&(struct DescriptorParameters){.media = MS_MEDIA_120,
                                                       .job   = MS_JOB_FOCUS,
                                                       .film  = MS_FILM_MONO,
                                                       .div   = 8,
                                                       .pExp  = &exposure}) == 0);

    return 0;
}

static int checkFormatCodes(void)
{
    struct Exposure             exposure = {897, 897, 897, 0};
    struct Descriptor           descriptor;
    struct DescriptorParameters params = {
        .media      = MS_MEDIA_120,
        .job        = MS_JOB_SCAN,
        .film       = MS_FILM_MONO,
        .formatCode = 4, // B660 / 6x6
        .div        = 1,
        .slot       = 0,
        .focus      = 0,
        .ir         = false,
        .readPasses = 0,
        .pExp       = &exposure,
    };

    CHECK(descriptorBuild(&params, &descriptor) == MS_OK);
    CHECK(descriptorGetWidth(&descriptor) == 7128);
    CHECK(descriptorGetRows(&descriptor) == 3u * 7128u); // K = 3 on a 6x format
    CHECK(descriptorGetDivisor(&descriptor) == 1);

    // a 35 mm format row on a 120 carrier, and the reverse, must refuse
    params.formatCode = 1;
    CHECK(descriptorBuild(&params, &descriptor) == MS_ERR_RANGE);

    params.media      = MS_MEDIA_35MM;
    params.formatCode = 4;
    CHECK(descriptorBuild(&params, &descriptor) == MS_ERR_RANGE);

    return 0;
}

int main(void)
{
    for (size_t idx = 0; idx < sizeof goldenCases / sizeof goldenCases[0]; idx++)
    {
        const struct GoldenCase    *pCase    = &goldenCases[idx];
        struct Exposure             exposure = pCase->exposure;
        struct Descriptor           want;
        struct Descriptor           got;
        struct DescriptorParameters params = {
            .media      = pCase->media,
            .job        = pCase->job,
            .film       = pCase->film,
            .div        = pCase->div,
            .slot       = pCase->slot,
            .focus      = pCase->focus,
            .ir         = pCase->ir,
            .readPasses = 0,
            .pExp       = &exposure,
        };

        CHECK(readGolden(pCase->szFile, &want) == 0);
        CHECK(descriptorBuild(&params, &got) == MS_OK);
        CHECK_BYTES(got.raw, want.raw, (size_t)MS_DESC_LEN);
    }

    CHECK(checkRefusals() == 0);
    CHECK(checkFormatCodes() == 0);

    return 0;
}
