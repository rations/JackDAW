#ifndef CLIPREGION_H_INCLUDED
#define CLIPREGION_H_INCLUDED

#include <glib.h>
#include <sys/types.h>
#include "audio_clip.h"

G_BEGIN_DECLS

/*
 * ClipRegion — one placement of (part of) a source audio file on a track's
 * timeline.  A track owns an ordered (by tl_pos) list of these.
 *
 * Frame conventions:
 *   tl_pos   — timeline start, in TIMELINE frames (= JACK sample rate)
 *   length   — duration on the timeline, in TIMELINE frames
 *   file_in  — first frame to read from the source file, in CLIP-FILE frames
 *              (= clip->info.samplerate)
 *
 * When clip->info.samplerate == JACK SR (the recording case) the two frame
 * domains are identical and all arithmetic is exact.  For imported files at a
 * different rate the feeder resamples; region boundaries are then converted
 * with the clip/jack sample-rate ratio.
 */
typedef struct {
    AudioClip *clip;     /* shared source (holds its own ref) */
    off_t      file_in;  /* in-point within the source file (clip frames) */
    off_t      length;   /* duration on the timeline (timeline frames) */
    off_t      tl_pos;   /* timeline start (timeline frames) */
    gfloat     gain;     /* per-region linear gain (default 1.0) */
} ClipRegion;

ClipRegion *clip_region_new (AudioClip *clip, off_t file_in,
                             off_t length, off_t tl_pos);
ClipRegion *clip_region_copy(const ClipRegion *r);
void        clip_region_free(ClipRegion *r);   /* GDestroyNotify-compatible */

static inline off_t clip_region_end(const ClipRegion *r)
{
    return r->tl_pos + r->length;
}

/* ---- Region list (GPtrArray of ClipRegion*, free func = clip_region_free) ---- */

GPtrArray  *clip_region_list_new (void);
GPtrArray  *clip_region_list_copy(GPtrArray *list);
void        clip_region_list_sort(GPtrArray *list);   /* by tl_pos ascending */

/* Last timeline frame covered by any region (0 if empty). */
off_t       clip_region_list_total_frames(GPtrArray *list);

/* Region covering timeline `frame`, or NULL if in a gap / past the end. */
ClipRegion *clip_region_list_at(GPtrArray *list, off_t frame);

/* ---- Edit operations (mutate the list in place) ----
 * jack_sr / clip_sr ratios are taken from each region's clip->info; the caller
 * passes the current JACK sample rate so timeline↔file conversions are exact.
 */
void clip_region_list_split_at    (GPtrArray *list, off_t frame, int jack_sr);
void clip_region_list_delete_range(GPtrArray *list, off_t a, off_t b, int jack_sr);
void clip_region_list_set_gain_range(GPtrArray *list, off_t a, off_t b,
                                     gfloat gain, int jack_sr);
void clip_region_list_remove_at   (GPtrArray *list, off_t frame);

/* Merge selected regions back into single sections ("Group").  Selected regions
 * are identified by their tl_pos (passed in `sel_tlpos`, `n` entries).  Adjacent
 * selected regions that share a source clip and are file-contiguous are merged
 * (same-clip regions are pulled flush, closing any gap, then merged); regions
 * from different clips, or separated by a non-selected region, are left as-is. */
void clip_region_list_group(GPtrArray *list, const off_t *sel_tlpos, guint n,
                            int jack_sr);

/* ---- Immutable snapshot for the feeder thread ----
 * Built on the main thread under the track's region lock; the feeder takes a
 * ref, uses it for a cycle, then unrefs.  Each entry's clip carries a ref. */
typedef struct {
    gint        refcount;
    int         n;
    ClipRegion *r;       /* contiguous array of n value copies */
} ClipRegionSnapshot;

ClipRegionSnapshot *clip_region_snapshot_new  (GPtrArray *list);
ClipRegionSnapshot *clip_region_snapshot_ref  (ClipRegionSnapshot *s);
void                clip_region_snapshot_unref(ClipRegionSnapshot *s);

G_END_DECLS

#endif /* CLIPREGION_H_INCLUDED */
