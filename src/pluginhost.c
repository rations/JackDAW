#define _GNU_SOURCE
#include <config.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pluginhost.h"
#include "pluginhost_internal.h"
#include "lv2ui_bridge.h"
#include "settings.h"

#ifdef GDK_WINDOWING_X11
#endif

/* ---- Global host state ---- */

static double    ph_sr        = 48000.0;
static int       ph_maxblock  = 1024;
static gboolean  ph_inited    = FALSE;
static GList    *ph_cat       = NULL;   /* PluginInfo* */
static gboolean  ph_scanned   = FALSE;
static GList    *ph_paths[PH_NFORMATS]; /* char* extra dirs per format */

static const char *ph_fmt_names[PH_NFORMATS] = {
    "LV2", "VST2", "VST3", "CLAP", "LADSPA"
};

const char *pluginhost_format_name(PluginFormat fmt)
{
    return (fmt >= 0 && fmt < PH_NFORMATS) ? ph_fmt_names[fmt] : "?";
}

void pluginhost_init(double sample_rate, int max_block)
{
    ph_sr       = sample_rate > 0 ? sample_rate : 48000.0;
    ph_maxblock = max_block   > 0 ? max_block   : 1024;
    ph_inited   = TRUE;
}

void pluginhost_ui_init(int *argc, char ***argv)
{
#ifdef HAVE_LV2
    ph_lv2_ui_init(argc, argv);
#else
    (void)argc; (void)argv;
#endif
}

/* ---- Helpers ---- */

PluginInfo *ph_info_new(PluginFormat fmt, const char *key,
                        const char *name, const char *category)
{
    PluginInfo *pi = g_new0(PluginInfo, 1);
    pi->format   = fmt;
    pi->key      = g_strdup(key ? key : "");
    pi->name     = g_strdup(name ? name : "(unnamed)");
    pi->category = g_strdup((category && *category) ? category
                                                    : ph_fmt_names[fmt]);
    pi->is_instrument = ph_category_is_instrument(pi->category);
    return pi;
}

/* The scan backends encode "instrument-ness" in the category string: LV2 sets
 * the lilv class label ("Instrument"), VST3 the subcategory ("Instrument|Synth"),
 * VST2 sets "Instrument" when effGetPlugCategory==kPlugCategSynth. */
gboolean ph_category_is_instrument(const char *category)
{
    if (!category) return FALSE;
    return strstr(category, "Instrument") != NULL ||
           strstr(category, "Synth")      != NULL;
}

/* ---- Transport (published by the engine each RT block; read by backends) ---- */
static double  ph_xport_bpm     = 120.0;
static double  ph_xport_sr      = 48000.0;
static gint64  ph_xport_frame   = 0;
static gboolean ph_xport_playing = FALSE;

void pluginhost_set_transport(double bpm, double sr, gint64 frame, gboolean playing)
{
    ph_xport_bpm     = bpm > 0.0 ? bpm : 120.0;
    ph_xport_sr      = sr  > 0.0 ? sr  : ph_sr;
    ph_xport_frame   = frame;
    ph_xport_playing = playing;
}

void ph_get_transport(double *bpm, double *sr, gint64 *frame, gboolean *playing)
{
    if (bpm)     *bpm     = ph_xport_bpm;
    if (sr)      *sr      = ph_xport_sr;
    if (frame)   *frame   = ph_xport_frame;
    if (playing) *playing = ph_xport_playing;
}

static void ph_info_free(gpointer p)
{
    PluginInfo *pi = p;
    if (!pi) return;
    g_free(pi->key); g_free(pi->name); g_free(pi->category);
    g_free(pi);
}

gboolean ph_path_is_safe(const char *path)
{
    if (!path || path[0] != '/') return FALSE;       /* must be absolute */
    if (strstr(path, "..")) return FALSE;            /* no parent traversal */
    if (strlen(path) >= 4096) return FALSE;
    /* g_strdup'd C strings can't carry NUL mid-string, but be explicit. */
    return TRUE;
}

