#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#ifdef HAVE_LV2

#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#ifdef HAVE_SUIL
#  include <suil/suil.h>
#endif

#include "pluginhost_internal.h"

#define LV2_GTK3_UI_URI "http://lv2plug.in/ns/extensions/ui#Gtk3UI"

/* ---- Shared LilvWorld + cached nodes ---- */

static LilvWorld *world;
static LilvNode  *n_audio, *n_control, *n_input, *n_output;

static void lv2_world_init(void)
{
    if (world) return;
    world = lilv_world_new();
    lilv_world_load_all(world);
    n_audio   = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    n_control = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
    n_input   = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    n_output  = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
}

/* ---- Minimal URID map feature (thread-safe) ---- */

static GHashTable *urid_table;        /* uri string -> GUINT id */
static guint32     urid_next = 1;
static GMutex      urid_lock;

static LV2_URID urid_map_cb(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    g_mutex_lock(&urid_lock);
    if (!urid_table)
        urid_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer v = g_hash_table_lookup(urid_table, uri);
    guint32 id;
    if (v) {
        id = GPOINTER_TO_UINT(v);
    } else {
        id = urid_next++;
        g_hash_table_insert(urid_table, g_strdup(uri), GUINT_TO_POINTER(id));
    }
    g_mutex_unlock(&urid_lock);
    return id;
}

static LV2_URID_Map    urid_map    = { NULL, urid_map_cb };
static LV2_Feature     feat_map    = { LV2_URID__map, &urid_map };
static const LV2_Feature *lv2_features[] = { &feat_map, NULL };

/* ---- Backend instance ---- */

typedef struct {
    guint  port_index;
    char  *name;
    float  min, max;
} Lv2Param;

typedef struct {
    const LilvPlugin *plugin;
    LilvInstance     *inst[2];   /* [1] only for dual-mono */
    int               n_inst;
    gboolean          dual_mono;

    guint   n_ports;
    float  *ctl;                 /* per-port control value store (n_ports) */
    float   ctl_out_dummy;

    int     audio_in[2],  n_audio_in;
    int     audio_out[2], n_audio_out;

    float  *outA, *outB;         /* scratch out buffers (max_block) */
    int     max_block;

    Lv2Param *params;
    guint     n_params;

#ifdef HAVE_SUIL
    SuilInstance *ui;
#endif
} Lv2Backend;

/* ---- Scan ---- */

void ph_lv2_scan(GList **catalog, const GList *extra)
{
    lv2_world_init();
    for (const GList *l = extra; l; l = l->next) {
        /* lilv uses LV2_PATH; extra dirs are best-effort via env at startup.
         * Here we just reload so newly-dropped bundles in known dirs appear. */
        (void)l;
    }
    lilv_world_load_all(world);

    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin *p = lilv_plugins_get(plugins, i);
        const char *uri = lilv_node_as_uri(lilv_plugin_get_uri(p));

        LilvNode *nm = lilv_plugin_get_name(p);
        const char *name = nm ? lilv_node_as_string(nm) : uri;

        const char *cat = "LV2";
        const LilvPluginClass *cl = lilv_plugin_get_class(p);
        if (cl) {
            const LilvNode *lbl = lilv_plugin_class_get_label(cl);
            if (lbl) cat = lilv_node_as_string(lbl);
        }
        *catalog = g_list_prepend(*catalog,
                                  ph_info_new(PH_LV2, uri, name, cat));
        if (nm) lilv_node_free(nm);
    }
}

/* ---- Ops ---- */

static void lv2_process(PluginInstance *pi, float *L, float *R, int n)
{
    Lv2Backend *b = pi->backend;
    if (n > b->max_block) n = b->max_block;
    if (b->n_audio_in == 0 || b->n_audio_out == 0) return;

    if (b->dual_mono) {
        lilv_instance_connect_port(b->inst[0], b->audio_in[0],  L);
        lilv_instance_connect_port(b->inst[0], b->audio_out[0], b->outA);
        lilv_instance_connect_port(b->inst[1], b->audio_in[0],  R);
        lilv_instance_connect_port(b->inst[1], b->audio_out[0], b->outB);
        lilv_instance_run(b->inst[0], n);
        lilv_instance_run(b->inst[1], n);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        memcpy(R, b->outB, (size_t)n * sizeof(float));
    } else {
        lilv_instance_connect_port(b->inst[0], b->audio_in[0], L);
        if (b->n_audio_in  > 1) lilv_instance_connect_port(b->inst[0], b->audio_in[1], R);
        lilv_instance_connect_port(b->inst[0], b->audio_out[0], b->outA);
        if (b->n_audio_out > 1) lilv_instance_connect_port(b->inst[0], b->audio_out[1], b->outB);
        lilv_instance_run(b->inst[0], n);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        if (b->n_audio_out > 1) memcpy(R, b->outB, (size_t)n * sizeof(float));
        else                    memcpy(R, b->outA, (size_t)n * sizeof(float));
    }
}

