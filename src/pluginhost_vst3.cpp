#include <config.h>

#ifdef HAVE_VST3

#include <string>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknownimpl.h"

#include "inamfileloader.h"

#include "pluginhost_internal.h"

/* X11 + raw-fd watches come LAST: <gdk/gdkx.h> pulls in Xlib, whose macros
 * (None, Bool, Status, …) collide with C++ identifiers. All VST3/SDK headers are
 * included above, so those macros never reach SDK code. */
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

/* inamfileloader.h only DECLAREs the interface IID; it must be DEFINEd in
 * exactly one translation unit, and this is the only one that queries it. */
DEF_CLASS_IID(NAMku::INamFileLoader)

/* Diagnostics (JACKDAW_DIAG): count IHostApplication::createInstance calls a
 * plug-in makes while the JACK RT thread is inside the process callback. Those
 * are heap allocations (new HostMessage / HostAttributeList) on the RT thread —
 * the classic VST3 metering-message path that breaks the no-RT-malloc rule. */
static volatile guint64 g_vst3_rt_alloc_calls = 0;
extern "C" guint64 ph_vst3_rt_alloc_count(void) { return g_vst3_rt_alloc_calls; }

class CountingHostApp : public HostApplication {
public:
    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void **obj) override
    {
        if (ph_rt_active()) g_vst3_rt_alloc_calls++;   /* RT thread = single writer */
        return HostApplication::createInstance(cid, _iid, obj);
    }
};

struct Vst3Backend;   /* fwd: the component handler holds a back-pointer */

/* Host IComponentHandler — the plug-in's editor calls performEdit() when the
 * user moves a control; we mirror it to the controller and queue it for the RT
 * process() (same path the generic panel uses via vst3_param_set). Without this
 * a native editor's knobs would not affect the audio. */
