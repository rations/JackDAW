#include <config.h>
#include <math.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "project.h"
#include "settings.h"
#include "track.h"
#include "audio_clip.h"
#include "clipregion.h"
#include "midiclip.h"
#include "pluginhost.h"
#include "jackdaw-engine.h"

G_DEFINE_TYPE(JackDawProject, jackdaw_project, G_TYPE_OBJECT)

enum {
    SIGNAL_TRACK_ADDED,
    SIGNAL_TRACK_REMOVED,
    SIGNAL_PORTS_CHANGED,
    SIGNAL_TIMING_CHANGED,
    SIGNAL_SELECTION_CHANGED,
    SIGNAL_TRACKS_REORDERED,
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
    if (p->sel_tracks) {
        g_ptr_array_unref(p->sel_tracks);
        p->sel_tracks = NULL;
    }
    if (p->undo) {
        undo_manager_free(p->undo);
        p->undo = NULL;
    }
    g_free(p->project_file);
    g_clear_object(&p->master_track);

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

    project_signals[SIGNAL_SELECTION_CHANGED] = g_signal_new(
        "selection-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, selection_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 0);

    project_signals[SIGNAL_TRACKS_REORDERED] = g_signal_new(
        "tracks-reordered", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawProjectClass, tracks_reordered),
        NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void jackdaw_project_init(JackDawProject *p)
{
    p->tracks          = g_ptr_array_new_with_free_func(g_object_unref);
    p->sel_tracks      = g_ptr_array_new();   /* borrowed pointers, no free func */
    p->active_track    = NULL;
    p->undo            = undo_manager_new(64);
    p->project_file    = NULL;
    p->master_volume   = 1.0f;
    p->master_rt_chain = NULL;
    /* Master bus as a real track: owns master gain/FX/mute. Kept out of the
     * tracks array so it never shows in the normal track list / save loop. */
    p->master_track    = jackdaw_track_new("Master", NULL);
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
    p->metronome_volume_db = 0.0;  /* unity; loaded per-project from save file */
    p->metronome_gain    = 1.0f;
    p->metronome_route   = METRONOME_ROUTE_MAIN;
    p->countin_before_record = 0;
    p->countin_before_play   = 0;
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
    /* Drop it from the multi-selection first so no dangling pointer remains.
     * active_track is a weak ref — clear it too if it pointed at t. */
    gboolean sel_changed = (p->sel_tracks && g_ptr_array_remove(p->sel_tracks, t));
    if (p->active_track == t) {
        p->active_track = (p->sel_tracks && p->sel_tracks->len > 0)
            ? g_ptr_array_index(p->sel_tracks, p->sel_tracks->len - 1) : NULL;
        sel_changed = TRUE;
    }
    if (sel_changed)
        g_signal_emit(p, project_signals[SIGNAL_SELECTION_CHANGED], 0);
    if (g_ptr_array_remove(p->tracks, t))
        g_signal_emit(p, project_signals[SIGNAL_TRACK_REMOVED], 0, t);
    g_object_unref(t);
}

/* ---- Track multi-selection ---- */

GPtrArray *jackdaw_project_get_selected_tracks(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    return p->sel_tracks;
}

gboolean jackdaw_project_is_selected(JackDawProject *p, JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), FALSE);
    for (guint i = 0; i < p->sel_tracks->len; i++)
        if (g_ptr_array_index(p->sel_tracks, i) == t) return TRUE;
    return FALSE;
}

void jackdaw_project_select_single(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    g_ptr_array_set_size(p->sel_tracks, 0);
    g_ptr_array_add(p->sel_tracks, t);
    p->active_track = t;
    g_signal_emit(p, project_signals[SIGNAL_SELECTION_CHANGED], 0);
}

void jackdaw_project_toggle_selected(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (g_ptr_array_remove(p->sel_tracks, t)) {
        /* Removed t. If it was active, fall back to the last remaining. */
        if (p->active_track == t)
            p->active_track = (p->sel_tracks->len > 0)
                ? g_ptr_array_index(p->sel_tracks, p->sel_tracks->len - 1) : NULL;
    } else {
        g_ptr_array_add(p->sel_tracks, t);
        p->active_track = t;          /* newly added becomes active */
    }
    g_signal_emit(p, project_signals[SIGNAL_SELECTION_CHANGED], 0);
}

void jackdaw_project_clear_selection(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    if (p->sel_tracks->len == 0 && p->active_track == NULL) return;
    g_ptr_array_set_size(p->sel_tracks, 0);
    p->active_track = NULL;
    g_signal_emit(p, project_signals[SIGNAL_SELECTION_CHANGED], 0);
}

JackDawTrack *jackdaw_project_get_active_track(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    return p->active_track;
}

void jackdaw_project_set_active_track(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    if (p->active_track == t) return;
    p->active_track = t;
    /* Invariant: the active track is part of the selection. */
    if (t && !jackdaw_project_is_selected(p, t))
        g_ptr_array_add(p->sel_tracks, t);
    g_signal_emit(p, project_signals[SIGNAL_SELECTION_CHANGED], 0);
}

/* ---- Global undo/redo ---- */

JackDawUndoManager *jackdaw_project_get_undo(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    return p->undo;
}

void jackdaw_project_undo(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    undo_manager_undo(p->undo);
}

void jackdaw_project_redo(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    undo_manager_redo(p->undo);
}

void jackdaw_project_emit_tracks_reordered(JackDawProject *p)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_signal_emit(p, project_signals[SIGNAL_TRACKS_REORDERED], 0);
}

