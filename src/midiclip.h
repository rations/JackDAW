#ifndef MIDICLIP_H_INCLUDED
#define MIDICLIP_H_INCLUDED

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/*
 * MIDI data model.
 *
 * Note timing is stored in musical TICKS at JACKDAW_PPQ ticks per quarter note,
 * so notes follow tempo changes (industry standard). Region placement (tl_pos)
 * stays in TIMELINE FRAMES, uniform with audio regions / the playhead / zoom.
 * Ticks are converted to frames at snapshot-build time using the project's
 * frames-per-beat (= sample_rate * 60 / bpm).
 */
#define JACKDAW_PPQ 960

/* One note. start/length in ticks; pitch/velocity 0..127; channel 0..15. */
typedef struct {
    guint32 start;
    guint32 length;
    guint8  pitch;
    guint8  velocity;
    guint8  channel;
} MidiNote;

/* A shared, ref-counted sequence of notes (mirrors AudioClip's discipline). */
typedef struct {
    GArray  *notes;     /* array of MidiNote (unsorted; snapshot sorts a copy) */
    guint32  length;    /* nominal clip length in ticks */
    gint     refcount;  /* shared across regions; new = 1 */
} MidiClip;

MidiClip *midi_clip_new (guint32 length_ticks);
MidiClip *midi_clip_ref (MidiClip *c);          /* NULL-safe */
void      midi_clip_free(MidiClip *c);          /* unref; frees at zero */
/* Deep copy: a brand-new clip (refcount 1) with every note duplicated, fully
 * independent of the source. Unlike midi_clip_ref (which shares the notes),
 * this is used for undo mementos. NULL-safe (returns NULL). */
MidiClip *midi_clip_copy(MidiClip *c);

/* Editing (main thread). add returns the new note's index. */
guint     midi_clip_add_note   (MidiClip *c, MidiNote note);
void      midi_clip_remove_note(MidiClip *c, guint index);
guint     midi_clip_note_count (MidiClip *c);
MidiNote *midi_clip_note        (MidiClip *c, guint index); /* borrowed, mutable */

/*
 * MidiRegion — one placement of (part of) a MidiClip on a track's timeline.
 *   tl_pos   — timeline start, in TIMELINE frames
 *   clip_in  — first tick of the clip that plays (window start), in ticks
 *   length   — window duration, in ticks
 */
typedef struct {
    MidiClip *clip;     /* shared source (holds its own ref) */
    guint32   clip_in;  /* in-point within the clip (ticks) */
    guint32   length;   /* window length (ticks) */
    off_t     tl_pos;   /* timeline start (frames) */
} MidiRegion;

MidiRegion *midi_region_new (MidiClip *clip, guint32 clip_in,
                             guint32 length, off_t tl_pos);
void        midi_region_free(MidiRegion *r);    /* GDestroyNotify-compatible */

/* Region list = GPtrArray of MidiRegion*, free func = midi_region_free. */
GPtrArray  *midi_region_list_new(void);

/*
 * Immutable RT event snapshot.
 *
 * Read by the JACK process callback, so it is published lock-free via the
 * track's atomic pointer + deferred-retire pattern (NOT ref-counted/mutexed):
 * the RT thread only reads it and never frees it. Each note is expanded into a
 * note-on and note-off event with an absolute timeline FRAME, sorted by frame.
 */
typedef struct {
    off_t  frame;     /* absolute timeline frame */
    guint8 s, d1, d2; /* status byte, data1, data2 */
} MidiSnapEvent;

typedef struct {
    guint          n;
    MidiSnapEvent *ev;   /* contiguous, sorted by frame ascending */
} MidiEventSnapshot;

MidiEventSnapshot *midi_event_snapshot_new (MidiClip *clip,
                                            double frames_per_beat);
void               midi_event_snapshot_free(MidiEventSnapshot *s);

/* Last event frame (0 if empty). */
off_t midi_event_snapshot_total_frames(const MidiEventSnapshot *s);

G_END_DECLS

#endif /* MIDICLIP_H_INCLUDED */