class HostComponentHandler
    : public U::ImplementsNonDestroyable<U::Directly<IComponentHandler>> {
public:
    Vst3Backend *b = nullptr;
    tresult PLUGIN_API beginEdit  (ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;   /* below */
    tresult PLUGIN_API endEdit    (ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

/* Host IPlugFrame + Linux::IRunLoop in one object (per editor). On Linux the
 * plug-in's X11 UI has no global event loop, so it registers its X-connection fd
 * and timers with the IRunLoop the host exposes through the frame; we back those
 * with GLib sources so they run inside JackDAW's existing GTK main loop (rather
 * than the SDK editorhost's standalone select() loop). resizeView lets the
 * plug-in drive its own size. (SDK refs: pluginterfaces/gui/iplugview.h,
 * editorhost/.../linux/irunloopimpl.h.) */
class HostPlugFrame
    : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame, Linux::IRunLoop>> {
public:
    GtkWidget *widget = nullptr;

    struct FdWatch { guint src; Linux::IEventHandler *h; int fd; };
    struct TimerWatch { guint src; Linux::ITimerHandler *h; };
    std::vector<FdWatch>    fds;
    std::vector<TimerWatch> timers;

    /* IPlugFrame */
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *r) override
    {
        if (widget && r) {
            gtk_widget_set_size_request(widget, r->getWidth(), r->getHeight());
            if (view) view->onSize(r);
        }
        return kResultOk;
    }

    /* Linux::IRunLoop */
    static gboolean fd_cb(gint fd, GIOCondition, gpointer d)
    { ((Linux::IEventHandler *)d)->onFDIsSet(fd); return G_SOURCE_CONTINUE; }
    static gboolean timer_cb(gpointer d)
    { ((Linux::ITimerHandler *)d)->onTimer(); return G_SOURCE_CONTINUE; }

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *h,
                                            Linux::FileDescriptor fd) override
    {
        if (!h) return kInvalidArgument;
        guint s = g_unix_fd_add(fd,
            (GIOCondition)(G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_ERR), fd_cb, h);
        fds.push_back({ s, h, fd });
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *h) override
    {
        if (!h) return kInvalidArgument;
        for (auto it = fds.begin(); it != fds.end(); ++it)
            if (it->h == h) { g_source_remove(it->src); fds.erase(it); return kResultTrue; }
        return kResultFalse;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler *h,
                                     Linux::TimerInterval ms) override
    {
        if (!h || ms == 0) return kInvalidArgument;
        guint s = g_timeout_add((guint)ms, timer_cb, h);
        timers.push_back({ s, h });
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *h) override
    {
        if (!h) return kInvalidArgument;
        for (auto it = timers.begin(); it != timers.end(); ++it)
            if (it->h == h) { g_source_remove(it->src); timers.erase(it); return kResultTrue; }
        return kResultFalse;
    }

    void remove_all_sources()
    {
        for (auto &f : fds)    g_source_remove(f.src);
        for (auto &t : timers) g_source_remove(t.src);
        fds.clear(); timers.clear();
    }
};

struct Vst3Editor {
    IPtr<IPlugView> view;
    HostPlugFrame  *frame   = nullptr;
    GtkWidget      *widget  = nullptr;   /* GtkDrawingArea: the X11 embed parent */
    gulong          map_id = 0, unrealize_id = 0, alloc_id = 0;
    guint           view_retry_id = 0;   /* poll until the X window is viewable */
    bool            attached = false;
    int             last_w = 0, last_h = 0;   /* last size pushed to view->onSize */
};

struct Vst3Backend {
    VST3::Hosting::Module::Ptr     module;
    IPtr<PlugProvider>             provider;
    IPtr<IComponent>               component;
    IPtr<IEditController>          controller;
    IPtr<IAudioProcessor>          processor;
    HostProcessData                data;
    ProcessContext                 ctx;
    ParameterChanges               in_params;   /* RT-thread-only */
    /* Lock-free UI -> RT parameter hand-off.
     *
     * in_params is the ParameterChanges object process() iterates on the audio
     * thread. The generic panel and the plug-in's own editor used to call
     * addParameterData()/addPoint() on it directly from the UI thread, which can
     * grow the queue list while the RT thread is reading it. Writes now go into
     * this SDK ring buffer and are drained into in_params on the RT thread. */
    ParameterChangeTransfer        param_xfer{2048};
    EventList                      in_events{256};   /* MIDI for instruments */
    int                            max_block = 0;
    std::vector<ParamID>           param_ids;
    HostComponentHandler           handler;            /* set on the controller */
    Vst3Editor                    *editor = nullptr;   /* live native editor, if any */
    /* Optional: the plug-in's file-loading interface (NAMku and friends). VST3
     * parameters are normalised floats, so a model/IR PATH cannot travel as one;
     * plug-ins that need it expose this on the controller. NULL when absent. */
    NAMku::INamFileLoader         *file_loader = nullptr;
};

tresult PLUGIN_API HostComponentHandler::performEdit(ParamID id, ParamValue value)
{
    if (!b) return kResultOk;
    if (b->controller) b->controller->setParamNormalized(id, value);
    /* Called from the plug-in's editor thread — queue rather than touching the
     * ParameterChanges the RT thread is reading. */
    b->param_xfer.addChange(id, value, 0);
    return kResultOk;
}

/* ---- Scan ----
 * The main process only ENUMERATES .vst3 files and routes each through
 * ph_scan_cached() (pluginhost.c), which describes it out-of-process via
 * `jackdaw --scan-plugin VST3 <path>` and caches the result. Loading VST3 code
 * in-process would drag Wine/yabridge (and its fontconfig) into the main process
 * and crash native plugin UIs, so it only ever happens in the throwaway scanner
 * (ph_vst3_describe, below) or at instantiate time. */

/* Load+describe one .vst3 — runs only in the out-of-process scanner. */
extern "C" void ph_vst3_describe(const char *path, GList **catalog)
{
    if (!ph_path_is_safe(path)) return;
    std::string err;
    auto mod = VST3::Hosting::Module::create(path, err);
    if (!mod) return;
    auto factory = mod->getFactory();
    for (auto &ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        /* name()/subCategoriesString() return std::string BY VALUE — keep them in
         * locals so their c_str() stays valid across the ph_info_new call. */
        std::string name = ci.name();
        std::string subcat = ci.subCategoriesString();
        gchar *key = g_strdup_printf("%s\n%s", path, name.c_str());
        *catalog = g_list_prepend(*catalog,
            ph_info_new(PH_VST3, key, name.c_str(),
                        subcat.empty() ? "VST3" : subcat.c_str()));
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
        if (g_str_has_suffix(e, ".vst3"))
            ph_scan_cached(PH_VST3, full, catalog);
        else if (g_file_test(full, G_FILE_TEST_IS_DIR))
            vst3_scan_dir(full, catalog, depth + 1);
        g_free(full);
    }
    g_dir_close(d);
}

extern "C" void ph_vst3_scan(GList **catalog, const GList *extra)
{
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
    /* Zero the output buffers first: if the plugin fails to process (e.g. a
     * bridge that isn't running) we get silence rather than stale garbage. */
    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * n);
    }
    /* Drain the UI's queued parameter changes into in_params here, on the RT
     * thread, so nothing else ever mutates the object process() reads. */
    b->param_xfer.transferChangesTo(b->in_params);
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