PluginInstance *ph_instance_alloc(PluginFormat fmt, const char *name,
                                  double sr, int max_block)
{
    PluginInstance *inst = g_new0(PluginInstance, 1);
    inst->format      = fmt;
    inst->name        = g_strdup(name ? name : "fx");
    inst->active      = 1;
    inst->mix_q15     = 32768;          /* fully wet by default */
    inst->sample_rate = sr;
    inst->max_block   = max_block;
    inst->dry_L       = g_new0(float, max_block > 0 ? max_block : 1);
    inst->dry_R       = g_new0(float, max_block > 0 ? max_block : 1);
    return inst;
}

/* ---- Catalog ---- */

/* ============================================================================
 * Out-of-process scanning + on-disk cache (Reaper-style).
 *
 * dlopen-format backends never load plugin code to list it; they route each
 * file through ph_scan_cached(), which on a miss spawns THIS binary as
 * `jackdaw --scan-plugin <FMT> <path>` to load+describe it in a throwaway
 * process — so Wine/yabridge (and its fontconfig) never enters the main process.
 * Results persist in ~/.jackdaw/plugincache keyed by path+mtime; only new or
 * changed plugins are re-scanned. LV2 is .ttl-only (lilv) so it scans in-process.
 * ==========================================================================*/

static void ph_esc(GString *s, const char *v)
{
    for (; v && *v; v++) {
        if      (*v == '\\') g_string_append(s, "\\\\");
        else if (*v == '\n') g_string_append(s, "\\n");
        else if (*v == '\t') g_string_append(s, "\\t");
        else                 g_string_append_c(s, *v);
    }
}
static char *ph_unesc(const char *v)
{
    GString *s = g_string_new("");
    for (; v && *v; v++) {
        if (*v == '\\' && v[1]) { v++;
            g_string_append_c(s, *v == 'n' ? '\n' : *v == 't' ? '\t' : *v);
        } else g_string_append_c(s, *v);
    }
    return g_string_free(s, FALSE);
}

typedef struct { gint64 mtime; GPtrArray *classes; gboolean seen; } PhCacheEnt;
static GHashTable *ph_cache;          /* path -> PhCacheEnt, valid only mid-scan */
static gboolean    ph_cache_dirty;
static void (*ph_progress_cb)(const char *, void *);
static void  *ph_progress_u;

void pluginhost_set_scan_progress(void (*cb)(const char *, void *), void *u)
{ ph_progress_cb = cb; ph_progress_u = u; }

static void ph_cache_ent_free(gpointer p)
{ PhCacheEnt *e = p; if (e->classes) g_ptr_array_free(e->classes, TRUE); g_free(e); }

static char *ph_cache_path(void)
{ return g_build_filename(g_get_home_dir(), ".jackdaw", "plugincache", NULL); }

static void ph_cache_load(void)
{
    ph_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ph_cache_ent_free);
    ph_cache_dirty = FALSE;
    char *file = ph_cache_path(), *data = NULL; gsize len = 0;
    if (g_file_get_contents(file, &data, &len, NULL)) {
        gchar **rows = g_strsplit(data, "\n", -1);
        for (int i = 0; rows[i]; i++) {
            if (!rows[i][0]) continue;
            gchar **f = g_strsplit(rows[i], "\t", 5);   /* epath mtime ekey ename ecat */
            if (f[0] && f[1] && f[2] && f[3] && f[4]) {
                char *path = ph_unesc(f[0]);
                PhCacheEnt *e = g_hash_table_lookup(ph_cache, path);
                if (!e) { e = g_new0(PhCacheEnt, 1);
                          e->mtime = g_ascii_strtoll(f[1], NULL, 10);
                          e->classes = g_ptr_array_new_with_free_func(g_free);
                          g_hash_table_insert(ph_cache, path, e); }
                else g_free(path);
                if (f[2][0])   /* empty ekey marks "scanned, no classes" */
                    g_ptr_array_add(e->classes,
                                    g_strdup_printf("%s\t%s\t%s", f[2], f[3], f[4]));
            }
            g_strfreev(f);
        }
        g_strfreev(rows); g_free(data);
    }
    g_free(file);
}

