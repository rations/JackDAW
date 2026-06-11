#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#ifdef HAVE_LV2

#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/atom/atom.h>
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/data-access/data-access.h>
#include <jack/ringbuffer.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <glib/gprintf.h>
#include <lv2/ui/ui.h>
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
    n_audio   = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    n_control = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
    n_input   = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    n_output  = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
}

/* Build the LV2_PATH lilv should search: the user's env (or the standard
 * directories) plus any extra dirs added in the FX window. */
static void lv2_apply_search_path(const GList *extra)
{
    GString *p = g_string_new("");
    /* `extra` is the full search-path list (common LV2 dirs are seeded in). */
    for (const GList *l = extra; l; l = l->next) {
        if (p->len) g_string_append_c(p, ':');
        g_string_append(p, (const char *)l->data);
    }
    const char *env = g_getenv("LV2_PATH");
    if (env && *env) {
        if (p->len) g_string_append_c(p, ':');
        g_string_append(p, env);
    }
    if (p->len == 0)
        g_string_append(p, "/usr/lib/lv2:/usr/lib/x86_64-linux-gnu/lv2");
    LilvNode *node = lilv_new_string(world, p->str);
    lilv_world_set_option(world, LILV_OPTION_LV2_PATH, node);
    lilv_node_free(node);
    g_string_free(p, TRUE);
}

/* ---- Minimal URID map feature (thread-safe) ---- */

static GHashTable *urid_table;        /* uri string -> GUINT id */
static GPtrArray  *urid_rev;          /* id -> uri string (index = id) */
static guint32     urid_next = 1;
static GMutex      urid_lock;

static LV2_URID urid_map_cb(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    g_mutex_lock(&urid_lock);
    if (!urid_table) {
        urid_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        urid_rev   = g_ptr_array_new();
        g_ptr_array_add(urid_rev, (gpointer)"");   /* id 0 reserved */
    }
    gpointer v = g_hash_table_lookup(urid_table, uri);
    guint32 id;
    if (v) {
        id = GPOINTER_TO_UINT(v);
    } else {
        id = urid_next++;
        char *dup = g_strdup(uri);
        g_hash_table_insert(urid_table, dup, GUINT_TO_POINTER(id));
        g_ptr_array_add(urid_rev, dup);            /* index == id */
    }
    g_mutex_unlock(&urid_lock);
    return id;
}

static const char *urid_unmap_cb(LV2_URID_Unmap_Handle h, LV2_URID urid)
{
    (void)h;
    const char *s = NULL;
    g_mutex_lock(&urid_lock);
    if (urid_rev && urid < urid_rev->len)
        s = g_ptr_array_index(urid_rev, urid);
    g_mutex_unlock(&urid_lock);
    return s;
}

static LV2_URID_Map    urid_map    = { NULL, urid_map_cb };
static LV2_URID_Unmap  urid_unmap  = { NULL, urid_unmap_cb };
static LV2_Feature     feat_map    = { LV2_URID__map,   &urid_map };
static LV2_Feature     feat_unmap  = { LV2_URID__unmap, &urid_unmap };
static LV2_Feature     feat_bounded= { LV2_BUF_SIZE__boundedBlockLength, NULL };
static LV2_Feature     feat_powof2 = { LV2_BUF_SIZE__powerOf2BlockLength, NULL };

/* ---- Log feature (shared) ---- */

static int log_vprintf_cb(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
    (void)h; (void)type;
    return g_vfprintf(stderr, fmt, ap);
}
static int log_printf_cb(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = log_vprintf_cb(h, type, fmt, ap);
    va_end(ap);
    return r;
}
static LV2_Log_Log  lv2_log     = { NULL, log_printf_cb, log_vprintf_cb };
static LV2_Feature  feat_log    = { LV2_LOG__log, &lv2_log };