/* Deliver this block's MIDI as VST3 note events then render (instruments).
 * Audio inputs are fed silence; the processContext carries transport/tempo. */
static void vst3_process_midi(PluginInstance *pi, const PhMidiEvent *ev,
                              int n_ev, float *L, float *R, int n)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (n > b->max_block || !b->processor) return;

    b->in_events.clear();
    for (int i = 0; i < n_ev; i++) {
        const guint8 *m = ev[i].data;
        uint8 status = m[0] & 0xF0, ch = m[0] & 0x0F;
        Event e{};
        e.busIndex     = 0;
        e.sampleOffset = (int32)ev[i].time;
        e.flags        = Event::kIsLive;
        if (status == 0x90 && m[2] > 0) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel  = ch;  e.noteOn.pitch = m[1];
            e.noteOn.velocity = m[2] / 127.0f;  e.noteOn.noteId = -1;
        } else if (status == 0x80 || (status == 0x90 && m[2] == 0)) {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel  = ch;  e.noteOff.pitch = m[1];
            e.noteOff.velocity = (status == 0x80 ? m[2] / 127.0f : 0.0f);
            e.noteOff.noteId   = -1;
        } else {
            continue;   /* CC/other: not handled in v1 */
        }
        b->in_events.addEvent(e);
    }
    b->data.inputEvents = &b->in_events;

    double bpm, sr; gint64 frame; gboolean playing;
    ph_get_transport(&bpm, &sr, &frame, &playing);
    b->ctx.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
    if (playing) b->ctx.state |= ProcessContext::kPlaying;
    b->ctx.sampleRate         = sr;
    b->ctx.tempo              = bpm;
    b->ctx.projectTimeSamples = (TSamples)frame;
    double fpb = (bpm > 0.0) ? sr * 60.0 / bpm : 0.0;
    b->ctx.projectTimeMusic   = (fpb > 0.0) ? (double)frame / fpb : 0.0;

    b->data.numSamples = n;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32)   /* silence in */
        for (int ch = 0; ch < b->data.inputs[0].numChannels; ch++)
            memset(b->data.inputs[0].channelBuffers32[ch], 0, sizeof(float) * n);
    if (b->data.outputs && b->data.outputs[0].channelBuffers32)
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * n);

    /* Drain the UI's queued parameter changes into in_params here, on the RT
     * thread, so nothing else ever mutates the object process() reads. */
    b->param_xfer.transferChangesTo(b->in_params);
    b->data.inputParameterChanges = &b->in_params;
    b->processor->process(b->data);
    b->in_params.clearQueue();
    b->data.inputEvents = nullptr;        /* reset for the effect (audio) path */

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * n);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * n);
    }
}

static void vst3_destroy_gui(PluginInstance *pi);   /* fwd */

static void vst3_reset(PluginInstance *pi)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b) return;
    /* Inactive→active cycle resets the processor's internal state. */
    if (b->processor) b->processor->setProcessing(false);
    if (b->component) b->component->setActive(false);
    if (b->component) b->component->setActive(true);
    if (b->processor) b->processor->setProcessing(true);
}

static void vst3_destroy(PluginInstance *pi)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b) return;
    if (b->editor) vst3_destroy_gui(pi);   /* defensive: normally already gone */
    if (b->controller) b->controller->setComponentHandler(nullptr);
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
        if (b->controller->getParameterInfo((int32)i, info) == kResultOk) {
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
    /* Main thread: queue for the RT thread to drain (see param_xfer). */
    b->param_xfer.addChange(b->param_ids[i], v, 0);
}

static void vst3_param_range(PluginInstance *pi, guint i, float *mn, float *mx)
{ (void)pi; (void)i; if (mn) *mn = 0.0f; if (mx) *mx = 1.0f; } /* normalised */

/* VST3 parameters are all normalised 0..1, so the slider position on its own
 * tells the user nothing. Ask the controller to render the real value. */
