#include <config.h>

#include <string.h>
#include "track.h"
#include "um.h"

G_DEFINE_TYPE(JackDawTrack, jackdaw_track, G_TYPE_OBJECT)

enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_ROUTING_CHANGED,
    LAST_SIGNAL
};

static guint track_signals[LAST_SIGNAL];

/* ---- GObject boilerplate ---- */

static void jackdaw_track_finalize(GObject *obj)
{
    JackDawTrack *t = JACKDAW_TRACK(obj);

    g_free(t->name);
    audio_clip_free(t->clip);

    if (t->play_buf_L)   jack_ringbuffer_free(t->play_buf_L);
    if (t->play_buf_R)   jack_ringbuffer_free(t->play_buf_R);
    if (t->rec_buf_L)    jack_ringbuffer_free(t->rec_buf_L);
    if (t->rec_buf_R)    jack_ringbuffer_free(t->rec_buf_R);
    if (t->midi_rec_buf) jack_ringbuffer_free(t->midi_rec_buf);

    G_OBJECT_CLASS(jackdaw_track_parent_class)->finalize(obj);
}

static void jackdaw_track_class_init(JackDawTrackClass *klass)
{
    GObjectClass *gc = G_OBJECT_CLASS(klass);
    gc->finalize = jackdaw_track_finalize;

    track_signals[SIGNAL_STATE_CHANGED] = g_signal_new(
        "state-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST, G_STRUCT_OFFSET(JackDawTrackClass, state_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 0);

    track_signals[SIGNAL_ROUTING_CHANGED] = g_signal_new(
        "routing-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST, G_STRUCT_OFFSET(JackDawTrackClass, routing_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void jackdaw_track_init(JackDawTrack *t)
{
    t->name          = NULL;
    t->slot          = G_MAXUINT;
    t->clip          = NULL;
    t->audio_in_idx  = -1;
    t->midi_in_idx   = -1;
    t->state_flags   = 0;
    t->volume        = 1.0f;
    t->pan           = 0.0f;
    t->peak_L        = 0.0f;
    t->peak_R        = 0.0f;
    t->play_buf_L    = NULL;
    t->play_buf_R    = NULL;
    t->rec_buf_L     = NULL;
    t->rec_buf_R     = NULL;
    t->midi_rec_buf  = NULL;
    t->played_frames = 0;
    t->rt_chain      = NULL;
}

/* ---- Constructor ---- */

JackDawTrack *jackdaw_track_new(const gchar *name, AudioClip *clip)
{
    JackDawTrack *t = g_object_new(JACKDAW_TYPE_TRACK, NULL);

    t->name = g_strdup(name ? name : "Track");
    t->clip = clip;  /* take ownership */

    /* Audio ringbuffers are allocated by jackdaw_engine_add_track() once the
     * JACK sample rate is known. They remain NULL until then. */

    return t;
}

/* ---- Accessors ---- */

const gchar *jackdaw_track_get_name(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->name;
}

void jackdaw_track_set_name(JackDawTrack *t, const gchar *name)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    g_free(t->name);
    t->name = g_strdup(name ? name : "Track");
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

AudioClip *jackdaw_track_get_clip(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->clip;
}

void jackdaw_track_set_clip(JackDawTrack *t, AudioClip *new_clip)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    audio_clip_free(t->clip);
    t->clip = new_clip;  /* take ownership */
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

/* ---- State flags ---- */

void jackdaw_track_set_armed(JackDawTrack *t, gboolean armed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (armed)
        g_atomic_int_or(&t->state_flags, TRACK_ARMED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_ARMED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_muted(JackDawTrack *t, gboolean muted)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (muted)
        g_atomic_int_or(&t->state_flags, TRACK_MUTED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_MUTED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (soloed)
        g_atomic_int_or(&t->state_flags, TRACK_SOLOED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_SOLOED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_armed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_ARMED) != 0;
}

gboolean jackdaw_track_is_muted(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_MUTED) != 0;
}

gboolean jackdaw_track_is_soloed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED) != 0;
}

/* ---- Volume / pan ---- */

void jackdaw_track_set_volume(JackDawTrack *t, gfloat vol)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->volume = CLAMP(vol, 0.0f, 2.0f);
}

gfloat jackdaw_track_get_volume(JackDawTrack *t)
{
    return t->volume;
}

void jackdaw_track_set_pan(JackDawTrack *t, gfloat pan)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->pan = CLAMP(pan, -1.0f, 1.0f);
}

gfloat jackdaw_track_get_pan(JackDawTrack *t)
{
    return t->pan;
}

/* ---- Input routing ---- */

void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->audio_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

void jackdaw_track_set_midi_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->midi_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

/* ---- Peak metering ---- */

void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    /* Racy read is acceptable: worst case shows one extra frame of peak. */
    if (out_L) { *out_L = t->peak_L; t->peak_L = 0.0f; }
    if (out_R) { *out_R = t->peak_R; t->peak_R = 0.0f; }
}
