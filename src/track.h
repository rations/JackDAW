#ifndef TRACK_H_INCLUDED
#define TRACK_H_INCLUDED

#include <glib-object.h>
#include <jack/ringbuffer.h>
#include "audio_clip.h"
#include "clipregion.h"
#include "midiclip.h"

G_BEGIN_DECLS

/* Track kind: an audio track streams AudioClip regions; an instrument track
 * sequences MidiRegions into its first FX-chain plugin (the instrument). */
typedef enum {
    JACKDAW_TRACK_AUDIO = 0,
    JACKDAW_TRACK_INSTRUMENT
} JackDawTrackKind;

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
    JackDawTrackKind kind;    /* audio (default) or instrument */

    /* MIDI clip (instrument tracks; main-thread). Notes stored at absolute tick
     * positions (tick 0 = timeline frame 0). The RT callback reads the immutable
     * rt_midi snapshot, published lock-free like rt_chain. */
    MidiClip          *midi_clip;     /* single clip for the whole timeline */
    gpointer           rt_midi;       /* MidiEventSnapshot* (atomic) */
    GPtrArray         *retire_midi;   /* MidiEventSnapshot* awaiting free */

    /* Timeline clip regions (main-thread list, ordered by tl_pos).
     * The feeder thread never touches this directly — it reads the immutable
     * rt_snapshot under region_lock. */
    GPtrArray          *regions;     /* GPtrArray of ClipRegion* */
    GMutex              region_lock; /* guards rt_snapshot (main ↔ feeder) */
    ClipRegionSnapshot *rt_snapshot; /* current snapshot for the feeder */

    /* Input routing — indices into engine port arrays; -1 = none */
    gint     audio_in_idx;
    gint     midi_in_idx;

    /* RT-safe state: written atomically by main thread */
    volatile gint32  state_flags;
    volatile gfloat  volume;    /* EFFECTIVE gain (= trim * fader), RT reads this */
    volatile gfloat  trim;      /* input trim gain (track strip dial) */
    volatile gfloat  fader;     /* channel fader gain (mixer fader) */
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

    /* Recording state — main thread only */
    off_t    rec_start_frame;  /* transport pos when Record was pressed */
    off_t    rec_latency;      /* JACK capture latency at record start (frames) */
    gboolean mono_record;      /* TRUE = 1-ch WAV (default), FALSE = stereo */

    /* Timeline position of the most recent recording (sample offset from 0).
     * Kept for the live recording overlay; finalised audio lives in regions. */
    off_t    clip_start;

    /* Real-time waveform peak buffer.
     * Allocated (main thread) at rec start, freed after clip finalized.
     * RT callback writes; main thread (wave view draw) reads. */
    gfloat        *rec_peak_buf;    /* interleaved [min,max] pairs per JACK period */
    volatile gint  rec_peak_count;  /* number of valid pairs written so far */
    guint          rec_peak_block;  /* samples per bucket (= jack_get_buffer_size) */

    /* External JACK ports connected to this track's input ports (main-thread only).
     * NULL = no connection established by jackdaw.
     * audio_src_port = left channel source; audio_src_port_r = right channel
     * source (NULL when the track input is mono). */
    gchar *audio_src_port;
    gchar *audio_src_port_r;
    gchar *midi_src_port;

    /* FX: main-thread list of PluginInstance* (effects on this track). */
    GPtrArray *fx_list;

    /* Immutable FX chain snapshot read by the RT callback (JackDawFxChain*),
     * published with g_atomic_pointer_set. Old chains/instances are reclaimed
     * on the next edit (deferred so the RT thread is never left dangling). */
    gpointer rt_chain;
    GPtrArray *retire_chains;   /* JackDawFxChain* awaiting free */
    GPtrArray *retire_fx;       /* PluginInstance* awaiting free */
};

/* Immutable FX chain snapshot. fx[i] is a PluginInstance* (opaque here). */
typedef struct {
    int       n;
    gpointer *fx;
} JackDawFxChain;

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

/* Returns the first region's source clip, or NULL — borrowed, do not free.
 * Back-compat helper; new code iterates jackdaw_track_get_regions(). */
AudioClip   *jackdaw_track_get_clip (JackDawTrack *t);
/* Replace all regions with a single region holding new_clip placed at tl=0.
 * Consumes one reference to new_clip (mhwaveedit "take ownership" semantics). */
void         jackdaw_track_set_clip (JackDawTrack *t, AudioClip *new_clip);

