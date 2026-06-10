#define _GNU_SOURCE
#include <config.h>

#ifdef HAVE_LADSPA

#include <dlfcn.h>
#include <string.h>
#include <math.h>

#include "ladspa.h"
#include "pluginhost_internal.h"

typedef struct {
    guint  port;
    char  *name;
    float  min, max;
} LadParam;

typedef struct {
    void                  *dl;
    const LADSPA_Descriptor *desc;
    LADSPA_Handle          h[2];
    int                    n_inst;
    gboolean               dual_mono;

    float    *ctl;          /* per-port control store (PortCount) */
    float     ctl_out_dummy;
    int       audio_in[2],  n_audio_in;
    int       audio_out[2], n_audio_out;
    float    *outA, *outB;
    int       max_block;

    LadParam *params;
    guint     n_params;
} LadBackend;

/* ---- Range-hint helpers ---- */

static void lad_hint(const LADSPA_PortRangeHint *hint, double sr,
                     float *mn, float *mx, float *def)
{
    LADSPA_PortRangeHintDescriptor d = hint->HintDescriptor;
    float lo = LADSPA_IS_HINT_BOUNDED_BELOW(d) ? hint->LowerBound : 0.0f;
    float hi = LADSPA_IS_HINT_BOUNDED_ABOVE(d) ? hint->UpperBound : 1.0f;
    if (LADSPA_IS_HINT_SAMPLE_RATE(d)) { lo *= (float)sr; hi *= (float)sr; }
    if (hi <= lo) hi = lo + 1.0f;

    float df = lo;
    if (LADSPA_IS_HINT_HAS_DEFAULT(d)) {
        if      (LADSPA_IS_HINT_DEFAULT_MINIMUM(d)) df = lo;
        else if (LADSPA_IS_HINT_DEFAULT_LOW(d))     df = lo * 0.75f + hi * 0.25f;
        else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(d))  df = (lo + hi) * 0.5f;
        else if (LADSPA_IS_HINT_DEFAULT_HIGH(d))    df = lo * 0.25f + hi * 0.75f;
        else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(d)) df = hi;
        else if (LADSPA_IS_HINT_DEFAULT_0(d))       df = 0.0f;
        else if (LADSPA_IS_HINT_DEFAULT_1(d))       df = 1.0f;
        else if (LADSPA_IS_HINT_DEFAULT_100(d))     df = 100.0f;
        else if (LADSPA_IS_HINT_DEFAULT_440(d))     df = 440.0f;
    }
    if (mn)  *mn  = lo;
    if (mx)  *mx  = hi;
    if (def) *def = df;
}

/* ---- Scan ---- */

static void lad_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 5) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            lad_scan_dir(full, catalog, depth + 1);
        } else if (g_str_has_suffix(e, ".so")) {
            if (ph_path_is_safe(full)) {
                void *dl = dlopen(full, RTLD_NOW | RTLD_LOCAL);
                if (dl) {
                    LADSPA_Descriptor_Function df =
                        (LADSPA_Descriptor_Function)dlsym(dl, "ladspa_descriptor");
                    if (df) {
                        for (unsigned long i = 0; ; i++) {
                            const LADSPA_Descriptor *de = df(i);
                            if (!de) break;
                            gchar *key = g_strdup_printf("%s\n%lu", full, i);
                            *catalog = g_list_prepend(*catalog,
                                ph_info_new(PH_LADSPA, key,
                                            de->Name ? de->Name : de->Label,
                                            "LADSPA"));
                            g_free(key);
                        }
                    }
                    dlclose(dl);
                }
            }
        }
        g_free(full);
    }
    g_dir_close(d);
}

void ph_ladspa_scan(GList **catalog, const GList *extra)
{
    lad_scan_dir("/usr/lib/ladspa", catalog, 0);
    lad_scan_dir("/usr/local/lib/ladspa", catalog, 0);
    lad_scan_dir("/usr/lib/x86_64-linux-gnu/ladspa", catalog, 0);
    gchar *home = g_build_filename(g_get_home_dir(), ".ladspa", NULL);
    lad_scan_dir(home, catalog, 0);
    g_free(home);
    const char *env = g_getenv("LADSPA_PATH");
    if (env) {
        gchar **parts = g_strsplit(env, ":", -1);
        for (gchar **p = parts; *p; p++) if (**p) lad_scan_dir(*p, catalog, 0);
        g_strfreev(parts);
    }
    for (const GList *l = extra; l; l = l->next)
        lad_scan_dir((const char *)l->data, catalog, 0);
}

/* ---- Ops ---- */

static void lad_process(PluginInstance *pi, float *L, float *R, int n)
{
    LadBackend *b = pi->backend;
    if (n > b->max_block) n = b->max_block;
    if (b->n_audio_in == 0 || b->n_audio_out == 0) return;
    const LADSPA_Descriptor *d = b->desc;

    if (b->dual_mono) {
        d->connect_port(b->h[0], b->audio_in[0],  L);
        d->connect_port(b->h[0], b->audio_out[0], b->outA);
        d->connect_port(b->h[1], b->audio_in[0],  R);
        d->connect_port(b->h[1], b->audio_out[0], b->outB);
        d->run(b->h[0], (unsigned long)n);
        d->run(b->h[1], (unsigned long)n);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        memcpy(R, b->outB, (size_t)n * sizeof(float));
    } else {
        d->connect_port(b->h[0], b->audio_in[0], L);
        if (b->n_audio_in  > 1) d->connect_port(b->h[0], b->audio_in[1], R);
        d->connect_port(b->h[0], b->audio_out[0], b->outA);
        if (b->n_audio_out > 1) d->connect_port(b->h[0], b->audio_out[1], b->outB);
        d->run(b->h[0], (unsigned long)n);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        memcpy(R, (b->n_audio_out > 1) ? b->outB : b->outA, (size_t)n * sizeof(float));
    }
}

