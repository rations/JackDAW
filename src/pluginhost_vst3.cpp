#include <config.h>

#ifdef HAVE_VST3

#include <string>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include "pluginhost_internal.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

struct Vst3Backend {
    VST3::Hosting::Module::Ptr     module;
    IPtr<PlugProvider>             provider;
    IPtr<IComponent>               component;
    IPtr<IEditController>          controller;
    IPtr<IAudioProcessor>          processor;
    HostProcessData                data;
    ProcessContext                 ctx;
    ParameterChanges               in_params;
    int                            max_block = 0;
    std::vector<ParamID>           param_ids;
};

/* ---- Scan ---- */

static const char *vst3_module_so(const char *bundle, std::string &store)
{
    /* Accept either a bare .so or a .vst3 bundle directory. Module::create
     * understands both, so just return the given path. */
    store = bundle;
    return store.c_str();
}

static void vst3_scan_file(const char *path, GList **catalog)
{
    if (!ph_path_is_safe(path)) return;
    std::string err, p;
    auto mod = VST3::Hosting::Module::create(vst3_module_so(path, p), err);
    if (!mod) return;
    auto factory = mod->getFactory();
    for (auto &ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        /* key = "path\nClassName" (class resolved again at instantiate) */
        gchar *key = g_strdup_printf("%s\n%s", path, ci.name().c_str());
        const char *cat = ci.subCategoriesString().empty()
                          ? "VST3" : ci.subCategoriesString().c_str();
        *catalog = g_list_prepend(*catalog,
            ph_info_new(PH_VST3, key, ci.name().c_str(), cat));
        g_free(key);
    }
}

static void vst3_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 6) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_str_has_suffix(e, ".vst3")) {
            vst3_scan_file(full, catalog);
        } else if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            vst3_scan_dir(full, catalog, depth + 1);
        }
        g_free(full);
    }
    g_dir_close(d);
}

extern "C" void ph_vst3_scan(GList **catalog, const GList *extra)
{
    gchar *home = g_build_filename(g_get_home_dir(), ".vst3", NULL);
    vst3_scan_dir(home, catalog, 0);
    g_free(home);
    vst3_scan_dir("/usr/lib/vst3", catalog, 0);
    for (const GList *l = extra; l; l = l->next)
        vst3_scan_dir((const char *)l->data, catalog, 0);
}

/* ---- Ops ---- */

static void vst3_process(PluginInstance *pi, float *L, float *R, int n)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (n > b->max_block || !b->processor) return;

    b->data.numSamples = n;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32) {
        memcpy(b->data.inputs[0].channelBuffers32[0], L, sizeof(float) * n);
        if (b->data.inputs[0].numChannels > 1)
            memcpy(b->data.inputs[0].channelBuffers32[1], R, sizeof(float) * n);
    }
    b->data.inputParameterChanges = &b->in_params;
    b->processor->process(b->data);
    b->in_params.clearQueue();

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * n);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * n);
    }
}

static void vst3_destroy(PluginInstance *pi)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b) return;
    if (b->processor) b->processor->setProcessing(false);
    if (b->component) b->component->setActive(false);
    b->data.unprepare();
    b->processor = nullptr;
    b->controller = nullptr;
    b->component = nullptr;
    b->provider = nullptr;
    b->module = nullptr;
    delete b;
}

static guint vst3_param_count(PluginInstance *pi)
{ Vst3Backend *b = (Vst3Backend *)pi->backend;
  return b->controller ? (guint)b->param_ids.size() : 0; }

static const char *vst3_param_name(PluginInstance *pi, guint i)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    static char buf[128];
    buf[0] = 0;
    if (b->controller && i < b->param_ids.size()) {
        ParameterInfo info;
        if (b->controller->getParameterInfoByIndex((int32)i, info) == kResultOk) {
            for (int k = 0; k < 128 && info.title[k]; k++)
                buf[k] = (char)info.title[k], buf[k + 1] = 0;
        }
    }
    return buf[0] ? buf : "param";
}

static float vst3_param_get(PluginInstance *pi, guint i)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (b->controller && i < b->param_ids.size())
        return (float)b->controller->getParamNormalized(b->param_ids[i]);
    return 0.0f;
}

static void vst3_param_set(PluginInstance *pi, guint i, float v)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b->controller || i >= b->param_ids.size()) return;
    b->controller->setParamNormalized(b->param_ids[i], v);
    int32 idx = 0;
    IParamValueQueue *q = b->in_params.addParameterData(b->param_ids[i], idx);
    if (q) { int32 pidx = 0; q->addPoint(0, v, pidx); }
}

static void vst3_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{ (void)pi; (void)i; if (mn) *mn = 0.0f; if (mx) *mx = 1.0f; } /* normalised */

static const PhOps vst3_ops = {
    vst3_process, vst3_destroy, NULL,
    vst3_param_count, vst3_param_name, vst3_param_get, vst3_param_set,
    vst3_param_range
};

/* ---- Instantiate ---- */

extern "C" PluginInstance *ph_vst3_instantiate(const PluginInfo *info,
                                               double sr, int max_block)
{
    gchar **parts = g_strsplit(info->key, "\n", 2);
    if (!parts[0] || !parts[1]) { g_strfreev(parts); return NULL; }
    std::string path = parts[0];
    std::string want = parts[1];
    g_strfreev(parts);
    if (!ph_path_is_safe(path.c_str())) return NULL;

    std::string err;
    auto module = VST3::Hosting::Module::create(path, err);
    if (!module) return NULL;
    auto factory = module->getFactory();

    /* Find the requested class by name. */
    const VST3::Hosting::ClassInfo *found = nullptr;
    static thread_local std::vector<VST3::Hosting::ClassInfo> infos;
    infos = factory.classInfos();
    for (auto &ci : infos) {
        if (ci.category() == kVstAudioEffectClass && ci.name() == want) {
            found = &ci; break;
        }
    }
    if (!found) return NULL;

    Vst3Backend *b = new Vst3Backend();
    b->module    = module;
    b->max_block = max_block;
    b->provider  = owned(new PlugProvider(factory, *found, true));
    if (!b->provider->initialize()) { delete b; return NULL; }
    b->component  = b->provider->getComponentPtr();
    b->controller = b->provider->getControllerPtr();
    if (!b->component) { delete b; return NULL; }
    b->processor  = FUnknownPtr<IAudioProcessor>(b->component);
    if (!b->processor) { delete b; return NULL; }

    /* Stereo in/out arrangement. */
    SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
    b->processor->setBusArrangements(&in, 1, &out, 1);

    ProcessSetup setup;
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = max_block;
    setup.sampleRate         = sr;
    b->processor->setupProcessing(setup);

    b->component->setActive(true);
    b->processor->setProcessing(true);

    /* Buffer management handled by HostProcessData. */
    b->data.prepare(*b->component, max_block, kSample32);
    memset(&b->ctx, 0, sizeof(b->ctx));
    b->ctx.sampleRate = sr;
    b->data.processContext = &b->ctx;
    b->data.processMode = kRealtime;
    b->data.symbolicSampleSize = kSample32;

    if (b->controller) {
        int32 pc = b->controller->getParameterCount();
        for (int32 i = 0; i < pc; i++) {
            ParameterInfo pinf;
            if (b->controller->getParameterInfoByIndex(i, pinf) == kResultOk)
                b->param_ids.push_back(pinf.id);
        }
    }

    PluginInstance *pi = ph_instance_alloc(PH_VST3, info->name, sr, max_block);
    pi->ops = &vst3_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_VST3 */
