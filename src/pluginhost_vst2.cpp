#include <config.h>

#ifdef HAVE_VST2

#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

#include <gdk/gdkx.h>          /* gdk_x11_window_get_xid for the embedded editor */

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
#define EFF_PROCESS_EVENTS  25
#define EFF_START_PROCESS   71
#define EFF_STOP_PROCESS    72
#define VST_PLUG_CATEG_SYNTH 2
#define VST_FLAGS_IS_SYNTH  (1 << 8)
#define VST_FLAGS_HAS_EDITOR 1

/* Editor opcodes (VST 2.4). The plug-in draws into an X11 window we own. */
#define EFF_EDIT_GET_RECT   13
#define EFF_EDIT_OPEN       14
#define EFF_EDIT_CLOSE      15
#define EFF_EDIT_IDLE       19

/* effEditGetRect returns a pointer to this (not in vestige's header). */
struct ERect { short top, left, bottom, right; };

typedef AEffect *(VST_CALL_CONV *Vst2EntryProc)(audioMasterCallback);

/* Block size published at instantiate (for audioMasterGetBlockSize). */
static int g_vst2_block = 1024;

/* Host callback. Instruments (e.g. Superior Drummer) query host time via
 * audioMasterGetTime and announce MIDI via canDo — answer those so they run. */
static intptr_t VST_CALL_CONV vst2_master(AEffect *e, int op, int idx,
                                          intptr_t val, void *ptr, float opt)
{
    (void)e; (void)idx; (void)val; (void)opt;
    switch (op) {
    case audioMasterVersion:       return 2400;
    case audioMasterGetTime: {
        static VstTimeInfo vti;     /* RT thread only; refilled each query */
        double bpm, sr; gint64 frame; gboolean playing;
        ph_get_transport(&bpm, &sr, &frame, &playing);
        memset(&vti, 0, sizeof vti);
        vti.sampleRate = sr;
        vti.samplePos  = (double)frame;
        vti.tempo      = bpm;
        double fpb = (bpm > 0.0) ? sr * 60.0 / bpm : 0.0;   /* frames per beat */
        vti.ppqPos   = (fpb > 0.0) ? (double)frame / fpb : 0.0;
        vti.timeSigNumerator   = 4;
        vti.timeSigDenominator = 4;
        vti.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid;
        if (playing) vti.flags |= kVstTransportPlaying;
        return (intptr_t)&vti;
    }
    case audioMasterGetSampleRate: {
        double sr; ph_get_transport(NULL, &sr, NULL, NULL);
        return (intptr_t)sr;
    }
    case audioMasterGetBlockSize:  return (intptr_t)g_vst2_block;
    case audioMasterWantMidi:      return 1;
    case audioMasterCurrentId:     return 0;
    case audioMasterGetCurrentProcessLevel: return 2;   /* realtime */
    case audioMasterCanDo:
        if (ptr && (!strcmp((const char *)ptr, "sendVstMidiEvent")    ||
                    !strcmp((const char *)ptr, "receiveVstMidiEvent") ||
                    !strcmp((const char *)ptr, "sendVstTimeInfo")))
            return 1;
        return 0;
    default: return 0;
    }
}

#define VST2_MAX_EVENTS 1024

typedef struct {
    void    *dl;
    AEffect *eff;
    /* Channel buffers sized to the plug-in's ACTUAL port counts. processReplacing
     * writes every numOutputs channel, so a fixed [2] array overruns for multi-out
     * instruments (Superior Drummer) -> the plug-in writes to a bogus pointer. */
    int      n_in, n_out;   /* eff->numInputs / numOutputs (>= the audio we use) */
    float  **in_ptrs;       /* [n_in]  channel pointers handed to the plug-in */
    float  **out_ptrs;      /* [n_out] channel pointers handed to the plug-in */
    float  **out_bufs;      /* [n_out] backing scratch the plug-in fills */
    float   *silence;       /* zeroed input (shared, read-only; max_block) */
    int      max_block;
    gboolean is_synth;

    /* Pre-allocated VST event block for MIDI delivery (no RT malloc). */
    VstMidiEvent *midi_pool;   /* VST2_MAX_EVENTS entries */
    VstEvents    *vst_events;  /* header + VST2_MAX_EVENTS event pointers */

    /* Native editor (effEditOpen into our X11 window), mirrors the VST3 path. */
    GtkWidget *gui;            /* GtkDrawingArea with its own X11 window */
    gulong     realize_id, unrealize_id;
    guint      idle_id;        /* effEditIdle pump */
    gboolean   editor_open;
} Vst2Backend;