static void lv2_destroy(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!b) return;
#ifdef HAVE_SUIL
    if (b->ui) { suil_instance_free(b->ui); b->ui = NULL; }
#endif
    for (int i = 0; i < b->n_inst; i++) {
        if (b->inst[i]) {
            lilv_instance_deactivate(b->inst[i]);
            lilv_instance_free(b->inst[i]);
        }
    }
    for (guint i = 0; i < b->n_params; i++) g_free(b->params[i].name);
    g_free(b->params);
    g_free(b->ctl);
    g_free(b->outA);
    g_free(b->outB);
    g_free(b);
}

static guint lv2_param_count(PluginInstance *pi)
{ return ((Lv2Backend *)pi->backend)->n_params; }

static const char *lv2_param_name(PluginInstance *pi, guint i)
{ Lv2Backend *b = pi->backend; return i < b->n_params ? b->params[i].name : ""; }

static float lv2_param_get(PluginInstance *pi, guint i)
{ Lv2Backend *b = pi->backend;
  return i < b->n_params ? b->ctl[b->params[i].port_index] : 0.0f; }

static void lv2_param_set(PluginInstance *pi, guint i, float v)
{ Lv2Backend *b = pi->backend;
  if (i < b->n_params) b->ctl[b->params[i].port_index] = v; }

static void lv2_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{ Lv2Backend *b = pi->backend;
  if (i < b->n_params) { if (mn) *mn = b->params[i].min; if (mx) *mx = b->params[i].max; } }

/* ---- Native GUI via suil (falls back to the generic panel on failure) ---- */

#ifdef HAVE_SUIL
static SuilHost *suil_host;

static void lv2_ui_write(SuilController controller, uint32_t port,
                         uint32_t size, uint32_t protocol, const void *buffer)
{
    PluginInstance *pi = (PluginInstance *)controller;
    Lv2Backend *b = pi->backend;
    if (protocol == 0 && size == sizeof(float) && port < b->n_ports)
        b->ctl[port] = *(const float *)buffer;   /* control-port edit from UI */
}

static GtkWidget *lv2_make_gui(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!suil_host)
        suil_host = suil_host_new(lv2_ui_write, NULL, NULL, NULL);
    if (!suil_host) return NULL;

    LilvUIs *uis = lilv_plugin_get_uis(b->plugin);
    if (!uis) return NULL;

    LilvNode *gtk3 = lilv_new_uri(world, LV2_GTK3_UI_URI);
    const LilvUI   *use_ui   = NULL;
    const LilvNode *use_type = NULL;
    LILV_FOREACH(uis, i, uis) {
        const LilvUI *ui = lilv_uis_get(uis, i);
        const LilvNode *type = NULL;
        if (lilv_ui_is_supported(ui, suil_ui_supported, gtk3, &type)) {
            use_ui = ui; use_type = type; break;
        }
    }
    if (!use_ui) { lilv_node_free(gtk3); lilv_uis_free(uis); return NULL; }

    const char *plugin_uri = lilv_node_as_uri(lilv_plugin_get_uri(b->plugin));
    const char *ui_uri     = lilv_node_as_uri(lilv_ui_get_uri(use_ui));
    const char *ui_type    = lilv_node_as_uri(use_type);
    char *bundle = lilv_node_get_path(lilv_ui_get_bundle_uri(use_ui), NULL);
    char *binary = lilv_node_get_path(lilv_ui_get_binary_uri(use_ui), NULL);

    b->ui = suil_instance_new(suil_host, pi, LV2_GTK3_UI_URI,
                              plugin_uri, ui_uri, ui_type,
                              bundle ? bundle : "", binary ? binary : "",
                              lv2_features);
    lilv_free(bundle);
    lilv_free(binary);
    lilv_node_free(gtk3);
    lilv_uis_free(uis);

    if (!b->ui) return NULL;
    GtkWidget *w = (GtkWidget *)suil_instance_get_widget(b->ui);
    return w;   /* NULL → caller falls back to the generic panel */
}
#else
static GtkWidget *lv2_make_gui(PluginInstance *pi) { (void)pi; return NULL; }
#endif