static gboolean vst3_param_display(PluginInstance *pi, guint i,
                                   char *buf, gsize len)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b->controller || i >= b->param_ids.size()) return FALSE;

    ParamValue v = b->controller->getParamNormalized(b->param_ids[i]);
    String128 s{};
    if (b->controller->getParamStringByValue(b->param_ids[i], v, s) != kResultOk)
        return FALSE;

    /* String128 is UTF-16; convert rather than truncating to Latin-1. */
    gchar *utf8 = g_utf16_to_utf8((const gunichar2 *)s, -1, NULL, NULL, NULL);
    if (!utf8) return FALSE;
    g_strlcpy(buf, utf8, len);
    g_free(utf8);
    return TRUE;
}

static gboolean vst3_param_is_stepped(PluginInstance *pi, guint i, int *n_steps)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (n_steps) *n_steps = 0;
    if (!b->controller || i >= b->param_ids.size()) return FALSE;

    ParameterInfo info{};
    if (b->controller->getParameterInfo((int32)i, info) != kResultOk) return FALSE;
    if (info.stepCount <= 0) return FALSE;
    if (n_steps) *n_steps = (int)info.stepCount + 1;   /* stepCount is intervals */
    return TRUE;
}

/* ---- Opaque state (project save/reload) ----
 * The normalised parameter list does NOT capture a VST3 plug-in's full state:
 * loaded sample/IR/NAM file paths, internal modes and any controller-private
 * data live in the IComponent/IEditController state streams, not in parameters.
 * Save both; the container is two little-endian length-prefixed blobs:
 *   [int32 comp_len][comp bytes][int32 ctrl_len][ctrl bytes]. */
static gboolean vst3_state_save(PluginInstance *pi, void **out, gsize *out_len)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b->component) return FALSE;

    MemoryStream comp, ctrl;
    if (b->component->getState(&comp) != kResultOk) return FALSE;
    gboolean have_ctrl =
        (b->controller && b->controller->getState(&ctrl) == kResultOk);

    gint32 clen = (gint32)comp.getSize();
    gint32 elen = have_ctrl ? (gint32)ctrl.getSize() : 0;
    if (clen < 0 || elen < 0) return FALSE;

    gsize total = 8 + (gsize)clen + (gsize)elen;
    guint8 *buf = (guint8 *)g_malloc(total);
    gint32 cle = GINT32_TO_LE(clen), ele = GINT32_TO_LE(elen);
    memcpy(buf + 0, &cle, 4);
    memcpy(buf + 4, &ele, 4);
    if (clen > 0) memcpy(buf + 8, comp.getData(), (size_t)clen);
    if (elen > 0) memcpy(buf + 8 + clen, ctrl.getData(), (size_t)elen);

    *out = buf;
    *out_len = total;
    return TRUE;
}

static gboolean vst3_state_load(PluginInstance *pi, const void *data, gsize len)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b->component || len < 8) return FALSE;

    const guint8 *p = (const guint8 *)data;
    gint32 clen, elen;
    memcpy(&clen, p + 0, 4); clen = GINT32_FROM_LE(clen);
    memcpy(&elen, p + 4, 4); elen = GINT32_FROM_LE(elen);
    if (clen < 0 || elen < 0 || (gsize)8 + (gsize)clen + (gsize)elen > len)
        return FALSE;

    /* Restore the processor (component) state, then hand the SAME stream to the
     * edit controller via setComponentState so a (re)opened editor reflects the
     * restored values — this is the step missing before, which is why VST3 GUIs
     * came back showing defaults. MemoryStream(mem,size) wraps our buffer
     * read-only without taking ownership. */
    if (clen > 0) {
        MemoryStream cs((void *)(p + 8), clen);
        b->component->setState(&cs);
        cs.seek(0, IBStream::kIBSeekSet, nullptr);
        if (b->controller) b->controller->setComponentState(&cs);
    }
    /* Controller-private state (editor zoom, etc.). */
    if (elen > 0 && b->controller) {
        MemoryStream es((void *)(p + 8 + clen), elen);
        b->controller->setState(&es);
    }

    /* Mirror the restored normalised values into the RT parameter queue so the
     * live processor matches the controller on the very next block (plug-ins
     * that drive DSP from parameter changes, not from setState alone). */
    if (b->controller) {
        for (size_t i = 0; i < b->param_ids.size(); i++) {
            ParamValue v = b->controller->getParamNormalized(b->param_ids[i]);
            b->param_xfer.addChange(b->param_ids[i], v, 0);
        }
    }
    return TRUE;
}

