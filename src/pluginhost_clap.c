#define _GNU_SOURCE
#include <config.h>

#ifdef HAVE_CLAP

#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

#include "clap/clap.h"
#include "pluginhost_internal.h"

#define CLAP_MAX_PENDING 256

/* ---- Host ---- */

static const void *clap_host_get_ext(const struct clap_host *h, const char *id)
{ (void)h; (void)id; return NULL; }
static void clap_host_noop(const struct clap_host *h) { (void)h; }

static const clap_host_t clap_host = {
    CLAP_VERSION_INIT,
    NULL, "JackDAW", "JackDAW", "0.1", "0.1",
    clap_host_get_ext, clap_host_noop, clap_host_noop, clap_host_noop
};

typedef struct {
    void                       *dl;
    const clap_plugin_entry_t  *entry;
    const clap_plugin_t        *plugin;
    const clap_plugin_params_t *params;

    clap_id  *param_ids;
    guint     n_params;

    /* pending param edits flushed into in_events on the next process */
    clap_event_param_value_t pending[CLAP_MAX_PENDING];
    volatile gint            n_pending;

    float    *outL, *outR;
    int       max_block;
} ClapBackend;

/* ---- Input/output event adapters ---- */

static uint32_t clap_in_size(const struct clap_input_events *l)
{ ClapBackend *b = (ClapBackend *)l->ctx; return (uint32_t)g_atomic_int_get(&b->n_pending); }

static const clap_event_header_t *clap_in_get(const struct clap_input_events *l, uint32_t i)
{ ClapBackend *b = (ClapBackend *)l->ctx;
  return (i < (uint32_t)b->n_pending) ? &b->pending[i].header : NULL; }

static bool clap_out_push(const struct clap_output_events *l,
                          const clap_event_header_t *e)
{ (void)l; (void)e; return true; }

/* ---- Scan ---- */

static const char *clap_category(const clap_plugin_descriptor_t *d)
{
    if (d->features) {
        for (const char *const *f = d->features; *f; f++) {
            if (g_strcmp0(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0) return "Audio Effect";
            if (g_strcmp0(*f, "reverb") == 0) return "Reverb";
            if (g_strcmp0(*f, "delay")  == 0) return "Delay";
            if (g_strcmp0(*f, "filter") == 0) return "Filter";
            if (g_strcmp0(*f, "equalizer") == 0) return "EQ";
        }
        if (d->features[0]) return d->features[0];
    }
    return "CLAP";
}

/* Load+describe one .clap — runs only in the out-of-process scanner. */
void ph_clap_describe(const char *path, GList **catalog)
{
    if (!ph_path_is_safe(path)) return;
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return;
    const clap_plugin_entry_t *entry =
        (const clap_plugin_entry_t *)dlsym(dl, "clap_entry");
    if (!entry || !entry->init || !entry->get_factory) { dlclose(dl); return; }
    if (!entry->init(path)) { dlclose(dl); return; }

    const clap_plugin_factory_t *fac =
        (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (fac) {
        uint32_t n = fac->get_plugin_count(fac);
        for (uint32_t i = 0; i < n; i++) {
            const clap_plugin_descriptor_t *d = fac->get_plugin_descriptor(fac, i);
            if (!d || !d->id) continue;
            /* key = "path\nplugin-id" */
            gchar *key = g_strdup_printf("%s\n%s", path, d->id);
            *catalog = g_list_prepend(*catalog,
                ph_info_new(PH_CLAP, key, d->name ? d->name : d->id,
                            clap_category(d)));
            g_free(key);
        }
    }
    if (entry->deinit) entry->deinit();
    dlclose(dl);
}

static void clap_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 6) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR))
            clap_scan_dir(full, catalog, depth + 1);
        else if (g_str_has_suffix(e, ".clap"))
            ph_scan_cached(PH_CLAP, full, catalog);
        g_free(full);
    }
    g_dir_close(d);
}

void ph_clap_scan(GList **catalog, const GList *extra)
{
    for (const GList *l = extra; l; l = l->next)
        clap_scan_dir((const char *)l->data, catalog, 0);
}

/* ---- Ops ---- */

static void clap_process_cb(PluginInstance *pi, float *L, float *R, int n)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    if (n > b->max_block) n = b->max_block;

    float *in_ch[2]  = { L, R };
    float *out_ch[2] = { b->outL, b->outR };
    clap_audio_buffer_t in  = { in_ch,  NULL, 2, 0, 0 };
    clap_audio_buffer_t out = { out_ch, NULL, 2, 0, 0 };

    clap_input_events_t  ine = { b, clap_in_size, clap_in_get };
    clap_output_events_t oute = { b, clap_out_push };

    clap_process_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.steady_time        = -1;
    pr.frames_count       = (uint32_t)n;
    pr.transport          = NULL;
    pr.audio_inputs       = &in;
    pr.audio_outputs      = &out;
    pr.audio_inputs_count = 1;
    pr.audio_outputs_count= 1;
    pr.in_events          = &ine;
    pr.out_events         = &oute;

    if (b->plugin && b->plugin->process)
        b->plugin->process(b->plugin, &pr);
    g_atomic_int_set(&b->n_pending, 0);    /* events consumed */

    memcpy(L, b->outL, (size_t)n * sizeof(float));
    memcpy(R, b->outR, (size_t)n * sizeof(float));
}