static void vst2_close_editor(Vst2Backend *b);   /* fwd (used by vst2_destroy) */

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

/* Load+describe one VST2 .so — runs only in the out-of-process scanner. */
extern "C" void ph_vst2_describe(const char *path, GList **catalog)
{
    void *dl = NULL;
    AEffect *eff = vst2_load(path, &dl);
    if (!eff) return;
    char nm[128] = {0};
    eff->dispatcher(eff, EFF_GET_EFFECT_NAME, 0, 0, nm, 0.0f);
    if (!nm[0]) { gchar *b = g_path_get_basename(path);
                  g_strlcpy(nm, b, sizeof(nm)); g_free(b); }
    /* An instrument advertises kPlugCategSynth or the effFlagsIsSynth bit;
     * "Instrument" in the category drives ph_info's is_instrument detection. */
    gboolean synth = (eff->flags & VST_FLAGS_IS_SYNTH) ||
        eff->dispatcher(eff, EFF_GET_PLUG_CATEGORY, 0, 0, NULL, 0.0f)
            == VST_PLUG_CATEG_SYNTH;
    *catalog = g_list_prepend(*catalog,
        ph_info_new(PH_VST2, path, nm, synth ? "Instrument|VST2" : "VST2"));
    if (dl) dlclose(dl);
}

static void vst2_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 6) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR))
            vst2_scan_dir(full, catalog, depth + 1);
        else if (g_str_has_suffix(e, ".so") || g_str_has_suffix(e, ".dll"))
            ph_scan_cached(PH_VST2, full, catalog);
        g_free(full);
    }
    g_dir_close(d);
}

void ph_vst2_scan(GList **catalog, const GList *extra)
{
    for (const GList *l = extra; l; l = l->next)
        vst2_scan_dir((const char *)l->data, catalog, 0);
    const char *envp = g_getenv("VST_PATH");
    if (envp) {
        gchar **parts = g_strsplit(envp, ":", -1);
        for (gchar **p = parts; *p; p++) if (**p) vst2_scan_dir(*p, catalog, 0);
        g_strfreev(parts);
    }
}

/* ---- Ops ---- */

/* Run processReplacing once. For instruments `inL/inR` is silence; outputs are
 * captured to scratch then copied to L/R. */
static void vst2_run(Vst2Backend *b, float *inL, float *inR,
                     float *L, float *R, int n)
{
    AEffect *e = b->eff;
    if (!e->processReplacing) return;
    /* Bind EVERY channel the plug-in expects: ch0=inL, ch1=inR, the rest silence;
     * each output channel gets its own scratch (the plug-in writes all n_out). */
    for (int i = 0; i < b->n_in; i++)
        b->in_ptrs[i] = (i == 0) ? inL : (i == 1) ? inR : b->silence;
    for (int i = 0; i < b->n_out; i++)
        b->out_ptrs[i] = b->out_bufs[i];
    e->processReplacing(e, b->in_ptrs, b->out_ptrs, n);
    if (b->n_out > 0) {
        memcpy(L, b->out_bufs[0], (size_t)n * sizeof(float));
        memcpy(R, b->out_bufs[b->n_out > 1 ? 1 : 0], (size_t)n * sizeof(float));
    }
}

static void vst2_process(PluginInstance *pi, float *L, float *R, int n)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (n > b->max_block) n = b->max_block;
    vst2_run(b, L, R, L, R, n);          /* effect: in-place audio */
}

/* Deliver this block's MIDI then render. Used for instrument tracks. */
static void vst2_process_midi(PluginInstance *pi, const PhMidiEvent *ev,
                              int n_ev, float *L, float *R, int n)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (n > b->max_block) n = b->max_block;

    if (n_ev > 0 && b->vst_events) {
        if (n_ev > VST2_MAX_EVENTS) n_ev = VST2_MAX_EVENTS;
        for (int i = 0; i < n_ev; i++) {
            VstMidiEvent *me = &b->midi_pool[i];
            memset(me, 0, sizeof *me);
            me->type        = kVstMidiType;
            me->byteSize    = sizeof(VstMidiEvent);
            me->deltaFrames = (int)ev[i].time;
            for (int k = 0; k < 3 && k < ev[i].size; k++)
                me->midiData[k] = (char)ev[i].data[k];
            b->vst_events->events[i] = (VstEvent *)me;
        }
        b->vst_events->numEvents = n_ev;
        b->eff->dispatcher(b->eff, EFF_PROCESS_EVENTS, 0, 0, b->vst_events, 0.0f);
    }
    /* Instruments take no audio input — feed silence; effects-with-MIDI get the
     * incoming signal. */
    if (b->is_synth) vst2_run(b, b->silence, b->silence, L, R, n);
    else             vst2_run(b, L, R, L, R, n);
}