/* ---- Worker feature (per instance) ----
 * The plugin calls schedule_work() from the RT run() thread; we copy the job
 * to a lock-free ringbuffer and wake a dedicated worker thread, which runs
 * work() and pushes any response back through a second ringbuffer. On the next
 * process() we feed those responses to work_response() (RT thread). */

#define WORKER_BUF_BYTES 8192

typedef struct {
    const LV2_Worker_Interface *iface;
    LV2_Handle                  plugin;     /* lilv instance handle */
    jack_ringbuffer_t          *requests;   /* RT  -> worker */
    jack_ringbuffer_t          *responses;  /* worker -> RT  */
    sem_t                       sem;
    GThread                    *thread;
    volatile gint               quit;
    gboolean                    active;
    LV2_Worker_Schedule         schedule;   /* handle = this Worker */
    LV2_Feature                 feature;    /* worker:schedule */
} Worker;

static LV2_Worker_Status worker_respond_cb(LV2_Worker_Respond_Handle h,
                                           uint32_t size, const void *data)
{
    Worker *w = h;
    if (jack_ringbuffer_write_space(w->responses) < sizeof(size) + size)
        return LV2_WORKER_ERR_NO_SPACE;
    jack_ringbuffer_write(w->responses, (const char *)&size, sizeof(size));
    jack_ringbuffer_write(w->responses, (const char *)data, size);
    return LV2_WORKER_SUCCESS;
}

static gpointer worker_thread_fn(gpointer arg)
{
    Worker *w = arg;
    char buf[WORKER_BUF_BYTES];
    for (;;) {
        sem_wait(&w->sem);
        if (g_atomic_int_get(&w->quit)) break;
        uint32_t size;
        while (jack_ringbuffer_read_space(w->requests) >= sizeof(size)) {
            jack_ringbuffer_read(w->requests, (char *)&size, sizeof(size));
            if (size > sizeof(buf)) {           /* oversized: drain & drop */
                jack_ringbuffer_read_advance(w->requests, size);
                continue;
            }
            jack_ringbuffer_read(w->requests, buf, size);
            if (w->iface && w->iface->work)
                w->iface->work(w->plugin, worker_respond_cb, w, size, buf);
        }
    }
    return NULL;
}

/* RT-safe: copy job to ringbuffer and signal the worker. No malloc/lock. */
static LV2_Worker_Status worker_schedule_cb(LV2_Worker_Schedule_Handle h,
                                            uint32_t size, const void *data)
{
    Worker *w = h;
    if (!w->requests) return LV2_WORKER_ERR_UNKNOWN;   /* not yet wired */
    if (jack_ringbuffer_write_space(w->requests) < sizeof(size) + size)
        return LV2_WORKER_ERR_NO_SPACE;
    jack_ringbuffer_write(w->requests, (const char *)&size, sizeof(size));
    jack_ringbuffer_write(w->requests, (const char *)data, size);
    sem_post(&w->sem);
    return LV2_WORKER_SUCCESS;
}

/* Apply pending worker responses then end_run — called from the RT thread. */
static void worker_apply_responses(Worker *w)
{
    if (!w->active || !w->iface) return;
    char buf[WORKER_BUF_BYTES];
    uint32_t size;
    while (jack_ringbuffer_read_space(w->responses) >= sizeof(size)) {
        jack_ringbuffer_read(w->responses, (char *)&size, sizeof(size));
        if (size > sizeof(buf)) { jack_ringbuffer_read_advance(w->responses, size); continue; }
        jack_ringbuffer_read(w->responses, buf, size);
        if (w->iface->work_response)
            w->iface->work_response(w->plugin, size, buf);
    }
    if (w->iface->end_run) w->iface->end_run(w->plugin);
}