/* ---- Clip regions (main thread only) ----
 * Place a clip as a new region at timeline position tl_pos, spanning the whole
 * file.  Consumes one reference to clip.  Rebuilds the feeder snapshot. */
void         jackdaw_track_place_clip(JackDawTrack *t, AudioClip *clip, off_t tl_pos);

/* Borrowed region list — edit in place, then call jackdaw_track_commit_regions(). */
GPtrArray   *jackdaw_track_get_regions(JackDawTrack *t);

/* ---- Track kind / MIDI (main thread) ---- */
JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t);
void             jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind);
gboolean         jackdaw_track_is_instrument(JackDawTrack *t);

/* Borrowed MIDI region list — edit in place (regions/notes), then call
 * jackdaw_track_commit_midi() to republish the RT event snapshot. */
MidiClip    *jackdaw_track_get_midi_clip(JackDawTrack *t);

/* Replace the track's MIDI clip wholesale (consumes `clip`; frees the old one)
 * and republish the RT event snapshot via the atomic swap. Used by undo to
 * restore a captured clip. frames_per_beat as for jackdaw_track_commit_midi. */
void         jackdaw_track_set_midi_clip(JackDawTrack *t, MidiClip *clip,
                                         double frames_per_beat);

/* Rebuild + publish the immutable RT MIDI event snapshot from midi_clip.
 * frames_per_beat = sample_rate * 60 / bpm (caller computes via the project).
 * Emits state-changed so timeline previews redraw. */
void         jackdaw_track_commit_midi(JackDawTrack *t, double frames_per_beat);

/* Rebuild the immutable feeder snapshot from the current region list and emit
 * state-changed so wave views redraw.  Call after any region-list edit. */
void         jackdaw_track_commit_regions(JackDawTrack *t);

/* Last timeline frame covered by any region (0 if empty). */
off_t        jackdaw_track_total_frames(JackDawTrack *t);

/* Feeder-thread access: take/drop a reference to the current snapshot.
 * ref locks region_lock briefly; the returned snapshot is stable until unref. */
ClipRegionSnapshot *jackdaw_track_ref_snapshot(JackDawTrack *t);

/* State flag helpers — use g_atomic_int_or/and for thread safety */
void  jackdaw_track_set_armed (JackDawTrack *t, gboolean armed);
void  jackdaw_track_set_muted (JackDawTrack *t, gboolean muted);
void  jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed);
gboolean jackdaw_track_is_armed (JackDawTrack *t);
gboolean jackdaw_track_is_muted (JackDawTrack *t);
gboolean jackdaw_track_is_soloed(JackDawTrack *t);

/* Volume/pan — stored as volatile float, main thread writes, RT reads */
/* Effective gain = trim * fader. set_volume() is legacy (sets the fader stage
 * with trim left at unity); get_volume() returns the effective product. */
void   jackdaw_track_set_volume(JackDawTrack *t, gfloat vol);
gfloat jackdaw_track_get_volume(JackDawTrack *t);

/* Two independent gain stages (gain staging): the track-strip dial drives the
 * trim; the mixer fader drives the fader. Both fold into the effective volume. */
void   jackdaw_track_set_trim (JackDawTrack *t, gfloat trim);
gfloat jackdaw_track_get_trim (JackDawTrack *t);
void   jackdaw_track_set_fader(JackDawTrack *t, gfloat fader);
gfloat jackdaw_track_get_fader(JackDawTrack *t);
void   jackdaw_track_set_pan   (JackDawTrack *t, gfloat pan);
gfloat jackdaw_track_get_pan   (JackDawTrack *t);

/* Input routing */
void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx);
void jackdaw_track_set_midi_in (JackDawTrack *t, gint idx);

/* Peak meter read (call from main thread, resets after read) */
void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R);

/* ---- FX chain (main thread) ----
 * Instances are PluginInstance* (from pluginhost.h); kept as gpointer here to
 * avoid pulling GTK/plugin headers into every track.h consumer. */
void      jackdaw_track_fx_add   (JackDawTrack *t, gpointer instance);
void      jackdaw_track_fx_remove(JackDawTrack *t, guint index);
void      jackdaw_track_fx_move  (JackDawTrack *t, guint from, guint to);
guint     jackdaw_track_fx_count (JackDawTrack *t);
gpointer  jackdaw_track_fx_get   (JackDawTrack *t, guint index);

G_END_DECLS

#endif /* TRACK_H_INCLUDED */