static void vst2_reset(PluginInstance *pi)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (!b || !b->eff) return;
    /* Toggle processing + power off/on: clears tails and held notes. */
    b->eff->dispatcher(b->eff, EFF_STOP_PROCESS,  0, 0, NULL, 0.0f);
    b->eff->dispatcher(b->eff, EFF_MAINS_CHANGED, 0, 0, NULL, 0.0f);
    b->eff->dispatcher(b->eff, EFF_MAINS_CHANGED, 0, 1, NULL, 0.0f);
    b->eff->dispatcher(b->eff, EFF_START_PROCESS, 0, 0, NULL, 0.0f);
}

static void vst2_destroy(PluginInstance *pi)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (!b) return;
    vst2_close_editor(b);            /* defensive: normally already closed */
    if (b->eff) {
        b->eff->dispatcher(b->eff, EFF_MAINS_CHANGED, 0, 0, NULL, 0.0f);
        b->eff->dispatcher(b->eff, EFF_CLOSE, 0, 0, NULL, 0.0f);
    }
    if (b->dl) dlclose(b->dl);
    for (int i = 0; i < b->n_out; i++) g_free(b->out_bufs[i]);
    g_free(b->out_bufs); g_free(b->out_ptrs); g_free(b->in_ptrs);
    g_free(b->silence);
    g_free(b->midi_pool); g_free(b->vst_events);
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

/* ---- Native editor (effEditOpen into a GTK X11 window) ---- */

/* Apply the plug-in's reported editor size to our drawing area. */
static void vst2_apply_rect(Vst2Backend *b)
{
    ERect *r = NULL;
    b->eff->dispatcher(b->eff, EFF_EDIT_GET_RECT, 0, 0, &r, 0.0f);
    if (r && r->right > r->left && r->bottom > r->top)
        gtk_widget_set_size_request(b->gui, r->right - r->left,
                                            r->bottom - r->top);
}

/* Periodic idle so the editor repaints (VST 2.4 hosts must pump effEditIdle). */
static gboolean vst2_idle_cb(gpointer data)
{
    Vst2Backend *b = (Vst2Backend *)data;
    if (b->editor_open && b->eff)
        b->eff->dispatcher(b->eff, EFF_EDIT_IDLE, 0, 0, NULL, 0.0f);
    return G_SOURCE_CONTINUE;
}

/* Attach once the drawing area has a real X11 window. The plug-in (via the
 * yabridge XEmbed bridge for Windows VSTs) reparents its UI into our window. */
static void vst2_on_realize(GtkWidget *w, gpointer data)
{
    Vst2Backend *b = (Vst2Backend *)data;
    if (b->editor_open) return;
    GdkWindow *gw = gtk_widget_get_window(w);
    if (!gw) return;
    Window xid = gdk_x11_window_get_xid(gw);
    if (b->eff->dispatcher(b->eff, EFF_EDIT_OPEN, 0, 0,
                           (void *)(uintptr_t)xid, 0.0f)) {
        /* some plug-ins return 0 on success; treat reaching here as opened */
    }
    b->editor_open = TRUE;
    vst2_apply_rect(b);
    if (!b->idle_id) b->idle_id = g_timeout_add(40, vst2_idle_cb, b);
}

static void vst2_close_editor(Vst2Backend *b)
{
    if (b->idle_id) { g_source_remove(b->idle_id); b->idle_id = 0; }
    if (b->editor_open && b->eff)
        b->eff->dispatcher(b->eff, EFF_EDIT_CLOSE, 0, 0, NULL, 0.0f);
    b->editor_open = FALSE;
}

/* Drawing area going away: close the editor while its X parent still exists. */
static void vst2_on_unrealize(GtkWidget *w, gpointer data)
{
    (void)w;
    vst2_close_editor((Vst2Backend *)data);
}

