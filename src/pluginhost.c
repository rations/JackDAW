#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#include "pluginhost.h"
#include "pluginhost_internal.h"
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
    return pi;
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
    inst->sample_rate = sr;
    inst->max_block   = max_block;
    return inst;
}

/* ---- Catalog ---- */

static void ph_do_scan(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;

#ifdef HAVE_LV2
    ph_lv2_scan(&ph_cat, ph_paths[PH_LV2]);
#endif
#ifdef HAVE_VST2
    ph_vst2_scan(&ph_cat, ph_paths[PH_VST2]);
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

void pluginhost_load_paths_from_settings(void)
{
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
    switch (info->format) {
#ifdef HAVE_LV2
    case PH_LV2:  return ph_lv2_instantiate (info, ph_sr, ph_maxblock);
#endif
#ifdef HAVE_VST2
    case PH_VST2: return ph_vst2_instantiate(info, ph_sr, ph_maxblock);
#endif
#ifdef HAVE_VST3
    case PH_VST3: return ph_vst3_instantiate(info, ph_sr, ph_maxblock);
#endif
#ifdef HAVE_CLAP
    case PH_CLAP: return ph_clap_instantiate(info, ph_sr, ph_maxblock);
#endif
#ifdef HAVE_LADSPA
    case PH_LADSPA: return ph_ladspa_instantiate(info, ph_sr, ph_maxblock);
#endif
    default: return NULL;
    }
}

void pluginhost_free(PluginInstance *inst)
{
    if (!inst) return;
    /* Generic panels are owned by the host; native (suil/X11) editors are freed
     * by the backend's destroy(). */
    if (inst->gui && !inst->gui_native)
        gtk_widget_destroy(inst->gui);
    if (inst->ops && inst->ops->destroy) inst->ops->destroy(inst);
    if (inst->gui)
        g_object_unref(inst->gui);
    g_free(inst->name);
    g_free(inst);
}

void pluginhost_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    if (!inst || !inst->ops || !inst->ops->process) return;
    if (!g_atomic_int_get(&inst->active)) return;   /* bypassed */
    inst->ops->process(inst, L, R, nframes);
}

void pluginhost_set_active(PluginInstance *inst, gboolean on)
{
    if (inst) g_atomic_int_set(&inst->active, on ? 1 : 0);
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

    GtkWidget *w = NULL;
    if (inst->ops && inst->ops->make_gui)
        w = inst->ops->make_gui(inst);
    if (w) {
        inst->gui_native = TRUE;
    } else {
        w = ph_generic_param_panel(inst);
        inst->gui_native = FALSE;
    }
    g_object_ref_sink(w);
    inst->gui = w;
    return w;
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
