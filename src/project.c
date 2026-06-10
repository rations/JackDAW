#include <config.h>
#include <math.h>

#include "project.h"
#include "settings.h"

G_DEFINE_TYPE(JackDawProject, jackdaw_project, G_TYPE_OBJECT)

enum {
    SIGNAL_TRACK_ADDED,
    SIGNAL_TRACK_REMOVED,
    SIGNAL_PORTS_CHANGED,
    SIGNAL_TIMING_CHANGED,
    LAST_SIGNAL
};

static guint project_signals[LAST_SIGNAL];

/* ---- GObject boilerplate ---- */

static void jackdaw_project_finalize(GObject *obj)
{
    JackDawProject *p = JACKDAW_PROJECT(obj);

    if (p->tracks) {
        g_ptr_array_unref(p->tracks);
        p->tracks = NULL;
    }
    g_free(p->project_file);

    G_OBJECT_CLASS(jackdaw_project_parent_class)->finalize(obj);
}

static void jackdaw_project_class_init(JackDawProjectClass *klass)
{
    GObjectClass *gc = G_OBJECT_CLASS(klass);
    gc->finalize = jackdaw_project_finalize;

    project_signals[SIGNAL_TRACK_ADDED] = g_signal_new(
        "track-added", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, track_added),
        NULL, NULL, NULL, G_TYPE_NONE, 1, JACKDAW_TYPE_TRACK);

    project_signals[SIGNAL_TRACK_REMOVED] = g_signal_new(
        "track-removed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, track_removed),
        NULL, NULL, NULL, G_TYPE_NONE, 1, JACKDAW_TYPE_TRACK);

    project_signals[SIGNAL_PORTS_CHANGED] = g_signal_new(
        "ports-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, ports_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 0);

    project_signals[SIGNAL_TIMING_CHANGED] = g_signal_new(
        "timing-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, timing_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void jackdaw_project_init(JackDawProject *p)
{
    p->tracks          = g_ptr_array_new_with_free_func(g_object_unref);
    p->project_file    = NULL;
    p->master_volume   = 1.0f;
    p->master_rt_chain = NULL;
    /* 0 = auto-detect from physical JACK ports at engine init */
    p->audio_in_count  = settings_get_uint32("jackAudioInCount",  0);
    p->audio_out_count = settings_get_uint32("jackAudioOutCount", 0);
    p->midi_in_count   = settings_get_uint32("jackMidiInCount",   0);
    p->midi_out_count  = settings_get_uint32("jackMidiOutCount",  0);

    p->bpm               = 120.0;
    p->beats_per_bar     = 4;
    p->beat_unit         = 4;
    p->grid_enabled      = FALSE;
    p->snap_enabled      = FALSE;
    p->metronome_enabled = FALSE;
    p->ruler_mode        = JACKDAW_RULER_TIME;
}

/* ---- Constructor ---- */

JackDawProject *jackdaw_project_new(void)
{
    return g_object_new(JACKDAW_TYPE_PROJECT, NULL);
}

/* ---- Track management ---- */

void jackdaw_project_add_track(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_return_if_fail(JACKDAW_IS_TRACK(t));

    g_ptr_array_add(p->tracks, g_object_ref(t));
    g_signal_emit(p, project_signals[SIGNAL_TRACK_ADDED], 0, t);
}

void jackdaw_project_remove_track(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_return_if_fail(JACKDAW_IS_TRACK(t));

    /* Hold a temporary ref so t stays valid while the signal is emitted.
     * g_ptr_array_remove() calls the free_func (g_object_unref) on removal,
     * and the "track-removed" handler may destroy the last external ref. */
    g_object_ref(t);
    if (g_ptr_array_remove(p->tracks, t))
        g_signal_emit(p, project_signals[SIGNAL_TRACK_REMOVED], 0, t);
    g_object_unref(t);
}

guint jackdaw_project_track_count(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 0);
    return p->tracks->len;
}

JackDawTrack *jackdaw_project_get_track(JackDawProject *p, guint idx)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    g_return_val_if_fail(idx < p->tracks->len, NULL);
    return JACKDAW_TRACK(g_ptr_array_index(p->tracks, idx));
}

/* ---- Master volume ---- */

void jackdaw_project_set_master_volume(JackDawProject *p, gfloat vol)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->master_volume = CLAMP(vol, 0.0f, 2.0f);
}

gfloat jackdaw_project_get_master_volume(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 1.0f);
    return p->master_volume;
}

/* ---- Project file ---- */

void jackdaw_project_set_file(JackDawProject *p, const gchar *path)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_free(p->project_file);
    p->project_file = g_strdup(path);
}

const gchar *jackdaw_project_get_file(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    return p->project_file;
}

/* ---- Ports changed ---- */

void jackdaw_project_emit_ports_changed(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_signal_emit(p, project_signals[SIGNAL_PORTS_CHANGED], 0);
}

/* ---- Tempo / grid ---- */

void jackdaw_project_emit_timing_changed(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_signal_emit(p, project_signals[SIGNAL_TIMING_CHANGED], 0);
}

void jackdaw_project_set_bpm(JackDawProject *p, gdouble bpm)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->bpm = CLAMP(bpm, 20.0, 999.0);
    jackdaw_project_emit_timing_changed(p);
}

gdouble jackdaw_project_get_bpm(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 120.0);
    return p->bpm;
}

void jackdaw_project_set_time_signature(JackDawProject *p, guint num, guint den)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->beats_per_bar = CLAMP(num, 1u, 32u);
    p->beat_unit     = CLAMP(den, 1u, 32u);
    jackdaw_project_emit_timing_changed(p);
}

void jackdaw_project_set_grid_enabled(JackDawProject *p, gboolean on)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->grid_enabled = on;
    jackdaw_project_emit_timing_changed(p);
}

void jackdaw_project_set_snap_enabled(JackDawProject *p, gboolean on)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->snap_enabled = on;
    jackdaw_project_emit_timing_changed(p);
}

void jackdaw_project_set_metronome(JackDawProject *p, gboolean on)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->metronome_enabled = on;
    jackdaw_project_emit_timing_changed(p);
}

void jackdaw_project_set_ruler_mode(JackDawProject *p, JackDawRulerMode m)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->ruler_mode = m;
    jackdaw_project_emit_timing_changed(p);
}

gdouble jackdaw_project_frames_per_beat(JackDawProject *p, guint32 sample_rate)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 0.0);
    if (p->bpm <= 0.0) return 0.0;
    return (gdouble)sample_rate * 60.0 / p->bpm;
}

gdouble jackdaw_project_frames_per_bar(JackDawProject *p, guint32 sample_rate)
{
    return jackdaw_project_frames_per_beat(p, sample_rate) *
           (gdouble)(p->beats_per_bar ? p->beats_per_bar : 1);
}

off_t jackdaw_project_snap_frame(JackDawProject *p, off_t frame,
                                 guint32 sample_rate)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), frame);
    if (!p->snap_enabled) return frame;
    gdouble fpb = jackdaw_project_frames_per_beat(p, sample_rate);
    if (fpb <= 0.0) return frame;
    gdouble n = (gdouble)frame / fpb;
    off_t snapped = (off_t)(floor(n + 0.5) * fpb);
    return snapped < 0 ? 0 : snapped;
}