static GtkWidget *vst2_make_gui(PluginInstance *pi)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (!(b->eff->flags & VST_FLAGS_HAS_EDITOR))
        return NULL;                 /* no editor -> host uses the generic panel */

    b->gui = gtk_drawing_area_new();  /* has its own native X11 window */
    gtk_widget_set_hexpand(b->gui, TRUE);
    gtk_widget_set_vexpand(b->gui, TRUE);
    vst2_apply_rect(b);               /* seed a size so the FX window fits */

    b->realize_id   = g_signal_connect(b->gui, "realize",
                                       G_CALLBACK(vst2_on_realize), b);
    b->unrealize_id = g_signal_connect(b->gui, "unrealize",
                                       G_CALLBACK(vst2_on_unrealize), b);
    return b->gui;
}

static void vst2_destroy_gui(PluginInstance *pi)
{
    Vst2Backend *b = (Vst2Backend *)pi->backend;
    if (!b || !b->gui) return;
    if (b->realize_id)   g_signal_handler_disconnect(b->gui, b->realize_id);
    if (b->unrealize_id) g_signal_handler_disconnect(b->gui, b->unrealize_id);
    b->realize_id = b->unrealize_id = 0;
    vst2_close_editor(b);
    b->gui = NULL;                    /* the host destroys the widget itself */
}

static const PhOps vst2_ops = {
    vst2_process, vst2_process_midi, vst2_destroy,
    vst2_make_gui, vst2_destroy_gui,
    vst2_param_count, vst2_param_name, vst2_param_get, vst2_param_set,
    vst2_param_range, vst2_reset
};

/* ---- Instantiate ---- */

PluginInstance *ph_vst2_instantiate(const PluginInfo *info, double sr, int max_block)
{
    void *dl = NULL;
    AEffect *eff = vst2_load(info->key, &dl);
    if (!eff) return NULL;

    Vst2Backend *b = g_new0(Vst2Backend, 1);
    b->dl = dl; b->eff = eff; b->max_block = max_block;
    /* Size channel arrays to the plug-in's real port counts (clamped to >=1 so the
     * pointer arrays handed to processReplacing are never NULL). */
    b->n_in  = eff->numInputs  > 0 ? eff->numInputs  : 0;
    b->n_out = eff->numOutputs > 0 ? eff->numOutputs : 0;
    b->in_ptrs  = g_new0(float *, b->n_in  > 0 ? b->n_in  : 1);
    b->out_ptrs = g_new0(float *, b->n_out > 0 ? b->n_out : 1);
    b->out_bufs = g_new0(float *, b->n_out > 0 ? b->n_out : 1);
    for (int i = 0; i < b->n_out; i++) b->out_bufs[i] = g_new0(float, max_block);
    b->silence = g_new0(float, max_block);
    b->is_synth = (eff->flags & VST_FLAGS_IS_SYNTH) ||
        eff->dispatcher(eff, EFF_GET_PLUG_CATEGORY, 0, 0, NULL, 0.0f)
            == VST_PLUG_CATEG_SYNTH;
    g_vst2_block = max_block;

    /* Pre-allocate the MIDI event block (no malloc in the RT path). VstEvents
     * has a flexible events[1] tail; size it for VST2_MAX_EVENTS pointers. */
    b->midi_pool  = g_new0(VstMidiEvent, VST2_MAX_EVENTS);
    b->vst_events = (VstEvents *)g_malloc0(
        sizeof(VstEvents) + (VST2_MAX_EVENTS - 1) * sizeof(VstEvent *));

    eff->dispatcher(eff, EFF_OPEN, 0, 0, NULL, 0.0f);
    eff->dispatcher(eff, EFF_SET_SAMPLE_RATE, 0, 0, NULL, (float)sr);
    eff->dispatcher(eff, EFF_SET_BLOCK_SIZE, 0, max_block, NULL, 0.0f);
    eff->dispatcher(eff, EFF_MAINS_CHANGED, 0, 1, NULL, 0.0f);
    eff->dispatcher(eff, EFF_START_PROCESS, 0, 0, NULL, 0.0f);

    char nm[128] = {0};
    eff->dispatcher(eff, EFF_GET_EFFECT_NAME, 0, 0, nm, 0.0f);
    PluginInstance *pi = ph_instance_alloc(PH_VST2,
        nm[0] ? nm : info->name, sr, max_block);
    pi->ops = &vst2_ops;
    pi->backend = b;
    return pi;
}

#endif /* HAVE_VST2 */