static void ph_cache_save(void)
{
    GString *s = g_string_new("");
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, ph_cache);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        PhCacheEnt *e = v; if (!e->seen) continue;   /* drop deleted plugins */
        GString *ep = g_string_new(""); ph_esc(ep, (const char *)k);
        if (e->classes->len == 0)
            g_string_append_printf(s, "%s\t%lld\t\t\t\n", ep->str, (long long)e->mtime);
        for (guint i = 0; i < e->classes->len; i++)
            g_string_append_printf(s, "%s\t%lld\t%s\n", ep->str, (long long)e->mtime,
                                   (const char *)g_ptr_array_index(e->classes, i));
        g_string_free(ep, TRUE);
    }
    char *file = ph_cache_path();
    g_file_set_contents(file, s->str, s->len, NULL);
    g_free(file); g_string_free(s, TRUE);
}

static void ph_emit_class(PluginFormat fmt, const char *line, GList **catalog)
{
    gchar **f = g_strsplit(line, "\t", 3);          /* ekey ename ecat */
    if (f[0] && f[0][0] && f[1] && f[2]) {
        char *key = ph_unesc(f[0]), *name = ph_unesc(f[1]), *cat = ph_unesc(f[2]);
        *catalog = g_list_prepend(*catalog, ph_info_new(fmt, key, name, cat));
        g_free(key); g_free(name); g_free(cat);
    }
    g_strfreev(f);
}

void ph_scan_cached(PluginFormat fmt, const char *path, GList **catalog)
{
    if (!ph_cache) return;
    struct stat st;
    gint64 mtime = (stat(path, &st) == 0) ? (gint64)st.st_mtime : 0;
    PhCacheEnt *e = g_hash_table_lookup(ph_cache, path);

    if (!e || e->mtime != mtime) {                  /* miss -> scan out of process */
        if (ph_progress_cb) ph_progress_cb(path, ph_progress_u);
        char *self = NULL;
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { exe[n] = 0; self = exe; }
        if (!self) return;
        char *argv[] = { self, (char *)"--scan-plugin",
                         (char *)pluginhost_format_name(fmt), (char *)path, NULL };
        char *sout = NULL; gint status = 0;
        gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL, &sout, NULL, &status, NULL);
        if (!ok) { g_free(sout); return; }

        if (!e) { e = g_new0(PhCacheEnt, 1);
                  e->classes = g_ptr_array_new_with_free_func(g_free);
                  g_hash_table_insert(ph_cache, g_strdup(path), e); }
        else g_ptr_array_set_size(e->classes, 0);
        e->mtime = mtime;
        if (sout) {
            gchar **rows = g_strsplit(sout, "\n", -1);
            for (int i = 0; rows[i]; i++)
                if (rows[i][0]) g_ptr_array_add(e->classes, g_strdup(rows[i]));
            g_strfreev(rows); g_free(sout);
        }
        ph_cache_dirty = TRUE;
    }
    e->seen = TRUE;
    for (guint i = 0; i < e->classes->len; i++)
        ph_emit_class(fmt, (const char *)g_ptr_array_index(e->classes, i), catalog);
}

int pluginhost_scan_helper_main(int argc, char **argv)
{
    if (argc < 4) return 2;
    int proto_fd = dup(STDOUT_FILENO);    /* metadata channel */
    dup2(STDERR_FILENO, STDOUT_FILENO);   /* plugin spew -> stderr */
    FILE *out = fdopen(proto_fd, "w");
    if (!out) return 2;

    PluginFormat fmt = (PluginFormat)-1;
    for (int i = 0; i < PH_NFORMATS; i++)
        if (!g_strcmp0(argv[2], ph_fmt_names[i])) fmt = (PluginFormat)i;
    const char *path = argv[3];

    GList *list = NULL;
    switch (fmt) {
#ifdef HAVE_VST2
        case PH_VST2:   ph_vst2_describe(path, &list);   break;
#endif
#ifdef HAVE_VST3
        case PH_VST3:   ph_vst3_describe(path, &list);   break;
#endif
#ifdef HAVE_CLAP
        case PH_CLAP:   ph_clap_describe(path, &list);   break;
#endif
#ifdef HAVE_LADSPA
        case PH_LADSPA: ph_ladspa_describe(path, &list); break;
#endif
        default: break;
    }
    for (GList *l = list; l; l = l->next) {
        PluginInfo *pi = l->data;
        GString *s = g_string_new("");
        ph_esc(s, pi->key);  g_string_append_c(s, '\t');
        ph_esc(s, pi->name); g_string_append_c(s, '\t');
        ph_esc(s, pi->category);
        fprintf(out, "%s\n", s->str);
        g_string_free(s, TRUE);
    }
    g_list_free_full(list, ph_info_free);
    fflush(out);
    return 0;
}