static void clap_destroy(PluginInstance *pi)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    if (!b) return;
    if (b->plugin) {
        if (b->plugin->stop_processing) b->plugin->stop_processing(b->plugin);
        if (b->plugin->deactivate)      b->plugin->deactivate(b->plugin);
        if (b->plugin->destroy)         b->plugin->destroy(b->plugin);
    }
    if (b->entry && b->entry->deinit) b->entry->deinit();
    if (b->dl) dlclose(b->dl);
    g_free(b->param_ids);
    g_free(b->outL); g_free(b->outR);
    g_free(b);
}

static guint clap_param_count(PluginInstance *pi)
{ return ((ClapBackend *)pi->backend)->n_params; }

static const char *clap_param_name(PluginInstance *pi, guint i)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    static char buf[CLAP_NAME_SIZE];
    buf[0] = 0;
    if (b->params && i < b->n_params) {
        clap_param_info_t info;
        if (b->params->get_info(b->plugin, i, &info))
            g_strlcpy(buf, info.name, sizeof(buf));
    }
    return buf[0] ? buf : "param";
}

static float clap_param_get(PluginInstance *pi, guint i)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    double v = 0.0;
    if (b->params && i < b->n_params)
        b->params->get_value(b->plugin, b->param_ids[i], &v);
    return (float)v;
}

static void clap_param_set(PluginInstance *pi, guint i, float v)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    if (i >= b->n_params) return;
    gint slot = g_atomic_int_get(&b->n_pending);
    if (slot >= CLAP_MAX_PENDING) return;
    clap_event_param_value_t *ev = &b->pending[slot];
    memset(ev, 0, sizeof(*ev));
    ev->header.size     = sizeof(*ev);
    ev->header.time     = 0;
    ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev->header.type     = CLAP_EVENT_PARAM_VALUE;
    ev->param_id  = b->param_ids[i];
    ev->note_id   = -1;
    ev->port_index= -1;
    ev->channel   = -1;
    ev->key       = -1;
    ev->value     = v;
    g_atomic_int_set(&b->n_pending, slot + 1);
}

static void clap_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{
    ClapBackend *b = (ClapBackend *)pi->backend;
    if (b->params && i < b->n_params) {
        clap_param_info_t info;
        if (b->params->get_info(b->plugin, i, &info)) {
            if (mn) *mn = (float)info.min_value;
            if (mx) *mx = (float)info.max_value;
            return;
        }
    }
    if (mn) *mn = 0.0f;
    if (mx) *mx = 1.0f;
}

static const PhOps clap_ops = {
    clap_process_cb, NULL /*process_midi*/, clap_destroy, NULL, NULL,
    clap_param_count, clap_param_name, clap_param_get, clap_param_set,
    clap_param_range
};

/* ---- Instantiate ---- */

PluginInstance *ph_clap_instantiate(const PluginInfo *info, double sr, int max_block)
{
    gchar **parts = g_strsplit(info->key, "\n", 2);
    if (!parts[0] || !parts[1]) { g_strfreev(parts); return NULL; }
    const char *path = parts[0];
    const char *pid  = parts[1];

    if (!ph_path_is_safe(path)) { g_strfreev(parts); return NULL; }
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { g_strfreev(parts); return NULL; }
    const clap_plugin_entry_t *entry =
        (const clap_plugin_entry_t *)dlsym(dl, "clap_entry");
    if (!entry || !entry->init(path)) { dlclose(dl); g_strfreev(parts); return NULL; }
    const clap_plugin_factory_t *fac =
        (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!fac) { if (entry->deinit) entry->deinit(); dlclose(dl); g_strfreev(parts); return NULL; }

    const clap_plugin_t *plugin = fac->create_plugin(fac, &clap_host, pid);
    g_strfreev(parts);
    if (!plugin) { if (entry->deinit) entry->deinit(); dlclose(dl); return NULL; }
    if (!plugin->init(plugin)) {
        plugin->destroy(plugin);
        if (entry->deinit) entry->deinit();
        dlclose(dl); return NULL;
    }
    plugin->activate(plugin, sr, 1, (uint32_t)max_block);
    if (plugin->start_processing) plugin->start_processing(plugin);

    ClapBackend *b = g_new0(ClapBackend, 1);
    b->dl = dl; b->entry = entry; b->plugin = plugin;
    b->max_block = max_block;
    b->outL = g_new0(float, max_block);
    b->outR = g_new0(float, max_block);

    b->params = (const clap_plugin_params_t *)
        plugin->get_extension(plugin, CLAP_EXT_PARAMS);
    if (b->params) {
        b->n_params  = b->params->count(plugin);
        b->param_ids = g_new0(clap_id, b->n_params ? b->n_params : 1);
        for (guint i = 0; i < b->n_params; i++) {
            clap_param_info_t pinf;
            if (b->params->get_info(plugin, i, &pinf))
                b->param_ids[i] = pinf.id;
        }
    }

    PluginInstance *pi = ph_instance_alloc(PH_CLAP, info->name, sr, max_block);
    pi->ops = &clap_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_CLAP */
