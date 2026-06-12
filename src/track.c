#include <config.h>

#include <string.h>
#include "track.h"
#include "jackdaw-engine.h"
#include "pluginhost.h"
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
    g_free(t->audio_src_port);
    g_free(t->midi_src_port);

    if (t->regions) g_ptr_array_unref(t->regions);
    if (t->rt_snapshot) clip_region_snapshot_unref(t->rt_snapshot);
    g_mutex_clear(&t->region_lock);

    /* MIDI: drop the live snapshot + any retired ones, then the region list. */
    if (t->rt_midi) midi_event_snapshot_free((MidiEventSnapshot *)t->rt_midi);
    if (t->retire_midi) {
        for (guint i = 0; i < t->retire_midi->len; i++)
            midi_event_snapshot_free(g_ptr_array_index(t->retire_midi, i));
        g_ptr_array_free(t->retire_midi, TRUE);
    }
    if (t->midi_regions) g_ptr_array_free(t->midi_regions, TRUE);

    /* Tear down FX: drop the live chain, then free every instance/chain. */
    JackDawFxChain *live = t->rt_chain;
    t->rt_chain = NULL;
    if (live) { g_free(live->fx); g_free(live); }
    if (t->retire_chains) {
        for (guint i = 0; i < t->retire_chains->len; i++) {
            JackDawFxChain *c = g_ptr_array_index(t->retire_chains, i);
            g_free(c->fx); g_free(c);
        }
        g_ptr_array_free(t->retire_chains, TRUE);
    }
    if (t->retire_fx) {
        for (guint i = 0; i < t->retire_fx->len; i++)
            pluginhost_free(g_ptr_array_index(t->retire_fx, i));
        g_ptr_array_free(t->retire_fx, TRUE);
    }
    if (t->fx_list) {
        for (guint i = 0; i < t->fx_list->len; i++)
            pluginhost_free(g_ptr_array_index(t->fx_list, i));
        g_ptr_array_free(t->fx_list, TRUE);
    }

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
    t->kind          = JACKDAW_TRACK_AUDIO;
    t->midi_regions  = midi_region_list_new();
    t->rt_midi       = NULL;
    t->retire_midi   = g_ptr_array_new();
    t->regions       = clip_region_list_new();
    g_mutex_init(&t->region_lock);
    t->rt_snapshot   = clip_region_snapshot_new(t->regions);
    t->audio_in_idx  = -1;
    t->midi_in_idx   = -1;
    t->audio_src_port = NULL;
    t->midi_src_port  = NULL;
    t->state_flags   = 0;
    t->volume        = 1.0f;
    t->pan           = 0.0f;
    t->peak_L        = 0.0f;
    t->peak_R        = 0.0f;
    t->play_buf_L      = NULL;
    t->play_buf_R      = NULL;
    t->rec_buf_L       = NULL;
    t->rec_buf_R       = NULL;
    t->midi_rec_buf    = NULL;
    t->played_frames   = 0;
    t->rec_start_frame = 0;
    t->rec_latency     = 0;
    t->mono_record     = TRUE;
    t->clip_start      = 0;
    t->rec_peak_buf    = NULL;
    t->rec_peak_count  = 0;
    t->rec_peak_block  = 0;
    t->fx_list         = g_ptr_array_new();
    t->rt_chain        = NULL;
    t->retire_chains   = g_ptr_array_new();
    t->retire_fx       = g_ptr_array_new();
}

/* ---- Constructor ---- */

JackDawTrack *jackdaw_track_new(const gchar *name, AudioClip *clip)
{
    JackDawTrack *t = g_object_new(JACKDAW_TYPE_TRACK, NULL);

    t->name = g_strdup(name ? name : "Track");

    /* If an initial clip is supplied, place it as a single region at tl=0.
     * jackdaw_track_set_clip consumes one reference, matching the old
     * "take ownership" contract of this constructor. */
    if (clip)
        jackdaw_track_set_clip(t, clip);

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
    if (!t->regions || t->regions->len == 0) return NULL;
    ClipRegion *r = g_ptr_array_index(t->regions, 0);
    return r->clip;
}

/* Region duration on the timeline (timeline frames) for a whole file. */
static off_t clip_timeline_length(AudioClip *clip)
{
    if (!clip) return 0;
    int jack_sr = (int)jackdaw_engine_get_sample_rate();
    int clip_sr = clip->info.samplerate;
    if (clip_sr <= 0 || jack_sr <= 0 || clip_sr == jack_sr)
        return clip->info.frames;
    return (off_t)((double)clip->info.frames *
                   (double)jack_sr / (double)clip_sr + 0.5);
}

void jackdaw_track_place_clip(JackDawTrack *t, AudioClip *clip, off_t tl_pos)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (!clip) return;
    ClipRegion *r = clip_region_new(clip, 0,
                                    clip_timeline_length(clip), tl_pos);
    g_ptr_array_add(t->regions, r);
    clip_region_list_sort(t->regions);
    audio_clip_free(clip);  /* consume caller's reference */
    jackdaw_track_commit_regions(t);
}

void jackdaw_track_set_clip(JackDawTrack *t, AudioClip *new_clip)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (t->regions->len > 0)
        g_ptr_array_remove_range(t->regions, 0, t->regions->len);
    if (new_clip)
        jackdaw_track_place_clip(t, new_clip, t->clip_start);
    else
        jackdaw_track_commit_regions(t);
}