static void ph_do_scan(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    ph_cache_load();

#ifdef HAVE_LV2
    ph_lv2_scan(&ph_cat, ph_paths[PH_LV2]);     /* in-process: lilv reads .ttl */
#endif
#ifdef HAVE_VST2
    ph_vst2_scan(&ph_cat, ph_paths[PH_VST2]);   /* enumerate -> ph_scan_cached */
#endif
#ifdef HAVE_VST3
    ph_vst3_scan(&ph_cat, ph_paths[PH_VST3]);
#endif
#ifdef HAVE_CLAP
    ph_clap_scan(&ph_cat, ph_paths[PH_CLAP]);
#endif
#ifdef HAVE_LADSPA
    ph_ladspa_scan(&ph_cat, ph_paths[PH_LADSPA]);
#endif

    ph_cache_save();
    g_hash_table_destroy(ph_cache); ph_cache = NULL;

    /* De-duplicate by (format,key): overlapping search paths (e.g. a dir that is
     * symlinked under two names) must not list the same plugin twice. */
    {
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
        GList *out = NULL;
        for (GList *l = ph_cat; l; l = l->next) {
            PluginInfo *pi = l->data;
            gchar *id = g_strdup_printf("%d::%s", pi->format, pi->key);
            if (g_hash_table_contains(seen, id)) {
                g_free(id);
                g_free(pi->key); g_free(pi->name); g_free(pi->category); g_free(pi);
            } else {
                g_hash_table_add(seen, id);
                out = g_list_prepend(out, pi);
            }
        }
        g_list_free(ph_cat);
        ph_cat = g_list_reverse(out);
        g_hash_table_destroy(seen);
    }
    ph_scanned = TRUE;

    /* Diagnostics (visible in the terminal). */
    guint n[PH_NFORMATS] = {0};
    for (GList *l = ph_cat; l; l = l->next) {
        PluginInfo *pi = l->data;
        if (pi->format < PH_NFORMATS) n[pi->format]++;
    }
    g_message("plugin scan: LV2=%u VST2=%u VST3=%u CLAP=%u LADSPA=%u%s",
              n[PH_LV2], n[PH_VST2], n[PH_VST3], n[PH_CLAP], n[PH_LADSPA],
#ifdef HAVE_VST3
              "");
#else
              "  (VST3 backend not compiled — build with: make VST3=1)");
#endif
}

const GList *pluginhost_catalog(void)
{
    if (!ph_scanned) ph_do_scan();
    return ph_cat;
}

void pluginhost_rescan(void)
{
    ph_do_scan();
}

/* Persisted baseline of every plugin identity seen on the previous scan, used
 * to tell which plugins are newly added. Separate from plugincache because LV2
 * is scanned in-process and never touches that cache. */
static char *ph_index_path(void)
{ return g_build_filename(g_get_home_dir(), ".jackdaw", "pluginindex", NULL); }

/* Stable identity for diffing across runs: "<format>:<escaped key>". */
static char *ph_identity(const PluginInfo *pi)
{
    GString *s = g_string_new("");
    g_string_append_printf(s, "%d:", (int)pi->format);
    ph_esc(s, pi->key);
    return g_string_free(s, FALSE);
}

GList *pluginhost_scan_report_new(void)
{
    /* Load the previous identity set. On the very first run this stays empty,
     * so every plugin found is reported as new (seeding the baseline too). */
    GHashTable *prev = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    char *file = ph_index_path(), *data = NULL; gsize len = 0;
    if (g_file_get_contents(file, &data, &len, NULL)) {
        gchar **rows = g_strsplit(data, "\n", -1);
        for (int i = 0; rows[i]; i++)
            if (rows[i][0]) g_hash_table_add(prev, g_strdup(rows[i]));
        g_strfreev(rows);
        g_free(data);
    }

    ph_do_scan();

    /* Diff the fresh catalog against the baseline; collect new names and
     * rebuild the index to persist. */
    GList *new_names = NULL;
    GString *idx = g_string_new("");
    for (const GList *l = ph_cat; l; l = l->next) {
        const PluginInfo *pi = l->data;
        char *id = ph_identity(pi);
        g_string_append(idx, id);
        g_string_append_c(idx, '\n');
        if (!g_hash_table_contains(prev, id))
            new_names = g_list_prepend(new_names, g_strdup(pi->name));
        g_free(id);
    }
    new_names = g_list_reverse(new_names);

    g_file_set_contents(file, idx->str, idx->len, NULL);

    g_string_free(idx, TRUE);
    g_free(file);
    g_hash_table_destroy(prev);
    return new_names;
}