/* ---- Native editor (IPlugView embedded in a GTK X11 window) ---- */

/* Tell the plug-in view its real on-screen size. The host MUST do this whenever
 * the embed window's size changes — GTK only changes OUR X window; the plug-in's
 * child view (and, for GPU editors, its swap chain) won't follow unless we call
 * onSize(). Skipping this is why a DXVK/Vulkan editor painted its restored state
 * once and then froze: its render surface never matched the window. We let the
 * plug-in clamp the rect to its own constraints first. */
static void vst3_push_size(Vst3Editor *ed, int w, int h)
{
    if (!ed->view || !ed->attached || w <= 0 || h <= 0) return;
    ViewRect r; r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    ed->view->checkSizeConstraint(&r);
    if (r.getWidth() <= 0 || r.getHeight() <= 0) return;
    if (r.getWidth() == ed->last_w && r.getHeight() == ed->last_h) return;
    ed->last_w = r.getWidth(); ed->last_h = r.getHeight();
    ed->view->onSize(&r);
}

/* Attach the IPlugView, but only once the GtkDrawingArea is genuinely viewable:
 * MAPPED (on-screen) AND given a real, non-degenerate allocation. X11 XEmbed
 * reparenting needs the parent window mapped; and a Wine-bridged view attached
 * against a still-1x1 parent (a freshly built FX window can fire "map" before
 * layout settles) comes up clipped and never receives the XEmbed activation —
 * it paints once and then sits frozen with no cursor. Gating on a real allocation
 * makes the attach independent of map/allocation timing (it was that timing the
 * global undo/redo + drag-section work perturbed). The `attached` guard keeps it
 * one-shot, so the repeated "map" of a GtkStack tab switch is a no-op. */
static gboolean vst3_view_retry_cb(gpointer data);

static void vst3_try_attach(Vst3Editor *ed, GtkWidget *w)
{
    if (!ed->view || ed->attached) return;
    if (!gtk_widget_get_mapped(w)) return;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    if (a.width <= 1 || a.height <= 1) return;   /* layout not settled yet */
    GdkWindow *gw = gtk_widget_get_window(w);
    if (!gw) return;
    /* XEmbed only activates once the whole ancestor chain is mapped (viewable);
     * no GTK signal reports an ancestor becoming viewable, so poll until it does.
     * Attaching into a non-viewable window leaves a bridged child frozen with no
     * cursor — the after-load regression. */
    if (!gdk_window_is_viewable(gw)) {
        if (!ed->view_retry_id)
            ed->view_retry_id = g_timeout_add(16, vst3_view_retry_cb, ed);
        return;
    }
    Window xid = gdk_x11_window_get_xid(gw);

    ed->view->setFrame(ed->frame);
    if (ed->view->attached((void *)(uintptr_t)xid,
                           kPlatformTypeX11EmbedWindowID) == kResultOk) {
        ed->attached = true;
        ViewRect r;
        if (ed->view->getSize(&r) == kResultOk && r.getWidth() > 0)
            gtk_widget_set_size_request(w, r.getWidth(), r.getHeight());
        /* Sync the view to the allocation we actually have, so the editor's
         * render surface is correct from the first frame. */
        vst3_push_size(ed, a.width, a.height);
    }
}

/* GTK gave the embed widget an allocation. Attach now if "map" fired too early
 * (degenerate 1x1 size); once attached, forward the new size to the view. */
static void vst3_on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    Vst3Editor *ed = (Vst3Editor *)data;
    if (!ed->attached) { vst3_try_attach(ed, w); return; }
    if (a) vst3_push_size(ed, a->width, a->height);
}

