/**
 * @file capi.h
 * @brief stable C ABI for embedding minscan
 *
 * The internals (struct Session/Frame/Transport) are deliberately hidden. A
 * session owns the scanner transport and the last frame; frames are planar
 * uint16_t buffers with a row stride that may exceed cols * 2.
 */
#ifndef MINSCAN_CAPI_H_
#define MINSCAN_CAPI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct MsSession MsSession;

enum MsMedia
{
    MS_API_MEDIA_35MM = 0,
    MS_API_MEDIA_120  = 1,
};

enum MsFilm
{
    MS_API_FILM_COLOUR_NEG = 0,
    MS_API_FILM_COLOUR_POS = 1,
    MS_API_FILM_MONO       = 2,
};

enum MsPhase
{
    MS_API_PHASE_CALIBRATE = 0,
    MS_API_PHASE_FOCUS     = 1,
    MS_API_PHASE_SCAN      = 2,
};

typedef struct MsCarrier
{
    int    media;       ///< @ref MsMedia
    int    film;        ///< @ref MsFilm
    int    format_code; ///< 0 = default for media
    int    glassless;   ///< HA-P2 insert (120 only)
    double ev;          ///< exposure offset in stops
} MsCarrier;

typedef struct MsRequest
{
    int    dpi;
    int    frame_group; ///< 1-based group; 0 = current
    int    slot;        ///< 35mm only: 0 = A, 1 = B
    int    autofocus;
    int    capture_ir; ///< acquire the IR plane (4-plane read)
    int    ice;        ///< run minscan's ICE (otherwise raw IR is returned)
    int    prescan;    ///< hardware prescan/index pass (no arming, fixed div 8)
    int    glassless;  ///< 120 only: HA-P2 glassless insert
    double ev;
} MsRequest;

/** @brief progress callback: phase, done, total */
typedef void (*MsProgressFn)(void *ctx, int phase, int done, int total);

/** @brief probe a device without keeping it open (enumeration)
 *
 * @p media receives @ref MsMedia when a carrier is detected, or -1 when no
 * usable carrier is loaded. It may be NULL.
 */
int ms_probe(const char *device, int *media, char *err, size_t errlen);

/** @brief open the transport; configure before scanning */
MsSession *ms_open(const char *device, char *err, size_t errlen);

/** @brief close the transport and free the last frame */
void ms_close(MsSession *session);

/** @brief bind a carrier and read its pages; call again after a carrier switch */
int ms_configure(MsSession *session, const MsCarrier *carrier, char *err, size_t errlen);

/** @brief force the vendor calibration ladder (normally automatic on first scan) */
int ms_calibrate(MsSession *session, MsProgressFn progress, void *ctx, char *err, size_t errlen);

/** @brief acquire and process one frame; the result stays in the session */
int ms_scan(MsSession *session, const MsRequest *request, MsProgressFn progress, void *ctx, char *err, size_t errlen);

/** @brief request cancellation from a progress callback or another thread */
void ms_cancel(MsSession *session);

/** @brief geometry of the last frame; row_stride is in bytes */
int ms_frame_geometry(const MsSession *session, int *rows, int *cols, int *planes, size_t *row_stride);

/** @brief plane pointer (R=0, G=1, B=2, IR=3), valid until the next scan/close */
const uint16_t *ms_frame_plane(const MsSession *session, int plane);

/** @brief free the last frame */
void ms_frame_free(MsSession *session);

/** @brief eject the carrier (31 00); returns 0 on success */
int ms_eject(MsSession *session, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif // MINSCAN_CAPI_H_