void pluginhost_add_search_path(PluginFormat fmt, const char *dir)
{
    if (fmt < 0 || fmt >= PH_NFORMATS || !dir || !*dir) return;
    for (GList *l = ph_paths[fmt]; l; l = l->next)
        if (g_strcmp0(l->data, dir) == 0) return;     /* dedupe */
    ph_paths[fmt] = g_list_append(ph_paths[fmt], g_strdup(dir));
}

void pluginhost_remove_search_path(PluginFormat fmt, const char *dir)
{
    if (fmt < 0 || fmt >= PH_NFORMATS || !dir) return;
    for (GList *l = ph_paths[fmt]; l; l = l->next) {
        if (g_strcmp0(l->data, dir) == 0) {
            g_free(l->data);
            ph_paths[fmt] = g_list_delete_link(ph_paths[fmt], l);
            return;
        }
    }
}

const GList *pluginhost_search_paths(PluginFormat fmt)
{
    if (fmt < 0 || fmt >= PH_NFORMATS) return NULL;
    return ph_paths[fmt];
}

static const char *ph_paths_key[PH_NFORMATS] = {
    "pluginPathsLV2", "pluginPathsVST2", "pluginPathsVST3",
    "pluginPathsCLAP", "pluginPathsLADSPA"
};

/* Common install locations seeded into the visible/scanned path list. */
static void ph_seed_default_paths(void)
{
    const char *home = g_get_home_dir();
    struct { PluginFormat fmt; const char *sub; } user[] = {
        { PH_LV2, ".lv2" }, { PH_VST2, ".vst" }, { PH_VST3, ".vst3" },
        { PH_CLAP, ".clap" }, { PH_LADSPA, ".ladspa" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(user); i++) {
        gchar *d = g_build_filename(home, user[i].sub, NULL);
        pluginhost_add_search_path(user[i].fmt, d);
        g_free(d);
    }
    pluginhost_add_search_path(PH_LV2, "/usr/lib/lv2");
    pluginhost_add_search_path(PH_LV2, "/usr/local/lib/lv2");
    pluginhost_add_search_path(PH_LV2, "/usr/lib/x86_64-linux-gnu/lv2");
    pluginhost_add_search_path(PH_VST2, "/usr/lib/vst");
    pluginhost_add_search_path(PH_VST2, "/usr/local/lib/vst");
    pluginhost_add_search_path(PH_VST3, "/usr/lib/vst3");
    pluginhost_add_search_path(PH_VST3, "/usr/local/lib/vst3");
    pluginhost_add_search_path(PH_VST3, "/usr/lib/x86_64-linux-gnu/vst3");
    pluginhost_add_search_path(PH_CLAP, "/usr/lib/clap");
    pluginhost_add_search_path(PH_CLAP, "/usr/local/lib/clap");
    pluginhost_add_search_path(PH_LADSPA, "/usr/lib/ladspa");
    pluginhost_add_search_path(PH_LADSPA, "/usr/local/lib/ladspa");
    pluginhost_add_search_path(PH_LADSPA, "/usr/lib/x86_64-linux-gnu/ladspa");
}

void pluginhost_load_paths_from_settings(void)
{
    ph_seed_default_paths();   /* common locations always present */
    for (int f = 0; f < PH_NFORMATS; f++) {
        gchar *s = settings_get_string(ph_paths_key[f], "");
        if (s && *s) {
            gchar **parts = g_strsplit(s, "\n", -1);
            for (gchar **p = parts; *p; p++)
                if (**p) pluginhost_add_search_path((PluginFormat)f, *p);
            g_strfreev(parts);
        }
        g_free(s);
    }
}

void pluginhost_save_paths_to_settings(void)
{
    for (int f = 0; f < PH_NFORMATS; f++) {
        GString *g = g_string_new("");
        for (GList *l = ph_paths[f]; l; l = l->next) {
            if (g->len) g_string_append_c(g, '\n');
            g_string_append(g, (char *)l->data);
        }
        settings_set_string(ph_paths_key[f], g->str);
        g_string_free(g, TRUE);
    }
}

/* ---- Instantiate / dispatch ---- */

PluginInstance *pluginhost_instantiate(const PluginInfo *info)
{
    if (!info) return NULL;
    PluginInstance *inst = NULL;
    switch (info->format) {
#ifdef HAVE_LV2
    case PH_LV2:  inst = ph_lv2_instantiate (info, ph_sr, ph_maxblock); break;
#endif
#ifdef HAVE_VST2
    case PH_VST2: inst = ph_vst2_instantiate(info, ph_sr, ph_maxblock); break;
#endif
#ifdef HAVE_VST3
    case PH_VST3: inst = ph_vst3_instantiate(info, ph_sr, ph_maxblock); break;
#endif
#ifdef HAVE_CLAP
    case PH_CLAP: inst = ph_clap_instantiate(info, ph_sr, ph_maxblock); break;
#endif
#ifdef HAVE_LADSPA
    case PH_LADSPA: inst = ph_ladspa_instantiate(info, ph_sr, ph_maxblock); break;
#endif
    default: return NULL;
    }
    if (inst) {
        inst->is_instrument = info->is_instrument;
        inst->key      = g_strdup(info->key);
        inst->category = g_strdup(info->category);
    }
    return inst;
}

gboolean pluginhost_is_instrument(PluginInstance *inst)
{
    return inst ? inst->is_instrument : FALSE;
}

PluginFormat pluginhost_format(PluginInstance *inst)
{ return inst ? inst->format : PH_LV2; }

const char *pluginhost_key(PluginInstance *inst)
{ return inst ? inst->key : NULL; }

const char *pluginhost_category(PluginInstance *inst)
{ return inst ? inst->category : NULL; }

void pluginhost_process_midi(PluginInstance *inst, const PhMidiEvent *ev,
                             int n_ev, float *L, float *R, int nframes)
{
    if (!inst || !inst->ops) return;
    if (!g_atomic_int_get(&inst->active)) return;   /* bypassed: leave L/R as-is */

    if (inst->ops->process_midi)
        inst->ops->process_midi(inst, ev, n_ev, L, R, nframes);
    else if (inst->ops->process)
        inst->ops->process(inst, L, R, nframes);    /* no MIDI path: audio only */

    /* Same speaker-safety net as pluginhost_process. */
    for (int i = 0; i < nframes; i++) {
        float a = L[i], b = R[i];
        if (!isfinite(a)) a = 0.0f; else if (a >  4.0f) a =  4.0f; else if (a < -4.0f) a = -4.0f;
        if (!isfinite(b)) b = 0.0f; else if (b >  4.0f) b =  4.0f; else if (b < -4.0f) b = -4.0f;
        L[i] = a; R[i] = b;
    }
}

void pluginhost_free(PluginInstance *inst)
{
    if (!inst) return;
    /* Tear the native editor (suil) down first, while the DSP instance it is
     * bound to (instance-access) is still alive; then destroy the cached widget
     * and free the backend. */
    if (inst->gui_native && inst->ops && inst->ops->destroy_gui)
        inst->ops->destroy_gui(inst);
    if (inst->gui) {
        gtk_widget_destroy(inst->gui);
        g_object_unref(inst->gui);
        inst->gui = NULL;
    }
    if (inst->ops && inst->ops->destroy) inst->ops->destroy(inst);
    g_free(inst->dry_L);
    g_free(inst->dry_R);
    g_free(inst->name);
    g_free(inst->key);
    g_free(inst->category);
    g_free(inst);
}

void pluginhost_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    if (!inst || !inst->ops || !inst->ops->process) return;
    if (!g_atomic_int_get(&inst->active)) return;   /* bypassed */

    int mq = g_atomic_int_get(&inst->mix_q15);
    gboolean blend = (mq < 32768) && inst->dry_L && inst->dry_R
                     && nframes <= inst->max_block;
    if (blend) {                       /* stash the dry signal for the mix */
        memcpy(inst->dry_L, L, (size_t)nframes * sizeof(float));
        memcpy(inst->dry_R, R, (size_t)nframes * sizeof(float));
    }

    inst->ops->process(inst, L, R, nframes);

    /* Safety net: a misbehaving plugin must never send NaN/inf or a runaway
     * level to the speakers. Replace non-finite samples and clamp magnitude. */
    for (int i = 0; i < nframes; i++) {
        float a = L[i], b = R[i];
        if (!isfinite(a)) a = 0.0f; else if (a >  4.0f) a =  4.0f; else if (a < -4.0f) a = -4.0f;
        if (!isfinite(b)) b = 0.0f; else if (b >  4.0f) b =  4.0f; else if (b < -4.0f) b = -4.0f;
        L[i] = a; R[i] = b;
    }

    if (blend) {                       /* wet/dry crossfade */
        float wet = mq * (1.0f / 32768.0f);
        float dry = 1.0f - wet;
        for (int i = 0; i < nframes; i++) {
            L[i] = inst->dry_L[i] * dry + L[i] * wet;
            R[i] = inst->dry_R[i] * dry + R[i] * wet;
        }
    }
}

