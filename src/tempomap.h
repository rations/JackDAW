#ifndef TEMPOMAP_H_INCLUDED
#define TEMPOMAP_H_INCLUDED

#include <glib.h>
#include <sys/types.h> /* off_t */

#include "project.h"

G_BEGIN_DECLS

/*
 * tempomap: the single authority for sample <-> beat <-> bar/beat/tick <->
 * seconds conversion, shared by the ruler, snap-to-grid, the metronome and
 * the transport readout so they always agree on the grid.
 *
 * Phase 1 is a fixed tempo + fixed time signature. Callers fill a TempoMap
 * value from the project (tempomap_from_project) and pass it to the pure
 * helpers below; when a real multi-segment tempo map lands, only this module
 * and that fill-in change. All helpers are allocation-free and RT-callable.
 */

/* Ticks per quarter note (matches the MIDI clip resolution). */
#define TEMPOMAP_PPQ 960

typedef struct {
    gdouble bpm;         /* > 0 */
    guint beats_per_bar; /* time-sig numerator, >= 1 */
    guint beat_unit;     /* time-sig denominator (display only for now) */
    guint32 sample_rate; /* JACK sample rate, > 0 */
} TempoMap;

/* Snap/grid resolution. Divisions subdivide one beat. */
typedef enum {
    TEMPOMAP_GRID_BAR = 0,
    TEMPOMAP_GRID_BEAT,
    TEMPOMAP_GRID_BEAT_DIV2,
    TEMPOMAP_GRID_BEAT_DIV3, /* triplets */
    TEMPOMAP_GRID_BEAT_DIV4,
    TEMPOMAP_GRID_LAST
} TempoMapGrid;

/* Musical position of a frame. bar/beat are 1-based for display; tick is
 * 0..TEMPOMAP_PPQ*4/beat_unit-1 within the beat. */
typedef struct {
    guint bar;
    guint beat;
    guint tick;
} TempoMapBBT;

/* Fill from the project's tempo fields (main/window thread only — reads
 * non-atomic gdouble fields the way the Linux engine does). */
void tempomap_from_project(TempoMap *tm, JackDawProject *p, guint32 sample_rate);

gdouble tempomap_frames_per_beat(const TempoMap *tm);
gdouble tempomap_frames_per_bar(const TempoMap *tm);

gdouble tempomap_frame_to_beat(const TempoMap *tm, off_t frame);
off_t tempomap_beat_to_frame(const TempoMap *tm, gdouble beat);
gdouble tempomap_frame_to_seconds(const TempoMap *tm, off_t frame);
void tempomap_frame_to_bbt(const TempoMap *tm, off_t frame, TempoMapBBT *out);

/* Frame step of one grid unit (bar, beat or beat division). */
gdouble tempomap_grid_frames(const TempoMap *tm, TempoMapGrid grid);

/* Snap a timeline frame to the nearest grid line (unconditionally — the
 * caller decides whether snap is enabled). Never returns < 0. */
off_t tempomap_snap_frame(const TempoMap *tm, off_t frame, TempoMapGrid grid);

/* Short display name for a grid unit ("Bar", "Beat", "1/2 Beat", …). */
const gchar *tempomap_grid_name(TempoMapGrid grid);

G_END_DECLS

#endif /* TEMPOMAP_H_INCLUDED */