/* ---- Structural (track add/delete/reorder) undo ----
 *
 * Memento = the ordered track list, holding a STRONG ref to each track so a
 * deleted track survives on the undo stack until the action is evicted. Restore
 * diffs the saved list against the live project: removes tracks added since,
 * re-adds tracks deleted since (still fully intact), then reorders to match. */

typedef struct { GPtrArray *tracks; } TrackListState;  /* g_object_ref'd, ordered */

static gpointer tracklist_capture(gpointer ctx)
{
    JackDawProject *p = ctx;
    TrackListState *s = g_new0(TrackListState, 1);
    s->tracks = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < p->tracks->len; i++)
        g_ptr_array_add(s->tracks, g_object_ref(g_ptr_array_index(p->tracks, i)));
    return s;
}

static void tracklist_free(gpointer state)
{
    TrackListState *s = state;
    if (!s) return;
    g_ptr_array_unref(s->tracks);
    g_free(s);
}

static gboolean ptr_array_has(GPtrArray *a, gconstpointer x)
{
    for (guint i = 0; i < a->len; i++)
        if (g_ptr_array_index(a, i) == x) return TRUE;
    return FALSE;
}

static void tracklist_restore(gpointer ctx, gpointer state)
{
    JackDawProject *p = ctx;
    TrackListState *s = state;

    /* Plugin (re)instantiation and engine slot changes are not RT-safe — hold
     * the graph while we reshape it, mirroring jackdaw_project_load. */
    jackdaw_engine_set_suspended(TRUE);

    /* Remove tracks present now but not in the saved list (undo of an add). */
    GPtrArray *cur = g_ptr_array_new();
    for (guint i = 0; i < p->tracks->len; i++)
        g_ptr_array_add(cur, g_ptr_array_index(p->tracks, i));
    for (guint i = 0; i < cur->len; i++) {
        JackDawTrack *t = g_ptr_array_index(cur, i);
        if (!ptr_array_has(s->tracks, t)) {
            jackdaw_engine_remove_track(t);
            jackdaw_project_remove_track(p, t);
        }
    }
    g_ptr_array_free(cur, TRUE);

    /* Re-add tracks in the saved list that are no longer live (undo of a
     * delete). The track object is intact — the memento kept it alive. */
    for (guint i = 0; i < s->tracks->len; i++) {
        JackDawTrack *t = g_ptr_array_index(s->tracks, i);
        if (jackdaw_project_track_index(p, t) < 0) {
            if (!jackdaw_engine_add_track(t))   /* FALSE = success */
                jackdaw_project_add_track(p, t);
        }
    }

    /* Reorder the live list to match the saved order. */
    for (guint i = 0; i < s->tracks->len; i++) {
        JackDawTrack *t = g_ptr_array_index(s->tracks, i);
        gint idx = jackdaw_project_track_index(p, t);
        if (idx >= 0 && (guint)idx != i)
            jackdaw_project_move_track(p, (guint)idx, i);
    }

    jackdaw_engine_set_suspended(FALSE);
    jackdaw_project_emit_tracks_reordered(p);
}

void jackdaw_project_push_structural_undo(JackDawProject *p, const char *desc)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    JackDawUndoAction a = {
        .ctx         = p,
        .saved_state = tracklist_capture(p),
        .capture_fn  = tracklist_capture,
        .restore_fn  = tracklist_restore,
        .free_fn     = tracklist_free,
        .after_fn    = NULL,
        .ctx_free_fn = NULL,            /* ctx is the project; never freed here */
        .desc        = g_strdup(desc ? desc : "Track change"),
    };
    undo_manager_push(p->undo, &a);
}

void jackdaw_project_delete_track(JackDawProject *p, JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    jackdaw_project_push_structural_undo(p, "Delete track");
    jackdaw_engine_remove_track(t);
    jackdaw_project_remove_track(p, t);
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

gint jackdaw_project_track_index(JackDawProject *p, JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), -1);
    for (guint i = 0; i < p->tracks->len; i++)
        if (g_ptr_array_index(p->tracks, i) == t) return (gint)i;
    return -1;
}

void jackdaw_project_move_track(JackDawProject *p, guint from, guint to)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    if (from >= p->tracks->len || to >= p->tracks->len || from == to) return;
    /* remove_index runs the array's free_func (g_object_unref), so take a ref to
     * keep the track alive; the insert hands that ref to the array's slot. */
    gpointer t = g_object_ref(g_ptr_array_index(p->tracks, from));
    g_ptr_array_remove_index(p->tracks, from);
    g_ptr_array_insert(p->tracks, (gint)to, t);
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

JackDawTrack *jackdaw_project_get_master_track(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), NULL);
    return p->master_track;
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

void jackdaw_project_set_metronome_volume(JackDawProject *p, gdouble db)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    if (db < -25.0) db = -25.0;
    if (db >  25.0) db =  25.0;
    p->metronome_volume_db = db;
    p->metronome_gain = (gfloat)pow(10.0, db / 20.0);
    jackdaw_project_emit_timing_changed(p);
}

gdouble jackdaw_project_get_metronome_volume(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 0.0);
    return p->metronome_volume_db;
}

void jackdaw_project_set_metronome_route(JackDawProject *p,
                                         JackDawMetronomeRoute route)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->metronome_route = (gint)route;
    jackdaw_project_emit_timing_changed(p);
}

JackDawMetronomeRoute jackdaw_project_get_metronome_route(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), METRONOME_ROUTE_MAIN);
    return (JackDawMetronomeRoute)p->metronome_route;
}

void jackdaw_project_set_countin_before_record(JackDawProject *p, guint beats)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->countin_before_record = MIN(beats, 32u);
    jackdaw_project_emit_timing_changed(p);
}

guint jackdaw_project_get_countin_before_record(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 0);
    return p->countin_before_record;
}