void pluginhost_reset(PluginInstance *inst)
{
    if (inst && inst->ops && inst->ops->reset)
        inst->ops->reset(inst);
}

void pluginhost_set_active(PluginInstance *inst, gboolean on)
{
    if (inst) g_atomic_int_set(&inst->active, on ? 1 : 0);
}

void pluginhost_set_mix(PluginInstance *inst, float mix)
{
    if (!inst) return;
    if (mix < 0.0f) mix = 0.0f; else if (mix > 1.0f) mix = 1.0f;
    g_atomic_int_set(&inst->mix_q15, (int)(mix * 32768.0f + 0.5f));
}

float pluginhost_get_mix(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->mix_q15) * (1.0f / 32768.0f) : 1.0f;
}

gboolean pluginhost_is_active(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->active) != 0 : FALSE;
}

const char *pluginhost_name(PluginInstance *inst)
{
    return inst ? inst->name : "";
}

GtkWidget *pluginhost_make_gui(PluginInstance *inst)
{
    if (!inst) return gtk_label_new("(no plugin)");
    if (inst->gui) return inst->gui;     /* cached, owned by the instance */

    /* Editor selection, in order:
     *   1. in-process suil (X11/Gtk3 UIs) via the backend  -> gui_native
     *   2. out-of-process helper (GtkUI/Qt UIs) via the bridge (GtkSocket)
     *   3. generic parameter panel */
    GtkWidget *w = NULL;
    if (inst->ops && inst->ops->make_gui)
        w = inst->ops->make_gui(inst);
    if (w) {
        inst->gui_native = TRUE;          /* backend (suil) owns teardown */
    } else {
        w = lv2ui_bridge_new(inst);       /* helper socket; self-tears on destroy */
        if (!w) w = ph_generic_param_panel(inst);
        inst->gui_native = FALSE;
    }
    g_object_ref_sink(w);
    inst->gui = w;
    return w;
}

