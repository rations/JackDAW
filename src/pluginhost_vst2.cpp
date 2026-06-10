#include <config.h>

#ifdef HAVE_VST2

#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

#include "vestige/aeffectx.h"
#include "pluginhost_internal.h"

/* VST2 opcodes we use (values from the VST 2.4 ABI). */
#define EFF_OPEN            0
#define EFF_CLOSE           1
#define EFF_SET_SAMPLE_RATE 10
#define EFF_SET_BLOCK_SIZE  11
#define EFF_MAINS_CHANGED   12
#define EFF_GET_PARAM_NAME  8
#define EFF_GET_EFFECT_NAME 45
#define EFF_GET_PLUG_CATEGORY 35

typedef AEffect *(VST_CALL_CONV *Vst2EntryProc)(audioMasterCallback);

/* Minimal host callback: most plugins only need a sane version number. */
static intptr_t VST_CALL_CONV vst2_master(AEffect *e, int op, int idx,
                                          intptr_t val, void *ptr, float opt)
{
    (void)e; (void)idx; (void)val; (void)ptr; (void)opt;
    switch (op) {
    case 1:  return 2400;   /* audioMasterVersion */
    default: return 0;
    }
}

typedef struct {
    void    *dl;
    AEffect *eff;
    float   *outL, *outR;   /* scratch (not guaranteed in-place) */
    int      max_block;
} Vst2Backend;

static AEffect *vst2_load(const char *path, void **dl_out)
{
    if (!ph_path_is_safe(path)) return NULL;
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return NULL;
    Vst2EntryProc entry = (Vst2EntryProc)dlsym(dl, "VSTPluginMain");
    if (!entry) entry = (Vst2EntryProc)dlsym(dl, "main");
    if (!entry) { dlclose(dl); return NULL; }
    AEffect *eff = entry(vst2_master);
    if (!eff || eff->magic != kEffectMagic) { dlclose(dl); return NULL; }
    *dl_out = dl;
    return eff;
}

/* ---- Scan ---- */

static void vst2_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 6) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            vst2_scan_dir(full, catalog, depth + 1);
        } else if (g_str_has_suffix(e, ".so")) {
            void *dl = NULL;
            AEffect *eff = vst2_load(full, &dl);
            if (eff) {
                char nm[128] = {0};
                eff->dispatcher(eff, EFF_GET_EFFECT_NAME, 0, 0, nm, 0.0f);
                if (!nm[0]) g_strlcpy(nm, e, sizeof(nm));
                *catalog = g_list_prepend(*catalog,
                    ph_info_new(PH_VST2, full, nm, "VST2"));
                if (dl) dlclose(dl);
            }
        }
        g_free(full);
    }
    g_dir_close(d);
}

void ph_vst2_scan(GList **catalog, const GList *extra)
{
    gchar *home = g_build_filename(g_get_home_dir(), ".vst", NULL);
    vst2_scan_dir(home, catalog, 0);
    g_free(home);
    const char *envp = g_getenv("VST_PATH");
    if (envp) {
        gchar **parts = g_strsplit(envp, ":", -1);
        for (gchar **p = parts; *p; p++) if (**p) vst2_scan_dir(*p, catalog, 0);
        g_strfreev(parts);
    }
    for (const GList *l = extra; l; l = l->next)
        vst2_scan_dir((const char *)l->data, catalog, 0);
}

/* ---- Ops ---- */

static void vst2_process(PluginInstance *pi, float *L, float *R, int n)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (n > b->max_block) n = b->max_block;
    AEffect *e = b->eff;
    float *ins[2]  = { L, (e->numInputs > 1) ? R : L };
    float *outs[2] = { b->outL, (e->numOutputs > 1) ? b->outR : b->outL };
    if (!e->processReplacing) return;
    e->processReplacing(e, ins, outs, n);
    memcpy(L, b->outL, (size_t)n * sizeof(float));
    memcpy(R, (e->numOutputs > 1) ? b->outR : b->outL, (size_t)n * sizeof(float));
}

static void vst2_destroy(PluginInstance *pi)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (!b) return;
    if (b->eff) {
        b->eff->dispatcher(b->eff, EFF_MAINS_CHANGED, 0, 0, NULL, 0.0f);
        b->eff->dispatcher(b->eff, EFF_CLOSE, 0, 0, NULL, 0.0f);
    }
    if (b->dl) dlclose(b->dl);
    g_free(b->outL); g_free(b->outR);
    g_free(b);
}

static guint vst2_param_count(PluginInstance *pi)
{ return (guint)((Vst2Backend *)pi->backend)->eff->numParams; }

static const char *vst2_param_name(PluginInstance *pi, guint i)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    static char buf[128];   /* GUI is single-threaded; fine */
    buf[0] = 0;
    b->eff->dispatcher(b->eff, EFF_GET_PARAM_NAME, (int)i, 0, buf, 0.0f);
    return buf[0] ? buf : "param";
}

static float vst2_param_get(PluginInstance *pi, guint i)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    return b->eff->getParameter ? b->eff->getParameter(b->eff, (int)i) : 0.0f;
}

static void vst2_param_set(PluginInstance *pi, guint i, float v)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (b->eff->setParameter) b->eff->setParameter(b->eff, (int)i, v);
}

static void vst2_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{ (void)pi; (void)i; if (mn) *mn = 0.0f; if (mx) *mx = 1.0f; }  /* normalised */

static const PhOps vst2_ops = {
    vst2_process, vst2_destroy, NULL,
    vst2_param_count, vst2_param_name, vst2_param_get, vst2_param_set,
    vst2_param_range
};

/* ---- Instantiate ---- */

PluginInstance *ph_vst2_instantiate(const PluginInfo *info, double sr, int max_block)
{
    void *dl = NULL;
    AEffect *eff = vst2_load(info->key, &dl);
    if (!eff) return NULL;

    Vst2Backend *b = g_new0(Vst2Backend, 1);
    b->dl = dl; b->eff = eff; b->max_block = max_block;
    b->outL = g_new0(float, max_block);
    b->outR = g_new0(float, max_block);

    eff->dispatcher(eff, EFF_OPEN, 0, 0, NULL, 0.0f);
    eff->dispatcher(eff, EFF_SET_SAMPLE_RATE, 0, 0, NULL, (float)sr);
    eff->dispatcher(eff, EFF_SET_BLOCK_SIZE, 0, max_block, NULL, 0.0f);
    eff->dispatcher(eff, EFF_MAINS_CHANGED, 0, 1, NULL, 0.0f);

    char nm[128] = {0};
    eff->dispatcher(eff, EFF_GET_EFFECT_NAME, 0, 0, nm, 0.0f);
    PluginInstance *pi = ph_instance_alloc(PH_VST2,
        nm[0] ? nm : info->name, sr, max_block);
    pi->ops = &vst2_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_VST2 */
