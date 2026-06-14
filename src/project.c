#include <config.h>
#include <math.h>
#include <string.h>

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
}

static void jackdaw_project_init(JackDawProject *p)
{
    p->tracks          = g_ptr_array_new_with_free_func(g_object_unref);
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
                jackdaw_track_fx_add(t, inst);
            }
        }
        g_free(key); g_free(fnm); g_free(cat);
    }
}

gboolean jackdaw_project_save(JackDawProject *p, const gchar *path)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(p), TRUE);
    if (!path) return TRUE;

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
            g_key_file_set_string(kf, rg, "path",
                (r->clip && r->clip->path) ? r->clip->path : "");
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

        project_save_fx(kf, t, grp);
    }

    /* Master bus track: gain stages + FX chain under the "master" group. */
    g_key_file_set_double (kf, "master", "trim",  jackdaw_track_get_trim(p->master_track));
    g_key_file_set_double (kf, "master", "fader", jackdaw_track_get_fader(p->master_track));
    g_key_file_set_integer(kf, "master", "muted",
                           jackdaw_track_is_muted(p->master_track) ? 1 : 0);
    project_save_fx(kf, p->master_track, "master");

    gboolean ok = g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    if (ok) jackdaw_project_set_file(p, path);
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
            if (rp && rp[0] == '/' && !strstr(rp, "..") && strlen(rp) < 4096) {
                AudioClip *clip = audio_clip_new(rp, NULL);
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
    jackdaw_project_set_file(p, path);
    jackdaw_project_emit_timing_changed(p);
    return FALSE;
}