GtkWidget *pluginhost_peek_gui(PluginInstance *inst)
{
    return inst ? inst->gui : NULL;
}

/* ---- Out-of-process native UI support (LV2 only for now) ---- */

gboolean pluginhost_ui_meta(PluginInstance *inst, const char **plugin_uri,
                            const char **ui_uri, const char **ui_type)
{
#ifdef HAVE_LV2
    if (inst && inst->format == PH_LV2)
        return ph_lv2_ui_meta(inst, plugin_uri, ui_uri, ui_type);
#else
    (void)inst; (void)plugin_uri; (void)ui_uri; (void)ui_type;
#endif
    return FALSE;
}

void pluginhost_ctl_set(PluginInstance *inst, guint port, float v)
{
#ifdef HAVE_LV2
    if (inst && inst->format == PH_LV2) ph_lv2_ctl_set(inst, port, v);
#else
    (void)inst; (void)port; (void)v;
#endif
}

float pluginhost_ctl_get(PluginInstance *inst, guint port)
{
#ifdef HAVE_LV2
    if (inst && inst->format == PH_LV2) return ph_lv2_ctl_get(inst, port);
#else
    (void)inst; (void)port;
#endif
    return 0.0f;
}

void pluginhost_ctl_ports(PluginInstance *inst, gboolean outputs,
                          const guint **ports, guint *n)
{
    *ports = NULL; *n = 0;
#ifdef HAVE_LV2
    if (inst && inst->format == PH_LV2) ph_lv2_ctl_ports(inst, outputs, ports, n);
#else
    (void)inst; (void)outputs;
#endif
}