static void worker_init(Worker *w, LilvInstance *inst)
{
    const LV2_Worker_Interface *iface = (const LV2_Worker_Interface *)
        lilv_instance_get_extension_data(inst, LV2_WORKER__interface);
    if (!iface) { w->active = FALSE; return; }
    w->iface     = iface;
    w->plugin    = lilv_instance_get_handle(inst);
    w->requests  = jack_ringbuffer_create(WORKER_BUF_BYTES * 4);
    w->responses = jack_ringbuffer_create(WORKER_BUF_BYTES * 4);
    sem_init(&w->sem, 0, 0);
    w->quit      = 0;
    w->thread    = g_thread_new("lv2-worker", worker_thread_fn, w);
    w->active    = TRUE;
}

static void worker_destroy(Worker *w)
{
    if (!w->active) return;
    g_atomic_int_set(&w->quit, 1);
    sem_post(&w->sem);
    if (w->thread) g_thread_join(w->thread);
    sem_destroy(&w->sem);
    if (w->requests)  jack_ringbuffer_free(w->requests);
    if (w->responses) jack_ringbuffer_free(w->responses);
    w->active = FALSE;
}

/* The set of feature URIs this host can satisfy. We provide all the standard
 * ones (map/unmap, options, buf-size, worker, log); a plugin that *requires*
 * something beyond this set is skipped at instantiate rather than run, so an
 * unimplemented extension can never hard-crash the whole audio engine. */
static gboolean lv2_feature_supported(const char *uri)
{
    static const char *ok[] = {
        LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength, LV2_BUF_SIZE__powerOf2BlockLength,
        LV2_WORKER__schedule, LV2_LOG__log,
        NULL
    };
    for (int i = 0; ok[i]; i++) if (!strcmp(uri, ok[i])) return TRUE;
    return FALSE;
}

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

    int    *ain;  int n_audio_in;   /* ALL audio input port indices  */
    int    *aout; int n_audio_out;  /* ALL audio output port indices */

    float  *outA, *outB;         /* scratch out buffers (max_block) */
    float  *dummy_in, *dummy_out;/* scratch for surplus audio ports */
    void   *misc;                /* zeroed buffer for atom/event/CV/other ports */
    int     max_block;

    Lv2Param *params;
    guint     n_params;

    /* Per-instance feature block (the options carry this instance's block
     * length and sample rate, so they cannot live in a shared global). */
    int32_t   opt_minblk, opt_maxblk, opt_seqsize;
    float     opt_srate;
    LV2_Options_Option options[5];
    LV2_Feature feat_opts;
    Worker      workers[2];        /* one per instance (worker:schedule handle) */
    const LV2_Feature *features[2][10]; /* per-instance feature list */

#ifdef HAVE_SUIL
    SuilInstance *ui;
    guint         ui_idle_id;    /* g_timeout driving suil/X11 UI idle */
    LV2_Extension_Data_Feature ui_ext_data; /* backing for data-access feature */
#endif
} Lv2Backend;

/* ---- Scan ---- */