void jackdaw_project_set_countin_before_play(JackDawProject *p, guint beats)
{
    g_return_if_fail(JACKDAW_IS_PROJECT(p));
    p->countin_before_play = MIN(beats, 32u);
    jackdaw_project_emit_timing_changed(p);
}

guint jackdaw_project_get_countin_before_play(JackDawProject *p)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), 0);
    return p->countin_before_play;
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

/* ============================ Save / Load ===============================
 * One GKeyFile (.jdaw). Boolean convention: TRUE = failure, FALSE = success.
 * All values read back are validated/clamped per CLAUDE.md before use.
 */

/* GKeyFile getters with defaults (g_key_file_get_* errors out on a missing key). */
static gint kf_int(GKeyFile *kf, const char *g, const char *k, gint def)
{ return g_key_file_has_key(kf, g, k, NULL) ? g_key_file_get_integer(kf, g, k, NULL) : def; }
static gint64 kf_i64(GKeyFile *kf, const char *g, const char *k, gint64 def)
{ return g_key_file_has_key(kf, g, k, NULL) ? g_key_file_get_int64(kf, g, k, NULL) : def; }
static gdouble kf_dbl(GKeyFile *kf, const char *g, const char *k, gdouble def)
{ return g_key_file_has_key(kf, g, k, NULL) ? g_key_file_get_double(kf, g, k, NULL) : def; }
static gboolean kf_bool(GKeyFile *kf, const char *g, const char *k, gboolean def)
{ return g_key_file_has_key(kf, g, k, NULL) ? g_key_file_get_boolean(kf, g, k, NULL) : def; }

static gboolean cat_is_instrument(const char *c)
{ return c && (strstr(c, "Instrument") || strstr(c, "Synth")); }

/* Write a track's FX chain. `grp` is the key group holding "fx_count"; each
 * plugin goes into "<grp>.fx<i>". Shared by normal tracks and the master. */
static void project_save_fx(GKeyFile *kf, JackDawTrack *t, const char *grp)
{
    guint fc = jackdaw_track_fx_count(t);
    g_key_file_set_integer(kf, grp, "fx_count", (gint)fc);
    for (guint fi = 0; fi < fc; fi++) {
        PluginInstance *inst = jackdaw_track_fx_get(t, fi);
        const char *key = pluginhost_key(inst), *cat = pluginhost_category(inst);
        char fg[64]; g_snprintf(fg, sizeof fg, "%s.fx%u", grp, fi);
        g_key_file_set_integer(kf, fg, "format", (gint)pluginhost_format(inst));
        g_key_file_set_string (kf, fg, "key", key ? key : "");
        g_key_file_set_string (kf, fg, "name", pluginhost_name(inst));
        g_key_file_set_string (kf, fg, "category", cat ? cat : "");
        g_key_file_set_boolean(kf, fg, "active", pluginhost_is_active(inst));
        g_key_file_set_double (kf, fg, "mix", pluginhost_get_mix(inst));
        guint pc = pluginhost_param_count(inst);
        if (pc > 0 && pc < 4096) {
            gdouble *pv = g_new(gdouble, pc);
            for (guint pi = 0; pi < pc; pi++) pv[pi] = pluginhost_param_get(inst, pi);
            g_key_file_set_double_list(kf, fg, "params", pv, pc);
            g_free(pv);
        }
        /* Opaque plug-in state chunk (VST2/VST3/CLAP) — the authoritative full
         * state the param list can't express (loaded IR/NAM/sample paths, modes,
         * editor state). Base64'd into the key file. No-op for LV2/LADSPA. */
        void *blob = NULL; gsize blen = 0;
        if (pluginhost_state_save(inst, &blob, &blen) && blob && blen > 0) {
            gchar *b64 = g_base64_encode((const guchar *)blob, blen);
            g_key_file_set_string(kf, fg, "state", b64);
            g_free(b64);
        }
        g_free(blob);
    }
}

/* Rebuild a track's FX chain from "<grp>.fx<i>" groups. */
static void project_load_fx(GKeyFile *kf, JackDawTrack *t, const char *grp)
{
    gint fc = CLAMP(kf_int(kf, grp, "fx_count", 0), 0, 1024);
    for (gint fi = 0; fi < fc; fi++) {
        char fg[64]; g_snprintf(fg, sizeof fg, "%s.fx%d", grp, fi);
        if (!g_key_file_has_group(kf, fg)) continue;
        gchar *key = g_key_file_get_string(kf, fg, "key", NULL);
        gchar *fnm = g_key_file_get_string(kf, fg, "name", NULL);
        gchar *cat = g_key_file_get_string(kf, fg, "category", NULL);
        gint   fmt = CLAMP(kf_int(kf, fg, "format", 0), 0, PH_NFORMATS - 1);
        if (key && key[0]) {
            PluginInfo info;
            info.format        = (PluginFormat)fmt;
            info.key           = key;
            info.name          = fnm ? fnm : (char *)"fx";
            info.category      = cat ? cat : (char *)"";
            info.is_instrument = cat_is_instrument(cat);
            PluginInstance *inst = pluginhost_instantiate(&info);
            if (inst) {
                pluginhost_set_active(inst, kf_bool(kf, fg, "active", TRUE));
                pluginhost_set_mix(inst, (float)kf_dbl(kf, fg, "mix", 1.0));
                gsize pn = 0;
                gdouble *pv = g_key_file_get_double_list(kf, fg, "params", &pn, NULL);
                guint pc = pluginhost_param_count(inst);
                for (gsize pi = 0; pv && pi < pn && pi < pc; pi++)
                    pluginhost_param_set(inst, (guint)pi, (float)pv[pi]);
                g_free(pv);
                /* Opaque state chunk last: it's authoritative for VST2/VST3/CLAP
                 * and overrides the params above (and syncs the native editor so
                 * a reopened GUI shows the restored settings). Applied before the
                 * instance joins the RT chain, so it's safe off the RT thread. */
                gchar *b64 = g_key_file_get_string(kf, fg, "state", NULL);
                if (b64 && b64[0]) {
                    gsize blen = 0;
                    guchar *blob = g_base64_decode(b64, &blen);
                    if (blob && blen > 0)
                        pluginhost_state_load(inst, blob, blen);
                    g_free(blob);
                }
                g_free(b64);
                jackdaw_track_fx_add(t, inst);
            }
        }
        g_free(key); g_free(fnm); g_free(cat);
    }
}