double pluginhost_sample_rate(PluginInstance *inst)
{
    return inst ? inst->sample_rate : 48000.0;
}

void pluginhost_release_gui(PluginInstance *inst)
{
    if (!inst || !inst->gui) return;
    if (inst->gui_native && inst->ops && inst->ops->destroy_gui)
        inst->ops->destroy_gui(inst);   /* free suil instance + push timer */
    gtk_widget_destroy(inst->gui);
    g_object_unref(inst->gui);          /* drop our ref_sink reference */
    inst->gui = NULL;
    inst->gui_native = FALSE;
}

/* ---- Generic parameter passthrough ---- */

guint pluginhost_param_count(PluginInstance *inst)
{
    return (inst && inst->ops && inst->ops->param_count)
           ? inst->ops->param_count(inst) : 0;
}
const char *pluginhost_param_name(PluginInstance *inst, guint i)
{
    return (inst && inst->ops && inst->ops->param_name)
           ? inst->ops->param_name(inst, i) : "";
}
float pluginhost_param_get(PluginInstance *inst, guint i)
{
    return (inst && inst->ops && inst->ops->param_get)
           ? inst->ops->param_get(inst, i) : 0.0f;
}
void pluginhost_param_set(PluginInstance *inst, guint i, float v)
{
    if (inst && inst->ops && inst->ops->param_set) inst->ops->param_set(inst, i, v);
}
void pluginhost_param_range(PluginInstance *inst, guint i, float *mn, float *mx)
{
    if (inst && inst->ops && inst->ops->param_range)
        inst->ops->param_range(inst, i, mn, mx);
    else { if (mn) *mn = 0.0f; if (mx) *mx = 1.0f; }
}

/* ---- Generic parameter panel (fallback GUI) ---- */

typedef struct { PluginInstance *inst; guint idx; } ParamLink;

static void ph_param_slider_changed(GtkRange *r, gpointer data)
{
    ParamLink *pl = data;
    pluginhost_param_set(pl->inst, pl->idx, (float)gtk_range_get_value(r));
}

GtkWidget *ph_generic_param_panel(PluginInstance *inst)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);

    guint n = pluginhost_param_count(inst);
    if (n == 0) {
        gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("This plugin exposes no editable parameters."),
            0, 0, 2, 1);
    }
    for (guint i = 0; i < n; i++) {
        float mn = 0.0f, mx = 1.0f;
        pluginhost_param_range(inst, i, &mn, &mx);
        if (mx <= mn) mx = mn + 1.0f;

        GtkWidget *lbl = gtk_label_new(pluginhost_param_name(inst, i));
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(lbl, 160, -1);

        double step = (mx - mn) / 200.0;
        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                 mn, mx, step > 0 ? step : 0.001);
        gtk_range_set_value(GTK_RANGE(sc), pluginhost_param_get(inst, i));
        gtk_widget_set_hexpand(sc, TRUE);
        gtk_widget_set_size_request(sc, 220, -1);

        ParamLink *pl = g_new0(ParamLink, 1);
        pl->inst = inst; pl->idx = i;
        g_object_set_data_full(G_OBJECT(sc), "param-link", pl, g_free);
        g_signal_connect(sc, "value-changed",
                         G_CALLBACK(ph_param_slider_changed), pl);

        gtk_grid_attach(GTK_GRID(grid), lbl, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), sc,  1, (int)i, 1, 1);
    }

    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_widget_show_all(scroll);
    return scroll;
}

void pluginhost_shutdown(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    for (int i = 0; i < PH_NFORMATS; i++) {
        g_list_free_full(ph_paths[i], g_free);
        ph_paths[i] = NULL;
    }
    (void)ph_inited;
}