static const PhOps lv2_ops = {
    .process     = lv2_process,
    .destroy     = lv2_destroy,
    .make_gui    = lv2_make_gui,
    .param_count = lv2_param_count,
    .param_name  = lv2_param_name,
    .param_get   = lv2_param_get,
    .param_set   = lv2_param_set,
    .param_range = lv2_param_range,
};

/* ---- Instantiate ---- */

PluginInstance *ph_lv2_instantiate(const PluginInfo *info, double sr, int max_block)
{
    lv2_world_init();
    LilvNode *uri = lilv_new_uri(world, info->key);
    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    const LilvPlugin  *p = lilv_plugins_get_by_uri(plugins, uri);
    lilv_node_free(uri);
    if (!p) return NULL;

    Lv2Backend *b = g_new0(Lv2Backend, 1);
    b->plugin    = p;
    b->max_block = max_block;
    b->n_ports   = lilv_plugin_get_num_ports(p);
    b->ctl       = g_new0(float, b->n_ports);
    b->outA      = g_new0(float, max_block);
    b->outB      = g_new0(float, max_block);

    /* Default control values */
    float *mins = g_new0(float, b->n_ports);
    float *maxs = g_new0(float, b->n_ports);
    float *defs = g_new0(float, b->n_ports);
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    GArray *params = g_array_new(FALSE, FALSE, sizeof(Lv2Param));

    for (guint i = 0; i < b->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
        gboolean is_audio = lilv_port_is_a(p, port, n_audio);
        gboolean is_ctl   = lilv_port_is_a(p, port, n_control);
        gboolean is_in    = lilv_port_is_a(p, port, n_input);
        gboolean is_out   = lilv_port_is_a(p, port, n_output);

        if (is_audio) {
            if (is_in  && b->n_audio_in  < 2) b->audio_in [b->n_audio_in++ ] = (int)i;
            if (is_out && b->n_audio_out < 2) b->audio_out[b->n_audio_out++] = (int)i;
        } else if (is_ctl) {
            float d = defs[i];
            if (d != d) d = 0.0f;   /* NaN guard */
            b->ctl[i] = d;
            if (is_in) {
                Lv2Param pr;
                pr.port_index = i;
                LilvNode *pn = lilv_port_get_name(p, port);
                pr.name = g_strdup(pn ? lilv_node_as_string(pn) : "param");
                if (pn) lilv_node_free(pn);
                pr.min = mins[i]; pr.max = maxs[i];
                if (pr.max <= pr.min) pr.max = pr.min + 1.0f;
                g_array_append_val(params, pr);
            }
        }
        (void)is_out;
    }
    g_free(mins); g_free(maxs); g_free(defs);

    b->n_params = params->len;
    b->params   = (Lv2Param *)g_array_free(params, FALSE);

    b->dual_mono = (b->n_audio_in == 1 && b->n_audio_out == 1);
    b->n_inst    = b->dual_mono ? 2 : 1;

    for (int k = 0; k < b->n_inst; k++) {
        b->inst[k] = lilv_plugin_instantiate(p, sr, lv2_features);
        if (!b->inst[k]) {
            for (int j = 0; j < k; j++) { lilv_instance_free(b->inst[j]); }
            for (guint q = 0; q < b->n_params; q++) g_free(b->params[q].name);
            g_free(b->params); g_free(b->ctl);
            g_free(b->outA); g_free(b->outB); g_free(b);
            return NULL;
        }
        /* Connect all control ports to the shared value store; control
         * outputs to a dummy. Audio ports are connected per process call. */
        for (guint i = 0; i < b->n_ports; i++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
            if (lilv_port_is_a(p, port, n_control)) {
                if (lilv_port_is_a(p, port, n_input))
                    lilv_instance_connect_port(b->inst[k], i, &b->ctl[i]);
                else
                    lilv_instance_connect_port(b->inst[k], i, &b->ctl_out_dummy);
            }
        }
        lilv_instance_activate(b->inst[k]);
    }

    LilvNode *nm = lilv_plugin_get_name(p);
    PluginInstance *pi = ph_instance_alloc(PH_LV2,
        nm ? lilv_node_as_string(nm) : info->name, sr, max_block);
    if (nm) lilv_node_free(nm);
    pi->ops     = &lv2_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_LV2 */
