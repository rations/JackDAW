#ifndef TRACK_H_INCLUDED
#define TRACK_H_INCLUDED

#include <glib-object.h>
#include <jack/ringbuffer.h>
#include "audio_clip.h"

G_BEGIN_DECLS

#define JACKDAW_TYPE_TRACK (jackdaw_track_get_type())
#define JACKDAW_TRACK(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST(obj, JACKDAW_TYPE_TRACK, JackDawTrack))
#define JACKDAW_IS_TRACK(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE(obj, JACKDAW_TYPE_TRACK))

/* State flag bits — written by main thread, read by RT callback */
#define TRACK_ARMED  (1 << 0)
#define TRACK_MUTED  (1 << 1)
#define TRACK_SOLOED (1 << 2)

/* Max tracks the engine supports */
#define JACKDAW_MAX_TRACKS 64

/* MIDI ringbuffer fixed size — event-based, not sample-rate-dependent */
#define TRACK_MIDI_RINGBUF_BYTES 65536

typedef struct _JackDawTrack      JackDawTrack;
typedef struct _JackDawTrackClass JackDawTrackClass;

struct _JackDawTrack {
    GObject  parent_instance;

    gchar     *name;
    guint      slot;          /* index in engine track slot array */
    AudioClip *clip;          /* audio data; track owns this — may be NULL */

    /* Input routing — indices into engine port arrays; -1 = none */
    gint     audio_in_idx;
    gint     midi_in_idx;

    /* RT-safe state: written atomically by main thread */
    volatile gint32  state_flags;
    volatile gfloat  volume;    /* 0.0 – 2.0, unity = 1.0 */
    volatile gfloat  pan;       /* -1.0 (L) … 0.0 (C) … 1.0 (R) */

    /* Peak metering: written by RT callback, read by main thread */
    volatile gfloat  peak_L;
    volatile gfloat  peak_R;

    /* Playback ringbuffers: fed by main thread, drained by RT callback */
    jack_ringbuffer_t *play_buf_L;
    jack_ringbuffer_t *play_buf_R;

    /* Record ringbuffers: filled by RT callback, drained by main thread */
    jack_ringbuffer_t *rec_buf_L;
    jack_ringbuffer_t *rec_buf_R;
    jack_ringbuffer_t *midi_rec_buf;

    /* Playback position in frames within clip */
    volatile off_t played_frames;

    /* External JACK ports connected to this track's input ports (main-thread only).
     * NULL = no connection established by jackdaw. */
    gchar *audio_src_port;
    gchar *midi_src_port;

    /* RT plugin chain pointer — swapped atomically (Phase 5) */
    volatile gpointer rt_chain;
};

struct _JackDawTrackClass {
    GObjectClass parent_class;

    void (*state_changed)  (JackDawTrack *track);
    void (*routing_changed)(JackDawTrack *track);
};

GType        jackdaw_track_get_type(void);

/* Constructor — track takes ownership of clip (may be NULL) */
JackDawTrack *jackdaw_track_new(const gchar *name, AudioClip *clip);

/* Accessors (main-thread safe) */
const gchar *jackdaw_track_get_name (JackDawTrack *t);
void         jackdaw_track_set_name (JackDawTrack *t, const gchar *name);

/* Returns borrowed pointer — do not free */
AudioClip   *jackdaw_track_get_clip (JackDawTrack *t);
/* Track takes ownership of new_clip; frees old clip */
void         jackdaw_track_set_clip (JackDawTrack *t, AudioClip *new_clip);

/* State flag helpers — use g_atomic_int_or/and for thread safety */
void  jackdaw_track_set_armed (JackDawTrack *t, gboolean armed);
void  jackdaw_track_set_muted (JackDawTrack *t, gboolean muted);
void  jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed);
gboolean jackdaw_track_is_armed (JackDawTrack *t);
gboolean jackdaw_track_is_muted (JackDawTrack *t);
gboolean jackdaw_track_is_soloed(JackDawTrack *t);

/* Volume/pan — stored as volatile float, main thread writes, RT reads */
void   jackdaw_track_set_volume(JackDawTrack *t, gfloat vol);
gfloat jackdaw_track_get_volume(JackDawTrack *t);
void   jackdaw_track_set_pan   (JackDawTrack *t, gfloat pan);
gfloat jackdaw_track_get_pan   (JackDawTrack *t);

/* Input routing */
void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx);
void jackdaw_track_set_midi_in (JackDawTrack *t, gint idx);

/* Peak meter read (call from main thread, resets after read) */
void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R);

G_END_DECLS

#endif /* TRACK_H_INCLUDED */