static void lad_destroy(PluginInstance *pi)
{
    LadBackend *b = pi->backend;
    if (!b) return;
    for (int i = 0; i < b->n_inst; i++) {
        if (b->h[i]) {
            if (b->desc->deactivate) b->desc->deactivate(b->h[i]);
            if (b->desc->cleanup)    b->desc->cleanup(b->h[i]);
        }
    }
    for (guint i = 0; i < b->n_params; i++) g_free(b->params[i].name);
    g_free(b->params);
    g_free(b->ctl);
    g_free(b->outA); g_free(b->outB);
    if (b->dl) dlclose(b->dl);
    g_free(b);
}

static guint lad_param_count(PluginInstance *pi)
{ return ((LadBackend *)pi->backend)->n_params; }
static const char *lad_param_name(PluginInstance *pi, guint i)
{ LadBackend *b = pi->backend; return i < b->n_params ? b->params[i].name : ""; }
static float lad_param_get(PluginInstance *pi, guint i)
{ LadBackend *b = pi->backend; return i < b->n_params ? b->ctl[b->params[i].port] : 0.0f; }
static void lad_param_set(PluginInstance *pi, guint i, float v)
{ LadBackend *b = pi->backend; if (i < b->n_params) b->ctl[b->params[i].port] = v; }
static void lad_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{ LadBackend *b = pi->backend;
  if (i < b->n_params) { if (mn) *mn = b->params[i].min; if (mx) *mx = b->params[i].max; } }

static const PhOps lad_ops = {
    lad_process, lad_destroy, NULL,
    lad_param_count, lad_param_name, lad_param_get, lad_param_set, lad_param_range
};

/* ---- Instantiate ---- */

PluginInstance *ph_ladspa_instantiate(const PluginInfo *info, double sr, int max_block)
{
    gchar **parts = g_strsplit(info->key, "\n", 2);
    if (!parts[0] || !parts[1]) { g_strfreev(parts); return NULL; }
    const char  *path = parts[0];
    unsigned long idx = strtoul(parts[1], NULL, 10);
    if (!ph_path_is_safe(path)) { g_strfreev(parts); return NULL; }

    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { g_strfreev(parts); return NULL; }
    LADSPA_Descriptor_Function df =
        (LADSPA_Descriptor_Function)dlsym(dl, "ladspa_descriptor");
    const LADSPA_Descriptor *desc = df ? df(idx) : NULL;
    g_strfreev(parts);
    if (!desc) { dlclose(dl); return NULL; }

    LadBackend *b = g_new0(LadBackend, 1);
    b->dl = dl; b->desc = desc; b->max_block = max_block;
    b->ctl  = g_new0(float, desc->PortCount ? desc->PortCount : 1);
    b->outA = g_new0(float, max_block);
    b->outB = g_new0(float, max_block);

    GArray *params = g_array_new(FALSE, FALSE, sizeof(LadParam));
    for (unsigned long i = 0; i < desc->PortCount; i++) {
        LADSPA_PortDescriptor pd = desc->PortDescriptors[i];
        if (LADSPA_IS_PORT_AUDIO(pd)) {
            if (LADSPA_IS_PORT_INPUT(pd)  && b->n_audio_in  < 2) b->audio_in [b->n_audio_in++ ] = (int)i;
            if (LADSPA_IS_PORT_OUTPUT(pd) && b->n_audio_out < 2) b->audio_out[b->n_audio_out++] = (int)i;
        } else if (LADSPA_IS_PORT_CONTROL(pd)) {
            float mn, mx, dfl;
            lad_hint(&desc->PortRangeHints[i], sr, &mn, &mx, &dfl);
            b->ctl[i] = dfl;
            if (LADSPA_IS_PORT_INPUT(pd)) {
                LadParam p;
                p.port = (guint)i;
                p.name = g_strdup(desc->PortNames[i] ? desc->PortNames[i] : "param");
                p.min = mn; p.max = mx;
                g_array_append_val(params, p);
            }
        }
    }
    b->n_params = params->len;
    b->params   = (LadParam *)g_array_free(params, FALSE);

    b->dual_mono = (b->n_audio_in == 1 && b->n_audio_out == 1);
    b->n_inst    = b->dual_mono ? 2 : 1;

    for (int k = 0; k < b->n_inst; k++) {
        b->h[k] = desc->instantiate(desc, (unsigned long)sr);
        if (!b->h[k]) {
            for (int j = 0; j < k; j++)
                if (desc->cleanup) desc->cleanup(b->h[j]);
            for (guint q = 0; q < b->n_params; q++) g_free(b->params[q].name);
            g_free(b->params); g_free(b->ctl);
            g_free(b->outA); g_free(b->outB); dlclose(dl); g_free(b);
            return NULL;
        }
        /* Connect control ports to the shared value store. */
        for (unsigned long i = 0; i < desc->PortCount; i++) {
            LADSPA_PortDescriptor pd = desc->PortDescriptors[i];
            if (LADSPA_IS_PORT_CONTROL(pd)) {
                if (LADSPA_IS_PORT_INPUT(pd))
                    desc->connect_port(b->h[k], i, &b->ctl[i]);
                else
                    desc->connect_port(b->h[k], i, &b->ctl_out_dummy);
            }
        }
        if (desc->activate) desc->activate(b->h[k]);
    }

    PluginInstance *pi = ph_instance_alloc(PH_LADSPA,
        desc->Name ? desc->Name : info->name, sr, max_block);
    pi->ops = &lad_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_LADSPA */