/* ---------------------- Project bundle directory ----------------------- */

gchar *jackdaw_default_projects_dir(void)
{
    const gchar *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    gchar *dir = (music && *music)
        ? g_build_filename(music, "JackDAW", "Projects", NULL)
        : g_build_filename(g_get_home_dir(), "Music", "JackDAW", "Projects", NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

/* Copy `src` to `dst`, skipping the copy when `dst` already holds a file of the
 * same size (assumed identical — avoids rewriting large WAVs on every save).
 * Boolean convention: FALSE = success, TRUE = failure. */
static gboolean project_copy_asset(const char *src, const char *dst)
{
    GStatBuf ss, ds;
    if (g_stat(src, &ss) == 0 && g_stat(dst, &ds) == 0 && ss.st_size == ds.st_size)
        return FALSE;   /* already present, identical size -> nothing to do */
    GFile *s = g_file_new_for_path(src);
    GFile *d = g_file_new_for_path(dst);
    gboolean ok = g_file_copy(s, d, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
    g_object_unref(s);
    g_object_unref(d);
    return ok ? FALSE : TRUE;
}

/* Move `src` onto `dst`, falling back to copy+unlink when rename() can't (e.g.
 * the two live on different filesystems). Used for scratch recordings, which
 * have a single home and should not be left behind. FALSE = success. */
static gboolean project_move_asset(const char *src, const char *dst)
{
    if (g_rename(src, dst) == 0) return FALSE;
    if (project_copy_asset(src, dst)) return TRUE;   /* copy failed -> give up */
    g_unlink(src);
    return FALSE;
}

/* Bring every referenced audio file into <audio_dir> and build a map from each
 * source absolute path to its bundle-relative path ("audio/<name>"). Files
 * already inside this bundle are referenced in place. Imported files are copied
 * (the user's original is left untouched). Live takes still sitting in the
 * scratch dir (~/.jackdaw/recordings) are *moved* in — they have a single home
 * and should not pile up there — and their in-memory clips are repointed to the
 * new location. Unsaved takes stay in the scratch dir for recovery. Differing
 * sources that share a basename are disambiguated with a numeric prefix. The
 * returned table is owned by the caller. */
static GHashTable *project_collect_assets(JackDawProject *p, const char *audio_dir)
{
    GHashTable *relmap = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    GHashTable *used   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    /* src path -> new absolute path, for clips whose scratch file we moved. */
    GHashTable *moved  = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    gchar *audio_prefix = g_strconcat(audio_dir, G_DIR_SEPARATOR_S, NULL);
    gchar *rec_dir      = g_build_filename(g_get_home_dir(), ".jackdaw",
                                           "recordings", NULL);
    gchar *rec_prefix   = g_strconcat(rec_dir, G_DIR_SEPARATOR_S, NULL);
    g_free(rec_dir);
    /* Never move a file out from under the feeder thread, which reads paths only
     * while the transport is playing; copy in that case and move on a later save. */
    gboolean may_move = !jackdaw_engine_is_playing();

    for (guint ti = 0; ti < p->tracks->len; ti++) {
        JackDawTrack *t = JACKDAW_TRACK(g_ptr_array_index(p->tracks, ti));
        GPtrArray *regs = jackdaw_track_get_regions(t);
        for (guint ri = 0; regs && ri < regs->len; ri++) {
            ClipRegion *r = g_ptr_array_index(regs, ri);
            const char *src = (r->clip && r->clip->path) ? r->clip->path : NULL;
            if (!src || !*src || g_hash_table_contains(relmap, src)) continue;

            /* Already inside this bundle -> keep its existing relative path. */
            if (g_str_has_prefix(src, audio_prefix)) {
                gchar *rel = g_build_filename("audio",
                                              src + strlen(audio_prefix), NULL);
                g_hash_table_insert(relmap, g_strdup(src), rel);
                continue;
            }

            gchar *bn = g_path_get_basename(src);
            gchar *cand = g_strdup(bn);
            for (guint n = 1; g_hash_table_contains(used, cand); n++) {
                g_free(cand);
                cand = g_strdup_printf("%u_%s", n, bn);
            }
            g_free(bn);

            gchar *dst = g_build_filename(audio_dir, cand, NULL);
            gboolean placed = FALSE, was_moved = FALSE;
            if (may_move && g_str_has_prefix(src, rec_prefix)) {
                if (!project_move_asset(src, dst)) {
                    g_hash_table_insert(moved, g_strdup(src), g_strdup(dst));
                    placed = was_moved = TRUE;
                }
            }
            if (!placed && !project_copy_asset(src, dst))
                placed = TRUE;

            gchar *rel;
            if (placed) {
                rel = g_build_filename("audio", cand, NULL);
                g_hash_table_add(used, g_strdup(cand));
            } else {
                rel = g_strdup(src);   /* move+copy failed -> reference original */
            }
            g_hash_table_insert(relmap, g_strdup(src), rel);
            /* A moved clip is repointed to `dst` below, so the save loop will look
             * it up by the new path — map that to the same relative path too. */
            if (was_moved)
                g_hash_table_insert(relmap, g_strdup(dst), g_strdup(rel));
            g_free(cand);
            g_free(dst);
        }
    }

    /* Repoint clips whose scratch file we just moved into the bundle. Safe: we
     * only move while stopped, so the feeder thread isn't reading these paths.
     * Shared clips update once (the second region already sees the new path). */
    if (g_hash_table_size(moved) > 0) {
        for (guint ti = 0; ti < p->tracks->len; ti++) {
            JackDawTrack *t = JACKDAW_TRACK(g_ptr_array_index(p->tracks, ti));
            GPtrArray *regs = jackdaw_track_get_regions(t);
            for (guint ri = 0; regs && ri < regs->len; ri++) {
                ClipRegion *r = g_ptr_array_index(regs, ri);
                if (!r->clip || !r->clip->path) continue;
                const char *np = g_hash_table_lookup(moved, r->clip->path);
                if (np) {
                    gchar *old = r->clip->path;
                    g_atomic_pointer_set(&r->clip->path, g_strdup(np));
                    g_free(old);
                }
            }
        }
    }

    g_free(audio_prefix);
    g_free(rec_prefix);
    g_hash_table_destroy(used);
    g_hash_table_destroy(moved);
    return relmap;
}

gboolean jackdaw_project_save(JackDawProject *p, const gchar *path)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), TRUE);
    if (!path || !*path) return TRUE;

    /* ---- Resolve the project bundle layout from `path`. ----
     * A project is a self-contained directory <bundle>/ holding <name>.jdaw plus
     * an audio/ subfolder with copies of every referenced sound file. If `path`
     * already sits inside a directory named after the project (a re-save), reuse
     * that directory rather than nesting a second level. */
    gchar *raw = g_path_get_basename(path);
    gchar *name = g_str_has_suffix(raw, ".jdaw")
        ? g_strndup(raw, strlen(raw) - 5) : g_strdup(raw);
    g_free(raw);
    if (!*name) { g_free(name); return TRUE; }

    gchar *parent = g_path_get_dirname(path);
    gchar *pbase  = g_path_get_basename(parent);
    gchar *bundle = (g_strcmp0(pbase, name) == 0)
        ? g_strdup(parent) : g_build_filename(parent, name, NULL);
    g_free(pbase);
    g_free(parent);

    gchar *audio_dir = g_build_filename(bundle, "audio", NULL);
    if (g_mkdir_with_parents(audio_dir, 0755) != 0) {
        g_free(name); g_free(bundle); g_free(audio_dir);
        return TRUE;
    }
    gchar *jdaw_name = g_strconcat(name, ".jdaw", NULL);
    gchar *jdaw_path = g_build_filename(bundle, jdaw_name, NULL);
    g_free(jdaw_name);

    /* Copy referenced audio into the bundle and map sources -> relative paths. */
    GHashTable *relmap = project_collect_assets(p, audio_dir);
    g_free(audio_dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_double (kf, "project", "bpm", p->bpm);
    g_key_file_set_integer(kf, "project", "beats_per_bar", (gint)p->beats_per_bar);
    g_key_file_set_integer(kf, "project", "beat_unit", (gint)p->beat_unit);
    /* Legacy field: effective master gain (so old readers still get a level). */
    g_key_file_set_double (kf, "project", "master_volume",
                           jackdaw_track_get_volume(p->master_track));
    g_key_file_set_boolean(kf, "project", "grid", p->grid_enabled);
    g_key_file_set_boolean(kf, "project", "snap", p->snap_enabled);
    g_key_file_set_boolean(kf, "project", "metronome", p->metronome_enabled);
    g_key_file_set_double (kf, "project", "metronome_volume", p->metronome_volume_db);
    g_key_file_set_integer(kf, "project", "metronome_route", p->metronome_route);
    g_key_file_set_integer(kf, "project", "countin_record", (gint)p->countin_before_record);
    g_key_file_set_integer(kf, "project", "countin_play",   (gint)p->countin_before_play);
    g_key_file_set_integer(kf, "project", "ruler", (gint)p->ruler_mode);
    g_key_file_set_integer(kf, "project", "track_count", (gint)p->tracks->len);

    for (guint ti = 0; ti < p->tracks->len; ti++) {
        JackDawTrack *t = JACKDAW_TRACK(g_ptr_array_index(p->tracks, ti));
        char grp[32]; g_snprintf(grp, sizeof grp, "track%u", ti);

        g_key_file_set_string (kf, grp, "name", jackdaw_track_get_name(t));
        g_key_file_set_integer(kf, grp, "kind", (gint)jackdaw_track_get_kind(t));
        g_key_file_set_double (kf, grp, "volume", jackdaw_track_get_volume(t));
        g_key_file_set_double (kf, grp, "trim",  jackdaw_track_get_trim(t));
        g_key_file_set_double (kf, grp, "fader", jackdaw_track_get_fader(t));
        g_key_file_set_double (kf, grp, "pan", jackdaw_track_get_pan(t));
        gint flags = (jackdaw_track_is_armed(t)  ? 1 : 0) |
                     (jackdaw_track_is_muted(t)  ? 2 : 0) |
                     (jackdaw_track_is_soloed(t) ? 4 : 0);
        g_key_file_set_integer(kf, grp, "flags", flags);
        g_key_file_set_integer(kf, grp, "audio_in", t->audio_in_idx);
        g_key_file_set_integer(kf, grp, "midi_in",  t->midi_in_idx);

        GPtrArray *regs = jackdaw_track_get_regions(t);
        g_key_file_set_integer(kf, grp, "region_count", regs ? (gint)regs->len : 0);
        for (guint ri = 0; regs && ri < regs->len; ri++) {
            ClipRegion *r = g_ptr_array_index(regs, ri);
            char rg[48]; g_snprintf(rg, sizeof rg, "track%u.region%u", ti, ri);
            const char *src = (r->clip && r->clip->path) ? r->clip->path : NULL;
            const char *relp = src ? g_hash_table_lookup(relmap, src) : NULL;
            g_key_file_set_string(kf, rg, "path",
                relp ? relp : (src ? src : ""));
            g_key_file_set_int64 (kf, rg, "file_in", r->file_in);
            g_key_file_set_int64 (kf, rg, "length",  r->length);
            g_key_file_set_int64 (kf, rg, "tl_pos",  r->tl_pos);
            g_key_file_set_double(kf, rg, "gain",    r->gain);
        }

        MidiClip *mc = jackdaw_track_get_midi_clip(t);
        guint nc = mc ? midi_clip_note_count(mc) : 0;
        g_key_file_set_integer(kf, grp, "midi_note_count", (gint)nc);
        if (nc > 0) {
            GArray *vals = g_array_new(FALSE, FALSE, sizeof(gint));
            for (guint ni = 0; ni < nc; ni++) {
                MidiNote *n = midi_clip_note(mc, ni);
                gint v;
                v = (gint)n->start;    g_array_append_val(vals, v);
                v = (gint)n->length;   g_array_append_val(vals, v);
                v = (gint)n->pitch;    g_array_append_val(vals, v);
                v = (gint)n->velocity; g_array_append_val(vals, v);
                v = (gint)n->channel;  g_array_append_val(vals, v);
            }
            g_key_file_set_integer_list(kf, grp, "midi_notes",
                                        (gint *)vals->data, vals->len);
            g_array_free(vals, TRUE);
        }

        /* MIDI regions (instrument tracks): timeline windows into the clip that
         * survive split/move. All regions share the single midi_clip, so only
         * the window geometry is stored. */
        GPtrArray *mregs = jackdaw_track_get_midi_regions(t);
        guint mrc = (jackdaw_track_get_kind(t) == JACKDAW_TRACK_INSTRUMENT &&
                     mregs) ? mregs->len : 0;
        g_key_file_set_integer(kf, grp, "midi_region_count", (gint)mrc);
        MidiClip *track_mc = jackdaw_track_get_midi_clip(t);
        for (guint mi = 0; mi < mrc; mi++) {
            MidiRegion *r = g_ptr_array_index(mregs, mi);
            char rg[56]; g_snprintf(rg, sizeof rg, "track%u.midiregion%u", ti, mi);
            g_key_file_set_integer(kf, rg, "clip_in", (gint)r->clip_in);
            g_key_file_set_integer(kf, rg, "length",  (gint)r->length);
            g_key_file_set_int64  (kf, rg, "tl_pos",  r->tl_pos);
            g_key_file_set_integer(kf, rg, "auto_grow", r->auto_grow ? 1 : 0);
            /* A region dragged in from another track references that track's clip
             * rather than this track's — persist its notes inline so the move
             * survives save/reload (the in-point ticks index into these notes). */
            if (r->clip && r->clip != track_mc) {
                guint rn = midi_clip_note_count(r->clip);
                g_key_file_set_integer(kf, rg, "own_clip_notes", (gint)rn);
                if (rn > 0) {
                    GArray *vals = g_array_new(FALSE, FALSE, sizeof(gint));
                    for (guint ni = 0; ni < rn; ni++) {
                        MidiNote *n = midi_clip_note(r->clip, ni);
                        gint v;
                        v = (gint)n->start;    g_array_append_val(vals, v);
                        v = (gint)n->length;   g_array_append_val(vals, v);
                        v = (gint)n->pitch;    g_array_append_val(vals, v);
                        v = (gint)n->velocity; g_array_append_val(vals, v);
                        v = (gint)n->channel;  g_array_append_val(vals, v);
                    }
                    g_key_file_set_integer_list(kf, rg, "own_clip",
                                                (gint *)vals->data, vals->len);
                    g_array_free(vals, TRUE);
                }
            }
        }

        project_save_fx(kf, t, grp);
    }

    /* Master bus track: gain stages + FX chain under the "master" group. */
    g_key_file_set_double (kf, "master", "trim",  jackdaw_track_get_trim(p->master_track));
    g_key_file_set_double (kf, "master", "fader", jackdaw_track_get_fader(p->master_track));
    g_key_file_set_integer(kf, "master", "muted",
                           jackdaw_track_is_muted(p->master_track) ? 1 : 0);
    project_save_fx(kf, p->master_track, "master");

    /* Roll the previous save into a single backup, overwritten on each save so
     * the project directory keeps exactly one <name>.jdaw.bak rather than a
     * growing pile. Copy (not rename) so the live .jdaw survives if the new
     * write below fails. */
    if (g_file_test(jdaw_path, G_FILE_TEST_EXISTS)) {
        gchar *bak = g_strconcat(jdaw_path, ".bak", NULL);
        GFile *s = g_file_new_for_path(jdaw_path);
        GFile *d = g_file_new_for_path(bak);
        g_file_copy(s, d, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
        g_object_unref(s);
        g_object_unref(d);
        g_free(bak);
    }

    gboolean ok = g_key_file_save_to_file(kf, jdaw_path, NULL);
    g_key_file_free(kf);
    g_hash_table_destroy(relmap);
    if (ok) jackdaw_project_set_file(p, jdaw_path);
    g_free(name);
    g_free(bundle);
    g_free(jdaw_path);
    return ok ? FALSE : TRUE;
}

gboolean jackdaw_project_load(JackDawProject *p, const gchar *path)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), TRUE);
    if (!path) return TRUE;

    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        return TRUE;
    }

    /* Directory holding the .jdaw, used to resolve bundle-relative audio paths. */
    gchar *projdir = g_path_get_dirname(path);

    /* Hold the RT graph off the plugins while we tear down the old chain and
     * instantiate the new one (dlopen / setupProcessing / buffer allocation are
     * not RT-safe and would otherwise xrun the live audio thread). Resumed at the
     * single success exit below. */
    jackdaw_engine_set_suspended(TRUE);

    /* Drop all undo history: mementos reference the outgoing session's tracks
     * and must not resurrect them into the loaded one. */
    undo_manager_clear(p->undo);

    /* Clear the current session (engine slots + project tracks). */
    guint cur = p->tracks->len;
    while (cur-- > 0) {
        JackDawTrack *t = jackdaw_project_get_track(p, 0);
        jackdaw_engine_remove_track(t);
        jackdaw_project_remove_track(p, t);
    }

    p->bpm           = CLAMP(kf_dbl(kf, "project", "bpm", 120.0), 20.0, 999.0);
    p->beats_per_bar = CLAMP(kf_int(kf, "project", "beats_per_bar", 4), 1, 32);
    p->beat_unit     = CLAMP(kf_int(kf, "project", "beat_unit", 4), 1, 32);
    p->master_volume = CLAMP(kf_dbl(kf, "project", "master_volume", 1.0), 0.0, 2.0);
    p->grid_enabled      = kf_bool(kf, "project", "grid", FALSE);
    p->snap_enabled      = kf_bool(kf, "project", "snap", FALSE);
    p->metronome_enabled = kf_bool(kf, "project", "metronome", FALSE);
    p->metronome_volume_db =
        CLAMP(kf_dbl(kf, "project", "metronome_volume", 0.0), -25.0, 25.0);
    p->metronome_gain = (gfloat)pow(10.0, p->metronome_volume_db / 20.0);
    p->metronome_route = (kf_int(kf, "project", "metronome_route",
                                 METRONOME_ROUTE_MAIN) == METRONOME_ROUTE_CLICK_PORT)
                         ? METRONOME_ROUTE_CLICK_PORT : METRONOME_ROUTE_MAIN;
    p->countin_before_record =
        (guint)CLAMP(kf_int(kf, "project", "countin_record", 0), 0, 32);
    p->countin_before_play =
        (guint)CLAMP(kf_int(kf, "project", "countin_play", 0), 0, 32);
    p->ruler_mode    = kf_int(kf, "project", "ruler", JACKDAW_RULER_TIME) ?
                       JACKDAW_RULER_BARS : JACKDAW_RULER_TIME;

    double fpb = jackdaw_project_frames_per_beat(p, jackdaw_engine_get_sample_rate());

    gint tc = CLAMP(kf_int(kf, "project", "track_count", 0), 0, JACKDAW_MAX_TRACKS);
    for (gint ti = 0; ti < tc; ti++) {
        char grp[32]; g_snprintf(grp, sizeof grp, "track%d", ti);
        if (!g_key_file_has_group(kf, grp)) continue;

        gchar *nm = g_key_file_has_key(kf, grp, "name", NULL)
                        ? g_key_file_get_string(kf, grp, "name", NULL) : g_strdup("Track");
        JackDawTrack *t = jackdaw_track_new(nm, NULL);
        g_free(nm);

        jackdaw_track_set_kind(t, kf_int(kf, grp, "kind", 0) ?
                               JACKDAW_TRACK_INSTRUMENT : JACKDAW_TRACK_AUDIO);
        /* Restore both gain stages. Legacy sessions had only "volume" — fall
         * back to placing it on the fader with trim at unity. */
        double legacy_vol = kf_dbl(kf, grp, "volume", 1.0);
        jackdaw_track_set_trim (t, (gfloat)kf_dbl(kf, grp, "trim", 1.0));
        jackdaw_track_set_fader(t, (gfloat)kf_dbl(kf, grp, "fader", legacy_vol));
        jackdaw_track_set_pan   (t, (gfloat)kf_dbl(kf, grp, "pan", 0.0));
        gint flags = kf_int(kf, grp, "flags", 0);
        jackdaw_track_set_armed (t, (flags & 1) != 0);
        jackdaw_track_set_muted (t, (flags & 2) != 0);
        jackdaw_track_set_soloed(t, (flags & 4) != 0);
        t->audio_in_idx = MAX(kf_int(kf, grp, "audio_in", -1), -1);
        t->midi_in_idx  = MAX(kf_int(kf, grp, "midi_in",  -1), -1);

        if (jackdaw_engine_add_track(t)) { g_object_unref(t); continue; }
        jackdaw_project_add_track(p, t);   /* project takes its own ref */
        g_object_unref(t);                 /* drop our creation ref; project owns it */

        /* audio regions */
        gint rc = CLAMP(kf_int(kf, grp, "region_count", 0), 0, 100000);
        GPtrArray *regs = jackdaw_track_get_regions(t);
        for (gint ri = 0; ri < rc; ri++) {
            char rg[48]; g_snprintf(rg, sizeof rg, "track%d.region%d", ti, ri);
            if (!g_key_file_has_group(kf, rg)) continue;
            gchar *rp = g_key_file_get_string(kf, rg, "path", NULL);
            /* Bundle-relative paths (e.g. "audio/kick.wav") resolve against the
             * .jdaw's directory; legacy absolute paths are used as-is. */
            gchar *abs = NULL;
            if (rp && rp[0] == '/')      abs = g_strdup(rp);
            else if (rp && rp[0])        abs = g_build_filename(projdir, rp, NULL);
            if (abs && abs[0] == '/' && !strstr(abs, "..") && strlen(abs) < 4096) {
                AudioClip *clip = audio_clip_new(abs, NULL);
                if (clip) {
                    ClipRegion *r = clip_region_new(clip,
                        kf_i64(kf, rg, "file_in", 0),
                        kf_i64(kf, rg, "length", clip->info.frames),
                        kf_i64(kf, rg, "tl_pos", 0));
                    r->gain = (gfloat)kf_dbl(kf, rg, "gain", 1.0);
                    g_ptr_array_add(regs, r);
                    audio_clip_free(clip);   /* region holds its own ref */
                }
            }
            g_free(abs);
            g_free(rp);
        }
        jackdaw_track_commit_regions(t);

        /* midi notes (flat list on track group) */
        {
            MidiClip *mc = jackdaw_track_get_midi_clip(t);
            gsize nn = 0;
            gint *notes = g_key_file_get_integer_list(kf, grp, "midi_notes", &nn, NULL);
            for (gsize k = 0; notes && k + 5 <= nn; k += 5) {
                MidiNote n;
                n.start    = (guint32)MAX(notes[k], 0);
                n.length   = (guint32)MAX(notes[k + 1], 1);
                n.pitch    = (guint8)CLAMP(notes[k + 2], 0, 127);
                n.velocity = (guint8)CLAMP(notes[k + 3], 0, 127);
                n.channel  = (guint8)CLAMP(notes[k + 4], 0, 15);
                midi_clip_add_note(mc, n);
            }
            g_free(notes);
        }

        /* MIDI regions (timeline windows). Absent in legacy sessions — then the
         * commit below auto-creates one full-clip region covering everything. */
        {
            MidiClip  *mc    = jackdaw_track_get_midi_clip(t);
            GPtrArray *mregs = jackdaw_track_get_midi_regions(t);
            gint mrc = CLAMP(kf_int(kf, grp, "midi_region_count", 0), 0, 100000);
            /* Replace the default region seeded by set_kind with the saved
             * arrangement.  No saved regions (legacy) → keep the default. */
            if (mrc > 0 && mregs->len > 0)
                g_ptr_array_remove_range(mregs, 0, mregs->len);
            for (gint mi = 0; mi < mrc; mi++) {
                char rg[56]; g_snprintf(rg, sizeof rg, "track%d.midiregion%d", ti, mi);
                if (!g_key_file_has_group(kf, rg)) continue;
                guint32 clip_in = (guint32)MAX(kf_int(kf, rg, "clip_in", 0), 0);
                guint32 length  = (guint32)MAX(kf_int(kf, rg, "length",  0), 0);
                off_t   tl_pos  = MAX(kf_i64(kf, rg, "tl_pos", 0), (gint64)0);
                if (length == 0) continue;

                /* Region dragged in from another track: rebuild its independent
                 * clip from the inline notes; otherwise window this track's clip. */
                MidiClip *rc = mc;
                gboolean  own = FALSE;
                if (kf_int(kf, rg, "own_clip_notes", 0) > 0) {
                    rc = midi_clip_new(clip_in + length);
                    own = TRUE;
                    gsize nn = 0;
                    gint *on = g_key_file_get_integer_list(kf, rg, "own_clip",
                                                           &nn, NULL);
                    for (gsize k = 0; on && k + 5 <= nn; k += 5) {
                        MidiNote n;
                        n.start    = (guint32)MAX(on[k], 0);
                        n.length   = (guint32)MAX(on[k + 1], 1);
                        n.pitch    = (guint8)CLAMP(on[k + 2], 0, 127);
                        n.velocity = (guint8)CLAMP(on[k + 3], 0, 127);
                        n.channel  = (guint8)CLAMP(on[k + 4], 0, 15);
                        midi_clip_add_note(rc, n);
                    }
                    g_free(on);
                }
                MidiRegion *reg = midi_region_new(rc, clip_in, length, tl_pos);
                reg->auto_grow = kf_int(kf, rg, "auto_grow", 0) ? TRUE : FALSE;
                g_ptr_array_add(mregs, reg);
                if (own) midi_clip_free(rc);   /* region holds its own ref */
            }
        }
        jackdaw_track_commit_midi(t, fpb);

        /* fx chain (incl. the instrument at index 0) */
        project_load_fx(kf, t, grp);
    }

    /* Master bus track: gain stages + FX. Legacy sessions have no "master"
     * group — fall back to master_volume on the fader, trim/FX at unity/none. */
    {
        double legacy_master = kf_dbl(kf, "project", "master_volume", 1.0);
        jackdaw_track_set_trim (p->master_track,
                                (gfloat)kf_dbl(kf, "master", "trim", 1.0));
        jackdaw_track_set_fader(p->master_track,
                                (gfloat)kf_dbl(kf, "master", "fader", legacy_master));
        jackdaw_track_set_muted(p->master_track,
                                kf_int(kf, "master", "muted", 0) != 0);
        while (jackdaw_track_fx_count(p->master_track) > 0)   /* drop old chain */
            jackdaw_track_fx_remove(p->master_track, 0);
        project_load_fx(kf, p->master_track, "master");
    }

    g_key_file_free(kf);
    g_free(projdir);
    jackdaw_project_set_file(p, path);
    jackdaw_project_emit_timing_changed(p);
    jackdaw_engine_set_suspended(FALSE);   /* graph rebuilt — resume the RT path */
    return FALSE;
}