/* Poll for the embed window to become viewable (ancestors mapped), then attach. */
static gboolean vst3_view_retry_cb(gpointer data)
{
    Vst3Editor *ed = (Vst3Editor *)data;
    if (ed->attached) { ed->view_retry_id = 0; return G_SOURCE_REMOVE; }
    vst3_try_attach(ed, ed->widget);
    if (ed->attached) { ed->view_retry_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;          /* keep waiting for viewability */
}

static void vst3_on_map(GtkWidget *w, gpointer data)
{
    vst3_try_attach((Vst3Editor *)data, w);
}

/* The window is going away (e.g. switching tabs never unrealizes a GtkStack
 * child, so this is effectively the close path). Detach the plug-in while its
 * parent X window still exists. */
static void vst3_on_unrealize(GtkWidget *w, gpointer data)
{
    (void)w;
    Vst3Editor *ed = (Vst3Editor *)data;
    if (ed->view_retry_id) { g_source_remove(ed->view_retry_id); ed->view_retry_id = 0; }
    if (ed->view && ed->attached) { ed->view->removed(); ed->attached = false; }
}

static GtkWidget *vst3_make_gui(PluginInstance *pi)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b->controller) { g_printerr("[vst3 ui] no controller\n"); return NULL; }

    IPlugView *view = b->controller->createView(ViewType::kEditor);
    if (!view) { g_printerr("[vst3 ui] createView(editor) returned NULL\n"); return NULL; }
    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        g_printerr("[vst3 ui] view does not support X11EmbedWindowID\n");
        view->release();
        return NULL;        /* host falls back to the generic parameter panel */
    }

    Vst3Editor *ed = new Vst3Editor();
    ed->view   = owned(view);
    ed->frame  = new HostPlugFrame();
    ed->widget = gtk_drawing_area_new();   /* has its own native X11 window */
    ed->frame->widget = ed->widget;
    gtk_widget_set_hexpand(ed->widget, TRUE);
    gtk_widget_set_vexpand(ed->widget, TRUE);

    ViewRect r;                            /* seed a size so the FX window fits */
    if (view->getSize(&r) == kResultOk && r.getWidth() > 0)
        gtk_widget_set_size_request(ed->widget, r.getWidth(), r.getHeight());

    ed->map_id       = g_signal_connect(ed->widget, "map",
                                        G_CALLBACK(vst3_on_map), ed);
    ed->unrealize_id = g_signal_connect(ed->widget, "unrealize",
                                        G_CALLBACK(vst3_on_unrealize), ed);
    ed->alloc_id     = g_signal_connect(ed->widget, "size-allocate",
                                        G_CALLBACK(vst3_on_size_allocate), ed);
    b->editor = ed;
    return ed->widget;
}

static void vst3_destroy_gui(PluginInstance *pi)
{
    Vst3Backend *b = (Vst3Backend *)pi->backend;
    if (!b || !b->editor) return;
    Vst3Editor *ed = b->editor;

    if (ed->view_retry_id) { g_source_remove(ed->view_retry_id); ed->view_retry_id = 0; }

    /* Stop our handlers firing during the gtk_widget_destroy the host does next. */
    if (ed->widget) {
        if (ed->map_id)       g_signal_handler_disconnect(ed->widget, ed->map_id);
        if (ed->unrealize_id) g_signal_handler_disconnect(ed->widget, ed->unrealize_id);
        if (ed->alloc_id)     g_signal_handler_disconnect(ed->widget, ed->alloc_id);
    }
    if (ed->view) {
        if (ed->attached) { ed->view->removed(); ed->attached = false; }
        ed->view->setFrame(nullptr);
        ed->view = nullptr;            /* IPtr drops the view's last ref */
    }
    if (ed->frame) {                   /* frame outlives removed() (it may unregister) */
        ed->frame->remove_all_sources();
        delete ed->frame;
        ed->frame = nullptr;
    }
    delete ed;
    b->editor = nullptr;
}

/* ---- File loading (INamFileLoader) ----
 * Exposed to the host layer as plain C so fxwindow.c can offer file pickers for
 * plug-ins whose real "parameter" is a path (a NAM model, an impulse response).
 * Returns FALSE for every plug-in that does not implement the interface. */

extern "C" gboolean ph_vst3_has_file_loader(PluginInstance *inst)
{
    if (!inst || inst->format != PH_VST3 || !inst->backend) return FALSE;
    return ((Vst3Backend *)inst->backend)->file_loader != nullptr;
}

extern "C" gboolean ph_vst3_file_get(PluginInstance *inst, int which,
                                     char *buf, int buflen)
{
    if (!buf || buflen <= 0) return FALSE;
    buf[0] = 0;
    if (!ph_vst3_has_file_loader(inst)) return FALSE;
    NAMku::INamFileLoader *fl = ((Vst3Backend *)inst->backend)->file_loader;
    tresult r = (which == PH_FILE_MODEL) ? fl->getModelFile(buf, buflen)
                                         : fl->getIrFile(buf, buflen);
    return r == kResultOk;
}