GPtrArray *jackdaw_track_get_regions(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->regions;
}

/* ---- Track kind / MIDI ---- */

JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), JACKDAW_TRACK_AUDIO);
    return t->kind;
}

void jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->kind = kind;
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_instrument(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), FALSE);
    return t->kind == JACKDAW_TRACK_INSTRUMENT;
}

GPtrArray *jackdaw_track_get_midi_regions(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->midi_regions;
}

/* Publish a fresh MIDI event snapshot for the RT thread. Mirrors
 * track_publish_chain: reclaim the PREVIOUS edit's retired snapshot (the RT
 * thread has moved past it), build the new one, atomic-swap, retire the old. */
void jackdaw_track_commit_midi(JackDawTrack *t, double frames_per_beat)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));

    for (guint i = 0; i < t->retire_midi->len; i++)
        midi_event_snapshot_free(g_ptr_array_index(t->retire_midi, i));
    g_ptr_array_set_size(t->retire_midi, 0);

    MidiEventSnapshot *ns = midi_event_snapshot_new(t->midi_regions,
                                                    frames_per_beat);
    MidiEventSnapshot *old = t->rt_midi;
    g_atomic_pointer_set(&t->rt_midi, ns);
    if (old) g_ptr_array_add(t->retire_midi, old);

    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_commit_regions(JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    clip_region_list_sort(t->regions);
    ClipRegionSnapshot *snap = clip_region_snapshot_new(t->regions);
    g_mutex_lock(&t->region_lock);
    ClipRegionSnapshot *old = t->rt_snapshot;
    t->rt_snapshot = snap;
    g_mutex_unlock(&t->region_lock);
    if (old) clip_region_snapshot_unref(old);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

off_t jackdaw_track_total_frames(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 0);
    return clip_region_list_total_frames(t->regions);
}

ClipRegionSnapshot *jackdaw_track_ref_snapshot(JackDawTrack *t)
{
    ClipRegionSnapshot *s;
    g_mutex_lock(&t->region_lock);
    s = clip_region_snapshot_ref(t->rt_snapshot);
    g_mutex_unlock(&t->region_lock);
    return s;
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
    t->volume = CLAMP(vol, 0.0f, 18.0f); /* 18.0 ≈ linear gain for +25 dB */
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
    /* Non-destructive read: the RT callback applies decay-hold, so multiple
     * meters (track strip + mixer) can poll this concurrently. */
    if (out_L) *out_L = t->peak_L;
    if (out_R) *out_R = t->peak_R;
}

/* ---- FX chain ---- */

/* Reclaim chains/instances retired by the PREVIOUS edit (the RT thread has had
 * many cycles to move past them by now), then publish a fresh chain built from
 * fx_list and retire the old one. */
static void track_publish_chain(JackDawTrack *t)
{
    for (guint i = 0; i < t->retire_chains->len; i++) {
        JackDawFxChain *c = g_ptr_array_index(t->retire_chains, i);
        g_free(c->fx); g_free(c);
    }
    g_ptr_array_set_size(t->retire_chains, 0);
    for (guint i = 0; i < t->retire_fx->len; i++)
        pluginhost_free(g_ptr_array_index(t->retire_fx, i));
    g_ptr_array_set_size(t->retire_fx, 0);

    JackDawFxChain *nc = g_new0(JackDawFxChain, 1);
    nc->n = (int)t->fx_list->len;
    if (nc->n > 0) {
        nc->fx = g_new0(gpointer, nc->n);
        for (int i = 0; i < nc->n; i++)
            nc->fx[i] = g_ptr_array_index(t->fx_list, i);
    }

    JackDawFxChain *old = t->rt_chain;
    g_atomic_pointer_set(&t->rt_chain, nc);
    if (old) g_ptr_array_add(t->retire_chains, old);
}

void jackdaw_track_fx_add(JackDawTrack *t, gpointer instance)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (!instance) return;
    g_ptr_array_add(t->fx_list, instance);
    track_publish_chain(t);
}

void jackdaw_track_fx_remove(JackDawTrack *t, guint index)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (index >= t->fx_list->len) return;
    gpointer inst = g_ptr_array_index(t->fx_list, index);
    g_ptr_array_remove_index(t->fx_list, index);
    track_publish_chain(t);
    /* Defer the instance free until the next edit so the retired chain that
     * still references it is no longer read by the RT thread. */
    g_ptr_array_add(t->retire_fx, inst);
}

void jackdaw_track_fx_move(JackDawTrack *t, guint from, guint to)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (from >= t->fx_list->len || to >= t->fx_list->len || from == to) return;
    gpointer inst = g_ptr_array_index(t->fx_list, from);
    g_ptr_array_remove_index(t->fx_list, from);
    g_ptr_array_insert(t->fx_list, (gint)to, inst);
    track_publish_chain(t);
}

guint jackdaw_track_fx_count(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 0);
    return t->fx_list->len;
}

gpointer jackdaw_track_fx_get(JackDawTrack *t, guint index)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return index < t->fx_list->len ? g_ptr_array_index(t->fx_list, index) : NULL;
}