void ph_lv2_scan(GList **catalog, const GList *extra)
{
    lv2_world_init();
    lv2_apply_search_path(extra);
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
        lilv_instance_connect_port(b->inst[0], b->ain[0],  L);
        lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
        lilv_instance_connect_port(b->inst[1], b->ain[0],  R);
        lilv_instance_connect_port(b->inst[1], b->aout[0], b->outB);
        lilv_instance_run(b->inst[0], n);
        lilv_instance_run(b->inst[1], n);
        worker_apply_responses(&b->workers[0]);
        worker_apply_responses(&b->workers[1]);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        memcpy(R, b->outB, (size_t)n * sizeof(float));
    } else {
        /* Route the first one/two audio ports to L/R; any surplus audio ports
         * are bound to scratch so run() never touches an unconnected port. */
        lilv_instance_connect_port(b->inst[0], b->ain[0], L);
        if (b->n_audio_in  > 1) lilv_instance_connect_port(b->inst[0], b->ain[1], R);
        for (int i = 2; i < b->n_audio_in; i++)
            lilv_instance_connect_port(b->inst[0], b->ain[i], b->dummy_in);

        lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
        if (b->n_audio_out > 1) lilv_instance_connect_port(b->inst[0], b->aout[1], b->outB);
        for (int i = 2; i < b->n_audio_out; i++)
            lilv_instance_connect_port(b->inst[0], b->aout[i], b->dummy_out);

        lilv_instance_run(b->inst[0], n);
        worker_apply_responses(&b->workers[0]);
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
    if (b->ui_idle_id) { g_source_remove(b->ui_idle_id); b->ui_idle_id = 0; }
    if (b->ui) { suil_instance_free(b->ui); b->ui = NULL; }
#endif
    for (int i = 0; i < b->n_inst; i++) {
        if (b->inst[i]) {
            lilv_instance_deactivate(b->inst[i]);
            worker_destroy(&b->workers[i]);   /* stop thread before freeing */
            lilv_instance_free(b->inst[i]);
        }
    }
    for (guint i = 0; i < b->n_params; i++) g_free(b->params[i].name);
    g_free(b->params);
    g_free(b->ctl);
    g_free(b->ain); g_free(b->aout);
    g_free(b->outA);
    g_free(b->outB);
    g_free(b->dummy_in); g_free(b->dummy_out);
    g_free(b->misc);
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

/* ---- Native GUI via suil ----
 * Embeds the plugin's own editor (Gtk3 UIs directly; X11/other UIs wrapped by
 * suil). suil-wrapped UIs require the host to drive an idle loop, otherwise they
 * never repaint and crash on interaction — that is done by lv2_ui_idle_cb. */

/* suil_init() must run once, after gtk_init() and before any UI is wrapped, or
 * the X11-in-Gtk3 wrapper is not set up and editors come up blank. */
void ph_lv2_ui_init(int *argc, char ***argv)
{
#ifdef HAVE_SUIL
    static gboolean done = FALSE;
    if (done) return;
    suil_init(argc, argv, SUIL_ARG_NONE);
    done = TRUE;
#else
    (void)argc; (void)argv;
#endif
}

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

static uint32_t lv2_ui_port_index(SuilController controller, const char *symbol)
{
    PluginInstance *pi = (PluginInstance *)controller;
    Lv2Backend *b = pi->backend;
    for (guint i = 0; i < b->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(b->plugin, i);
        const LilvNode *sym = lilv_port_get_symbol(b->plugin, port);
        if (sym && !g_strcmp0(lilv_node_as_string(sym), symbol)) return i;
    }
    return LV2UI_INVALID_PORT_INDEX;
}

/* Drive the UI's idle interface (required for suil-wrapped X11 UIs) and feed it
 * the latest control values so its widgets track the DSP state. */
static gboolean lv2_ui_idle_cb(gpointer data)
{
    Lv2Backend *b = data;
    if (!b->ui) return G_SOURCE_REMOVE;
    /* The editor widget persists on the instance but is detached from the FX
     * window when the effect is deselected/removed/closed. Servicing the UI's
     * idle while its widget is unrealized crashes the suil X11 wrapper, so only
     * drive idle when the widget is actually realized on screen. */
    GtkWidget *w = (GtkWidget *)suil_instance_get_widget(b->ui);
    if (!w || !gtk_widget_get_mapped(w)) return G_SOURCE_CONTINUE;
    const LV2UI_Idle_Interface *idle =
        suil_instance_extension_data(b->ui, LV2_UI__idleInterface);
    if (idle && idle->idle)
        idle->idle(suil_instance_get_handle(b->ui));
    return G_SOURCE_CONTINUE;
}

static GtkWidget *lv2_make_gui(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!suil_host)
        suil_host = suil_host_new(lv2_ui_write, lv2_ui_port_index, NULL, NULL);
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

    /* The editor gets its OWN feature list — NOT the DSP one. It must not be
     * handed worker:schedule (which can confuse a UI), but it DOES need
     * instance-access + data-access so UIs like guitarix can bind directly to
     * the running DSP instance (without them they render blank / inert). */
    b->ui_ext_data.data_access =
        lilv_instance_get_descriptor(b->inst[0])->extension_data;
    LV2_Feature feat_inst_access = {
        LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(b->inst[0]) };
    LV2_Feature feat_data_access = {
        LV2_DATA_ACCESS_URI, &b->ui_ext_data };
    /* Many X11 UIs (e.g. guitarix) declare ui:idleInterface as a REQUIRED
     * feature: the host must pass it to signal it will drive idle(). Without it
     * the UI fails to instantiate and suil shows an empty wrapper. Worker/
     * options/log belong to the DSP, not the editor. */
    LV2_Feature feat_ui_idle = { LV2_UI__idleInterface, NULL };
    const LV2_Feature *ui_features[] = {
        &feat_map, &feat_inst_access, &feat_data_access, &feat_ui_idle, NULL };

    b->ui = suil_instance_new(suil_host, pi, LV2_GTK3_UI_URI,
                              plugin_uri, ui_uri, ui_type,
                              bundle ? bundle : "", binary ? binary : "",
                              ui_features);
    lilv_free(bundle);
    lilv_free(binary);
    lilv_node_free(gtk3);
    lilv_uis_free(uis);

    if (!b->ui) return NULL;   /* no usable native UI → caller uses generic panel */
    GtkWidget *w = (GtkWidget *)suil_instance_get_widget(b->ui);
    if (!w) { suil_instance_free(b->ui); b->ui = NULL; return NULL; }

    /* Seed the UI with the current input control values so its knobs/sliders
     * start in the right position. */
    for (guint i = 0; i < b->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(b->plugin, i);
        if (lilv_port_is_a(b->plugin, port, n_control) &&
            lilv_port_is_a(b->plugin, port, n_input))
            suil_instance_port_event(b->ui, i, sizeof(float), 0, &b->ctl[i]);
    }

    b->ui_idle_id = g_timeout_add(30, lv2_ui_idle_cb, b);
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

    /* Skip plugins that require a feature we cannot satisfy (e.g. worker
     * threads, state). Running them would dereference a missing feature and
     * crash the RT thread — better to never load them. */
    LilvNodes *req = lilv_plugin_get_required_features(p);
    if (req) {
        gboolean ok = TRUE;
        LILV_FOREACH(nodes, it, req) {
            const LilvNode *f = lilv_nodes_get(req, it);
            const char *furi = lilv_node_as_uri(f);
            if (!furi || !lv2_feature_supported(furi)) {
                g_message("LV2: skipping %s — requires unsupported feature %s",
                          info->name, furi ? furi : "(?)");
                ok = FALSE; break;
            }
        }
        lilv_nodes_free(req);
        if (!ok) return NULL;
    }

    Lv2Backend *b = g_new0(Lv2Backend, 1);
    b->plugin    = p;
    b->max_block = max_block;
    b->n_ports   = lilv_plugin_get_num_ports(p);
    b->ctl       = g_new0(float, b->n_ports);
    b->outA      = g_new0(float, max_block);
    b->outB      = g_new0(float, max_block);
    b->dummy_in  = g_new0(float, max_block);
    b->dummy_out = g_new0(float, max_block);
    /* Zeroed scratch for any non-audio/non-control port (atom/event/CV). LV2
     * requires EVERY port be connected before run(); leaving one NULL crashes
     * the plugin. A zeroed buffer reads as an empty atom sequence. */
    b->misc      = g_malloc0(MAX((gsize)max_block * sizeof(float), (gsize)8192));
    b->ain       = g_new0(int, b->n_ports ? b->n_ports : 1);
    b->aout      = g_new0(int, b->n_ports ? b->n_ports : 1);

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
            if (is_in)  b->ain [b->n_audio_in++ ] = (int)i;
            if (is_out) b->aout[b->n_audio_out++] = (int)i;
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

    /* Build the Options feature: block length + sample rate. Many plugins
     * (e.g. guitarix) require these and crash without them. */
    b->opt_minblk = 1;
    b->opt_maxblk = max_block;
    b->opt_seqsize = (int32_t)MAX((gsize)max_block * sizeof(float), (gsize)8192);
    b->opt_srate  = (float)sr;
    const LV2_URID urid_int   = urid_map_cb(NULL, LV2_ATOM__Int);
    const LV2_URID urid_float = urid_map_cb(NULL, LV2_ATOM__Float);
    LV2_Options_Option *o = b->options;
    o[0] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0,
        urid_map_cb(NULL, LV2_BUF_SIZE__minBlockLength),
        sizeof(int32_t), urid_int, &b->opt_minblk };
    o[1] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0,
        urid_map_cb(NULL, LV2_BUF_SIZE__maxBlockLength),
        sizeof(int32_t), urid_int, &b->opt_maxblk };
    o[2] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0,
        urid_map_cb(NULL, LV2_BUF_SIZE__sequenceSize),
        sizeof(int32_t), urid_int, &b->opt_seqsize };
    o[3] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0,
        urid_map_cb(NULL, LV2_PARAMETERS__sampleRate),
        sizeof(float), urid_float, &b->opt_srate };
    o[4] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL };
    b->feat_opts.URI  = LV2_OPTIONS__options;
    b->feat_opts.data = b->options;

    for (int k = 0; k < b->n_inst; k++) {
        /* Each instance gets its own worker:schedule handle (so dual-mono
         * instances don't share a job queue). The ringbuffers/thread are
         * created in worker_init() after instantiation, once we know whether
         * the plugin actually implements the worker interface. */
        Worker *w = &b->workers[k];
        w->schedule.handle = w;
        w->schedule.schedule_work = worker_schedule_cb;
        w->feature.URI  = LV2_WORKER__schedule;
        w->feature.data = &w->schedule;

        int nf = 0;
        b->features[k][nf++] = &feat_map;
        b->features[k][nf++] = &feat_unmap;
        b->features[k][nf++] = &b->feat_opts;
        b->features[k][nf++] = &feat_bounded;
        b->features[k][nf++] = &feat_powof2;
        b->features[k][nf++] = &feat_log;
        b->features[k][nf++] = &w->feature;
        b->features[k][nf]   = NULL;

        b->inst[k] = lilv_plugin_instantiate(p, sr, b->features[k]);
        if (!b->inst[k]) {
            for (int j = 0; j < k; j++) { worker_destroy(&b->workers[j]); lilv_instance_free(b->inst[j]); }
            for (guint q = 0; q < b->n_params; q++) g_free(b->params[q].name);
            g_free(b->params); g_free(b->ctl);
            g_free(b->ain); g_free(b->aout);
            g_free(b->outA); g_free(b->outB);
            g_free(b->dummy_in); g_free(b->dummy_out); g_free(b->misc);
            g_free(b);
            return NULL;
        }
        /* Connect EVERY port up front. Control ports → shared value store
         * (outputs → dummy). Audio ports are (re)connected per process call,
         * but bind them to scratch now so an unused one is never NULL. Any
         * other port type (atom/event/CV) → the zeroed misc buffer. */
        for (guint i = 0; i < b->n_ports; i++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
            if (lilv_port_is_a(p, port, n_control)) {
                if (lilv_port_is_a(p, port, n_input))
                    lilv_instance_connect_port(b->inst[k], i, &b->ctl[i]);
                else
                    lilv_instance_connect_port(b->inst[k], i, &b->ctl_out_dummy);
            } else if (lilv_port_is_a(p, port, n_audio)) {
                lilv_instance_connect_port(b->inst[k], i,
                    lilv_port_is_a(p, port, n_input) ? b->dummy_in : b->dummy_out);
            } else {
                lilv_instance_connect_port(b->inst[k], i, b->misc);
            }
        }
        lilv_instance_activate(b->inst[k]);
        /* Now that the instance exists, wire up the worker thread if the
         * plugin implements the worker interface. */
        worker_init(&b->workers[k], b->inst[k]);
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