extern "C" gboolean ph_vst3_file_set(PluginInstance *inst, int which,
                                     const char *path)
{
    if (!ph_vst3_has_file_loader(inst)) return FALSE;
    NAMku::INamFileLoader *fl = ((Vst3Backend *)inst->backend)->file_loader;
    tresult r = (which == PH_FILE_MODEL) ? fl->setModelFile(path ? path : "")
                                         : fl->setIrFile(path ? path : "");
    return r == kResultOk;
}

static const PhOps vst3_ops = {
    .process      = vst3_process,
    .process_midi = vst3_process_midi,
    .destroy      = vst3_destroy,
    .make_gui     = vst3_make_gui,
    .destroy_gui  = vst3_destroy_gui,
    .param_count  = vst3_param_count,
    .param_name   = vst3_param_name,
    .param_get    = vst3_param_get,
    .param_set    = vst3_param_set,
    .param_range  = vst3_param_range,
    .param_display = vst3_param_display,
    .param_is_stepped = vst3_param_is_stepped,
    .reset        = vst3_reset,
    .state_save   = vst3_state_save,
    .state_load   = vst3_state_load,
};

/* ---- Instantiate ---- */

extern "C" PluginInstance *ph_vst3_instantiate(const PluginInfo *info,
                                               double sr, int max_block)
{
    /* Expose an IHostApplication to plug-ins via the plugin context, registered
     * once. JUCE (and other) VST3 plug-ins query the host context during
     * controller initialize() to create IMessage/IAttributeList objects; without
     * it their createView() returns NULL and we'd never get the editor. The SDK's
     * PlugProvider passes this context to component+controller initialize(). */
    static Steinberg::Vst::HostApplication *gHostApp = nullptr;
    if (!gHostApp) {
        gHostApp = new CountingHostApp();   /* counts RT-thread message allocs */
        PluginContextFactory::instance().setPluginContext(gHostApp);
    }

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

    /* Stereo in/out arrangement (best effort; if the plug-in rejects it, it keeps
     * its own default and our buffers follow its actual bus channel counts). */
    SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
    b->processor->setBusArrangements(&in, 1, &out, 1);

    ProcessSetup setup;
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = max_block;
    setup.sampleRate         = sr;
    b->processor->setupProcessing(setup);

    /* VST3 buses are INACTIVE by default — "The plug-in should only process an
     * activated bus" — so without this most plug-ins emit silence. Activate every
     * audio bus the plug-in flags kDefaultActive (its main I/O).
     * (SDK ref: pluginterfaces/vst/ivstcomponent.h, BusInfo::kDefaultActive.) */
    for (int d = 0; d < 2; d++) {
        BusDirection bd = (d == 0) ? kInput : kOutput;
        int32 nb = b->component->getBusCount(kAudio, bd);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kAudio, bd, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kAudio, bd, i, true);
        }
    }
    /* Instruments receive notes on an event INPUT bus, also inactive by default.
     * Activate every default-active event input bus so MIDI reaches the synth. */
    {
        int32 nb = b->component->getBusCount(kEvent, kInput);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kEvent, kInput, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kEvent, kInput, i, true);
        }
    }

    /* Buffer management handled by HostProcessData (prepare before activation). */
    b->data.prepare(*b->component, max_block, kSample32);
    memset(&b->ctx, 0, sizeof(b->ctx));
    b->ctx.sampleRate = sr;
    b->data.processContext = &b->ctx;
    b->data.processMode = kRealtime;
    b->data.symbolicSampleSize = kSample32;

    b->component->setActive(true);
    b->processor->setProcessing(true);

    if (b->controller) {
        /* Route editor knob edits to the DSP (needed for native-editor controls). */
        b->handler.b = b;
        b->controller->setComponentHandler(&b->handler);

        /* Optional file-loading interface. Plug-ins that don't implement it
         * simply fail the query and the host offers no file buttons. */
        void *fl = nullptr;
        if (b->controller->queryInterface(NAMku::INamFileLoader::iid, &fl) == kResultOk)
            b->file_loader = (NAMku::INamFileLoader *)fl;

        int32 pc = b->controller->getParameterCount();
        for (int32 i = 0; i < pc; i++) {
            ParameterInfo pinf;
            if (b->controller->getParameterInfo(i, pinf) == kResultOk)
                b->param_ids.push_back(pinf.id);
        }
    }

    PluginInstance *pi = ph_instance_alloc(PH_VST3, info->name, sr, max_block);
    pi->ops = &vst3_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_VST3 */
