#define _GNU_SOURCE
#include <config.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_LV2

#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/data-access/data-access.h>
#include <lv2/state/state.h>
#include <lv2/resize-port/resize-port.h>
#include "rt_ringbuffer.h"
#include <semaphore.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <glib/gprintf.h>
#include <lv2/ui/ui.h>
#ifdef HAVE_SRATOM
#  include <sratom/sratom.h>
#  include "lv2ui_ipc.h"   /* the atom-over-pipe contract shared with the helper */
#endif
#ifdef HAVE_SUIL
#  include <suil/suil.h>
#  include <gtk/gtkx.h>   /* GTK_IS_SOCKET (suil's wrapper is a GtkSocket) */
#endif

#include "pluginhost_internal.h"

#define LV2_GTK3_UI_URI "http://lv2plug.in/ns/extensions/ui#Gtk3UI"

/* Capacity of each editor<->DSP atom ring (see Lv2Backend.atoms_to_dsp). */
#define LV2_ATOM_RING_BYTES (256u * 1024u)

/* Native plugin editors are hosted IN-PROCESS via suil, modeled on jalv
 * (jalv/src/gtk/jalv_gtk.c + jalv/src/jalv.c): the suil widget is embedded in a
 * GtkEventBox passed as ui:parent, with jalv's exact feature set; suil drives
 * the UI idle loop itself (we must NOT). Control-output values are pushed to the
 * UI on a 30 Hz timer (jalv_update analog). */

/* ---- Shared LilvWorld + cached nodes ---- */

static LilvWorld *world;
static LilvNode  *n_audio, *n_control, *n_input, *n_output, *n_atom_port, *n_cv;
static LilvNode  *n_min_size;   /* resize-port:minimumSize */

static void lv2_world_init(void)
{
    if (world) return;
    world = lilv_world_new();
    n_audio     = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
    n_control   = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
    n_input     = lilv_new_uri(world, LILV_URI_INPUT_PORT);
    n_output    = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
    n_atom_port = lilv_new_uri(world, LV2_ATOM__AtomPort);
    n_cv        = lilv_new_uri(world, LV2_CORE__CVPort);
    n_min_size  = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
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
    if (p->len == 0) {
        g_string_append(p, "/usr/lib/lv2");
#ifdef PH_MULTIARCH_LIBDIR
        g_string_append(p, ":" PH_MULTIARCH_LIBDIR "/lv2");
#endif
    }
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
    rt_ringbuffer_t          *requests;   /* RT  -> worker */
    rt_ringbuffer_t          *responses;  /* worker -> RT  */
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
    if (rt_ringbuffer_write_space(w->responses) < sizeof(size) + size)
        return LV2_WORKER_ERR_NO_SPACE;
    rt_ringbuffer_write(w->responses, (const char *)&size, sizeof(size));
    rt_ringbuffer_write(w->responses, (const char *)data, size);
    return LV2_WORKER_SUCCESS;
}

/* Take the next record's size header off a worker ring, but only once its
 * whole payload is in: the producer writes header and payload as two separate
 * writes, so the header alone does not mean the record is complete. Consuming
 * it early would hand work()/work_response() a short payload and desync every
 * record after it. An incomplete record is left for the next wake-up (worker)
 * or the next cycle (RT). RT-safe. */
static gboolean worker_ring_take(rt_ringbuffer_t *rb, uint32_t *size)
{
    size_t avail = rt_ringbuffer_read_space(rb);
    if (avail < sizeof *size) return FALSE;
    rt_ringbuffer_peek(rb, (char *)size, sizeof *size);
    if (avail - sizeof *size < *size) return FALSE;
    rt_ringbuffer_read_advance(rb, sizeof *size);
    return TRUE;
}

static gpointer worker_thread_fn(gpointer arg)
{
    Worker *w = arg;
    char buf[WORKER_BUF_BYTES];
    for (;;) {
        sem_wait(&w->sem);
        if (g_atomic_int_get(&w->quit)) break;
        uint32_t size;
        while (worker_ring_take(w->requests, &size)) {
            if (size > sizeof(buf)) {           /* oversized: drain & drop */
                rt_ringbuffer_read_advance(w->requests, size);
                continue;
            }
            rt_ringbuffer_read(w->requests, buf, size);
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
    if (rt_ringbuffer_write_space(w->requests) < sizeof(size) + size)
        return LV2_WORKER_ERR_NO_SPACE;
    rt_ringbuffer_write(w->requests, (const char *)&size, sizeof(size));
    rt_ringbuffer_write(w->requests, (const char *)data, size);
    sem_post(&w->sem);
    return LV2_WORKER_SUCCESS;
}

/* Apply pending worker responses then end_run — called from the RT thread. */
static void worker_apply_responses(Worker *w)
{
    if (!w->active || !w->iface) return;
    char buf[WORKER_BUF_BYTES];
    uint32_t size;
    while (worker_ring_take(w->responses, &size)) {
        if (size > sizeof(buf)) { rt_ringbuffer_read_advance(w->responses, size); continue; }
        rt_ringbuffer_read(w->responses, buf, size);
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
    w->requests  = rt_ringbuffer_create(WORKER_BUF_BYTES * 4);
    w->responses = rt_ringbuffer_create(WORKER_BUF_BYTES * 4);
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
    if (w->requests)  rt_ringbuffer_free(w->requests);
    if (w->responses) rt_ringbuffer_free(w->responses);
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

    int    *ain;  int n_audio_in;   /* ALL audio input port indices  */
    int    *aout; int n_audio_out;  /* ALL audio output port indices */
    guint  *ctl_in;  guint n_ctl_in;   /* control INPUT ports  (UI knobs)   */
    guint  *ctl_out; guint n_ctl_out;  /* control OUTPUT ports (UI meters)  */

    char   *uri, *ui_uri, *ui_type;    /* cached UI metadata */

    float  *outA, *outB;         /* scratch out buffers (max_block) */
    float  *dummy_in, *dummy_out;/* scratch for surplus audio + CV ports */
    void   *misc;                /* zeroed buffer for any leftover port type */
    int     max_block;

    /* Atom ports (MIDI/patch). Each needs its OWN buffer per instance, set up as
     * an LV2_Atom_Sequence and RESET before every run() — exactly as jalv does in
     * lv2_evbuf_reset. Sharing one un-initialised buffer (as before) let plugins
     * like gxtuner forge MIDI into a zero-capacity buffer -> heap corruption. */
    struct Lv2AtomPort {
        guint    index;
        gboolean is_input;
        uint32_t capacity;       /* bytes the plugin may write (output ports) */
        void    *buf[2];         /* sizeof(LV2_Atom_Sequence)+capacity, per inst */
    }      *atoms;
    guint   n_atoms;
    LV2_URID urid_seq, urid_chunk, urid_midi;

    /* MIDI delivery (instruments): forge events into the first atom INPUT port
     * before run(). midi_in_atom = index into atoms[] (-1 if none). */
    int               midi_in_atom;
    LV2_Atom_Forge    forge;

    /* Editor <-> DSP atom traffic (atom:eventTransfer / ui:portNotification).
     * Control ports carry floats and nothing else, so everything that is not a
     * number — a model/IR file to load, patch:Set, the plugin's state, a
     * capture list — travels as an atom through the control and notify ports.
     * A host that bridges only floats leaves those plugins visibly alive and
     * completely mute. Both rings are lock-free: the RT thread drains
     * atoms_to_dsp into the input sequence before run() and copies whatever the
     * plugin wrote on its atom OUTPUT ports into atoms_to_ui after it. */
    rt_ringbuffer_t *atoms_to_dsp;   /* UI thread -> RT  */
    rt_ringbuffer_t *atoms_to_ui;    /* RT -> UI thread  */
    void             *atom_scratch;    /* RT read buffer, sized to the ring    */
    guint32           atom_scratch_sz;
    LV2_URID          urid_ev_transfer;
    volatile gint     ui_open;         /* 0 = nobody drains atoms_to_ui        */

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
    SuilInstance   *ui;            /* in-process editor (suil), or NULL */
    PluginInstance *ui_owner;      /* the instance that editor belongs to */
    guint         ui_push_id;      /* 30 Hz timer pushing DSP -> UI */
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

/* Re-arm this instance's atom ports before run(): input ports become an empty
 * but valid LV2_Atom_Sequence; output ports advertise their full capacity as an
 * atom:Chunk. Matches jalv's lv2_evbuf_reset — without it a plugin writing MIDI
 * (e.g. gxtuner) forges into a buffer of unknown/zero capacity and corrupts the
 * heap. RT-safe: only touches the two header fields. */
static inline void lv2_reset_atoms(Lv2Backend *b, int k)
{
    for (guint a = 0; a < b->n_atoms; a++) {
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)b->atoms[a].buf[k];
        if (!seq) continue;
        if (b->atoms[a].is_input) {
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = b->urid_seq;
        } else {
            seq->atom.size = b->atoms[a].capacity;
            seq->atom.type = b->urid_chunk;
        }
    }
}

/* One ring record: an atom addressed to a port. The payload that follows is
 * the complete LV2_Atom (header included), so a reader can hand it straight to
 * the forge or to suil_instance_port_event. */
typedef struct { guint32 port; guint32 size; } Lv2AtomRec;

/* Move everything the editor queued since the last cycle out of the ring and
 * into the RT scratch buffer, so it can be forged into EVERY instance (a
 * dual-mono plugin runs two copies and both must see the same messages).
 * Returns the number of scratch bytes staged. RT-safe: no malloc, no lock. */
static guint32 lv2_stage_ui_atoms(Lv2Backend *b)
{
    if (!b->atoms_to_dsp) return 0;
    guint32 used = 0;
    for (;;) {
        Lv2AtomRec rec;
        if (rt_ringbuffer_read_space(b->atoms_to_dsp) < sizeof rec) break;
        rt_ringbuffer_peek(b->atoms_to_dsp, (char *)&rec, sizeof rec);
        if (rt_ringbuffer_read_space(b->atoms_to_dsp) < sizeof rec + rec.size) break;
        if (used + sizeof rec + rec.size > b->atom_scratch_sz) break;  /* next cycle */
        rt_ringbuffer_read_advance(b->atoms_to_dsp, sizeof rec);
        memcpy((char *)b->atom_scratch + used, &rec, sizeof rec);
        rt_ringbuffer_read(b->atoms_to_dsp,
                           (char *)b->atom_scratch + used + sizeof rec, rec.size);
        used += sizeof rec + rec.size;
    }
    return used;
}

/* Build instance k's atom INPUT sequence for this block: first whatever the
 * editor sent (file loads, patch:Set, state), then this block's MIDI. Replaces
 * the empty sequence lv2_reset_atoms wrote for that port. Mirrors jalv's
 * process.c, which forges its UI ring and its MIDI into the one sequence.
 * The forge refuses to write past the buffer, so an oversized message is
 * dropped rather than corrupting the heap. */
static void lv2_forge_input(Lv2Backend *b, int k, guint32 staged,
                            const PhMidiEvent *ev, int n_ev)
{
    if (b->midi_in_atom < 0 || (staged == 0 && n_ev == 0)) return;
    struct Lv2AtomPort *ap = &b->atoms[b->midi_in_atom];
    lv2_atom_forge_set_buffer(&b->forge, (uint8_t *)ap->buf[k],
                              sizeof(LV2_Atom_Sequence) + ap->capacity);
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_sequence_head(&b->forge, &frame, 0);

    for (guint32 off = 0; off + sizeof(Lv2AtomRec) <= staged; ) {
        Lv2AtomRec rec;
        memcpy(&rec, (char *)b->atom_scratch + off, sizeof rec);
        off += sizeof rec;
        if (off + rec.size > staged) break;
        /* Only the port we forge into; anything else was queued for a port this
         * host does not drive and is dropped (see ph_lv2_atom_to_dsp). */
        if (rec.port == ap->index) {
            lv2_atom_forge_frame_time(&b->forge, 0);
            lv2_atom_forge_write(&b->forge, (char *)b->atom_scratch + off, rec.size);
        }
        off += rec.size;
    }

    for (int i = 0; i < n_ev; i++) {
        lv2_atom_forge_frame_time(&b->forge, ev[i].time);
        lv2_atom_forge_atom(&b->forge, ev[i].size, b->urid_midi);
        lv2_atom_forge_write(&b->forge, ev[i].data, ev[i].size);
    }
    lv2_atom_forge_pop(&b->forge, &frame);
}

/* Copy whatever the plugin wrote on its atom OUTPUT ports (its notify port:
 * capture lists, state, parameter echoes) to the editor's ring. Only while an
 * editor is open — with nobody draining, the ring would fill and the plugin's
 * replies would go stale. RT-safe: fixed-size writes into a lock-free ring,
 * and a full ring drops the message rather than blocking. Instance 0 only:
 * a dual-mono pair has one editor and both copies say the same thing. */
static void lv2_collect_atom_out(Lv2Backend *b)
{
    if (!b->atoms_to_ui || !g_atomic_int_get(&b->ui_open)) return;
    for (guint a = 0; a < b->n_atoms; a++) {
        if (b->atoms[a].is_input) continue;
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)b->atoms[a].buf[0];
        if (!seq || seq->atom.type != b->urid_seq) continue;
        LV2_ATOM_SEQUENCE_FOREACH(seq, evt) {
            const LV2_Atom *atom = &evt->body;
            Lv2AtomRec rec = { b->atoms[a].index, (guint32)lv2_atom_total_size(atom) };
            if (rt_ringbuffer_write_space(b->atoms_to_ui) < sizeof rec + rec.size)
                return;   /* editor is behind; drop the rest of this block */
            rt_ringbuffer_write(b->atoms_to_ui, (const char *)&rec, sizeof rec);
            rt_ringbuffer_write(b->atoms_to_ui, (const char *)atom, rec.size);
        }
    }
}

static void lv2_process(PluginInstance *pi, float *L, float *R, int n)
{
    Lv2Backend *b = pi->backend;
    if (n > b->max_block) n = b->max_block;
    if (b->n_audio_in == 0 || b->n_audio_out == 0) return;

    const guint32 staged = lv2_stage_ui_atoms(b);

    if (b->dual_mono) {
        lilv_instance_connect_port(b->inst[0], b->ain[0],  L);
        lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
        lilv_instance_connect_port(b->inst[1], b->ain[0],  R);
        lilv_instance_connect_port(b->inst[1], b->aout[0], b->outB);
        lv2_reset_atoms(b, 0);
        lv2_reset_atoms(b, 1);
        lv2_forge_input(b, 0, staged, NULL, 0);
        lv2_forge_input(b, 1, staged, NULL, 0);
        lilv_instance_run(b->inst[0], n);
        lilv_instance_run(b->inst[1], n);
        worker_apply_responses(&b->workers[0]);
        worker_apply_responses(&b->workers[1]);
        lv2_collect_atom_out(b);
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

        lv2_reset_atoms(b, 0);
        lv2_forge_input(b, 0, staged, NULL, 0);
        lilv_instance_run(b->inst[0], n);
        worker_apply_responses(&b->workers[0]);
        lv2_collect_atom_out(b);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        if (b->n_audio_out > 1) memcpy(R, b->outB, (size_t)n * sizeof(float));
        else                    memcpy(R, b->outA, (size_t)n * sizeof(float));
    }
}

/* Instrument render: forge MIDI, feed silent audio inputs, run, copy outputs.
 * (Synths usually have 0 audio inputs, which lv2_process would skip.) */
static void lv2_process_midi(PluginInstance *pi, const PhMidiEvent *ev,
                             int n_ev, float *L, float *R, int n)
{
    Lv2Backend *b = pi->backend;
    if (n > b->max_block) n = b->max_block;

    lv2_reset_atoms(b, 0);
    lv2_forge_input(b, 0, lv2_stage_ui_atoms(b), ev, n_ev);

    for (int i = 0; i < b->n_audio_in; i++)
        lilv_instance_connect_port(b->inst[0], b->ain[i], b->dummy_in); /* silence */
    if (b->n_audio_out > 0) lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
    if (b->n_audio_out > 1) lilv_instance_connect_port(b->inst[0], b->aout[1], b->outB);
    for (int i = 2; i < b->n_audio_out; i++)
        lilv_instance_connect_port(b->inst[0], b->aout[i], b->dummy_out);

    lilv_instance_run(b->inst[0], n);
    worker_apply_responses(&b->workers[0]);
    lv2_collect_atom_out(b);

    if (b->n_audio_out > 0) memcpy(L, b->outA, (size_t)n * sizeof(float));
    else                    memset(L, 0, (size_t)n * sizeof(float));
    if (b->n_audio_out > 1) memcpy(R, b->outB, (size_t)n * sizeof(float));
    else                    memcpy(R, L, (size_t)n * sizeof(float));
}

static void lv2_destroy_gui(PluginInstance *pi);   /* fwd (suil section below) */

static void lv2_reset(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!b) return;
    for (int i = 0; i < b->n_inst; i++) {
        if (!b->inst[i]) continue;
        lilv_instance_deactivate(b->inst[i]);
        lilv_instance_activate(b->inst[i]);
    }
}

static void lv2_destroy(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!b) return;
    lv2_destroy_gui(pi);              /* stop UI push timer + free suil instance */
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
    g_free(b->ain); g_free(b->aout); g_free(b->ctl_in); g_free(b->ctl_out);
    g_free(b->outA);
    g_free(b->outB);
    g_free(b->dummy_in); g_free(b->dummy_out);
    g_free(b->misc);
    for (guint a = 0; a < b->n_atoms; a++) { g_free(b->atoms[a].buf[0]); g_free(b->atoms[a].buf[1]); }
    g_free(b->atoms);
    if (b->atoms_to_dsp) rt_ringbuffer_free(b->atoms_to_dsp);
    if (b->atoms_to_ui)  rt_ringbuffer_free(b->atoms_to_ui);
    g_free(b->atom_scratch);
    g_free(b->uri); g_free(b->ui_uri); g_free(b->ui_type);
    g_free(b);
}

/* ---- Out-of-process UI metadata + control-port access (for lv2ui_bridge) ---- */

/* Choose the best UI we have a helper for, preferring the gtk3-handled types.
 * Caches the plugin URI + chosen UI URI/type on the backend. */
gboolean ph_lv2_ui_meta(PluginInstance *pi, const char **plugin_uri,
                        const char **ui_uri, const char **ui_type)
{
    Lv2Backend *b = pi->backend;
    if (!b->uri)
        b->uri = g_strdup(lilv_node_as_uri(lilv_plugin_get_uri(b->plugin)));

    if (!b->ui_uri) {
        LilvUIs *uis = lilv_plugin_get_uis(b->plugin);
        if (uis) {
            /* preference order: X11/Gtk3 (gtk3 helper) > Gtk2 > Qt5 > Qt6 */
            static const char *pref[] = {
                "http://lv2plug.in/ns/extensions/ui#X11UI",
                "http://lv2plug.in/ns/extensions/ui#Gtk3UI",
                "http://lv2plug.in/ns/extensions/ui#GtkUI",
                "http://lv2plug.in/ns/extensions/ui#Qt5UI",
                "http://lv2plug.in/ns/extensions/ui#Qt6UI", NULL };
            for (int pi_i = 0; pref[pi_i] && !b->ui_uri; pi_i++) {
                LilvNode *want = lilv_new_uri(world, pref[pi_i]);
                LILV_FOREACH(uis, it, uis) {
                    const LilvUI *ui = lilv_uis_get(uis, it);
                    if (lilv_ui_is_a(ui, want)) {
                        b->ui_uri  = g_strdup(lilv_node_as_uri(lilv_ui_get_uri(ui)));
                        b->ui_type = g_strdup(pref[pi_i]);
                        break;
                    }
                }
                lilv_node_free(want);
            }
            lilv_uis_free(uis);
        }
    }
    if (!b->ui_uri) return FALSE;
    if (plugin_uri) *plugin_uri = b->uri;
    if (ui_uri)     *ui_uri     = b->ui_uri;
    if (ui_type)    *ui_type    = b->ui_type;
    return TRUE;
}

void ph_lv2_ctl_set(PluginInstance *pi, guint port, float v)
{ Lv2Backend *b = pi->backend; if (port < b->n_ports) b->ctl[port] = v; }

float ph_lv2_ctl_get(PluginInstance *pi, guint port)
{ Lv2Backend *b = pi->backend; return port < b->n_ports ? b->ctl[port] : 0.0f; }

void ph_lv2_ctl_ports(PluginInstance *pi, gboolean outputs,
                      const guint **ports, guint *n)
{
    Lv2Backend *b = pi->backend;
    if (outputs) { *ports = b->ctl_out; *n = b->n_ctl_out; }
    else         { *ports = b->ctl_in;  *n = b->n_ctl_in;  }
}

/* ---- Editor <-> DSP atom traffic ------------------------------------------
 * Shared by the in-process suil editor and the out-of-process UI bridge. The
 * raw pair moves atoms inside this process; the Turtle pair is for the bridge,
 * because a URID is only meaningful in the process that mapped it, so the atom
 * has to cross as text and be re-mapped on the far side (sratom, the same
 * serialisation LV2 state files use).
 */

guint32 ph_lv2_atom_transfer_urid(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    return b ? b->urid_ev_transfer : 0;
}

void ph_lv2_atom_ui_open(PluginInstance *pi, gboolean open)
{
    Lv2Backend *b = pi ? pi->backend : NULL;
    if (!b) return;
    if (open && b->atoms_to_ui) {
        /* Drop anything queued while no editor was listening — it is stale by
         * the time one opens. Draining from the READ side is this thread's own
         * half of the ring; rt_ringbuffer_reset touches both pointers and
         * would race the RT thread if it were mid-write. */
        rt_ringbuffer_read_advance(b->atoms_to_ui,
                                   rt_ringbuffer_read_space(b->atoms_to_ui));
    }
    g_atomic_int_set(&b->ui_open, open ? 1 : 0);
}

gboolean ph_lv2_atom_to_dsp(PluginInstance *pi, guint32 port,
                            const void *atom, guint32 size)
{
    Lv2Backend *b = pi ? pi->backend : NULL;
    if (!b || !b->atoms_to_dsp || !atom || size < sizeof(LV2_Atom)) return FALSE;
    /* Only the atom input port the RT thread forges into is driven; see
     * lv2_forge_input. Anything else would be queued and never delivered. */
    if (b->midi_in_atom < 0 || b->atoms[b->midi_in_atom].index != port) return FALSE;

    Lv2AtomRec rec = { port, size };
    if (rt_ringbuffer_write_space(b->atoms_to_dsp) < sizeof rec + size)
        return FALSE;
    rt_ringbuffer_write(b->atoms_to_dsp, (const char *)&rec, sizeof rec);
    rt_ringbuffer_write(b->atoms_to_dsp, (const char *)atom, size);
    return TRUE;
}

gboolean ph_lv2_atom_from_dsp(PluginInstance *pi, guint32 *port,
                              void *buf, guint32 bufsz, guint32 *size)
{
    Lv2Backend *b = pi ? pi->backend : NULL;
    if (!b || !b->atoms_to_ui) return FALSE;

    Lv2AtomRec rec;
    if (rt_ringbuffer_read_space(b->atoms_to_ui) < sizeof rec) return FALSE;
    rt_ringbuffer_peek(b->atoms_to_ui, (char *)&rec, sizeof rec);
    if (rt_ringbuffer_read_space(b->atoms_to_ui) < sizeof rec + rec.size)
        return FALSE;                      /* still being written; try again */
    rt_ringbuffer_read_advance(b->atoms_to_ui, sizeof rec);
    if (rec.size > bufsz) {                /* cannot deliver it: drop it */
        rt_ringbuffer_read_advance(b->atoms_to_ui, rec.size);
        return FALSE;
    }
    rt_ringbuffer_read(b->atoms_to_ui, (char *)buf, rec.size);
    *port = rec.port;
    *size = rec.size;
    return TRUE;
}

#ifdef HAVE_SRATOM
/* One Sratom for the whole host, used only on the GTK thread. The base URI and
 * the subject/predicate the atom hangs off are the protocol's (lv2ui_ipc.h):
 * the helper parses what we write and we parse what it writes. */
static Sratom *lv2_sratom(void)
{
    static Sratom *sr;
    if (!sr) {
        sr = sratom_new(&urid_map);
        if (sr) sratom_set_pretty_numbers(sr, false);
    }
    return sr;
}
static SerdNode lv2_atom_subject(void)
{ return serd_node_from_string(SERD_URI, (const uint8_t *)LV2UI_IPC_ATOM_SUBJECT); }
static SerdNode lv2_atom_pred(void)
{ return serd_node_from_string(SERD_URI, (const uint8_t *)LV2UI_IPC_ATOM_PRED); }

char *ph_lv2_atom_pop_turtle(PluginInstance *pi, guint32 *port)
{
    /* Big enough for the largest message a port asked room for (rsz:minimumSize
     * capacities are checked against this when the ring is written). */
    static char buf[LV2_ATOM_RING_BYTES];
    guint32 size = 0;
    if (!ph_lv2_atom_from_dsp(pi, port, buf, sizeof buf, &size)) return NULL;

    Sratom *sr = lv2_sratom();
    if (!sr) return NULL;
    const LV2_Atom *atom = (const LV2_Atom *)buf;
    SerdNode subj = lv2_atom_subject(), pred = lv2_atom_pred();
    char *ttl = sratom_to_turtle(sr, &urid_unmap, LV2UI_IPC_ATOM_BASE,
                                 &subj, &pred,
                                 atom->type, atom->size, LV2_ATOM_BODY_CONST(atom));
    if (!ttl) return NULL;
    char *out = g_strdup(ttl);
    free(ttl);
    return out;
}

gboolean ph_lv2_atom_push_turtle(PluginInstance *pi, guint32 port, const char *ttl)
{
    Sratom *sr = lv2_sratom();
    if (!sr || !ttl) return FALSE;
    SerdNode subj = lv2_atom_subject(), pred = lv2_atom_pred();
    LV2_Atom *atom = sratom_from_turtle(sr, LV2UI_IPC_ATOM_BASE, &subj, &pred, ttl);
    if (!atom) return FALSE;
    gboolean ok = ph_lv2_atom_to_dsp(pi, port, atom,
                                     (guint32)lv2_atom_total_size(atom));
    free(atom);
    return ok;
}
#endif /* HAVE_SRATOM */

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

/* ---- In-process native GUI via suil (modeled on jalv) ----------------------
 * jalv/src/jalv.c:jalv_instantiate_ui + jalv/src/gtk/jalv_gtk.c. Key points:
 *  - the suil widget is embedded in a GtkEventBox passed as ui:parent;
 *  - features = map, unmap, instance-access, data-access, log, parent, options,
 *    idleInterface;
 *  - suil drives the UI idle loop itself (we must NOT);
 *  - control values are pushed to the UI on a timer (jalv_update analog).
 */

void ph_lv2_ui_init(int *argc, char ***argv)
{
#ifdef HAVE_SUIL
    static gboolean done = FALSE;
    if (done) return;
    suil_init(argc, argv, SUIL_ARG_NONE);   /* once, before any UI is wrapped */
    done = TRUE;
#else
    (void)argc; (void)argv;
#endif
}

#ifdef HAVE_SUIL
static SuilHost *suil_host;

/* UI wrote a port. A control port is a float the RT thread reads out of b->ctl;
 * anything else is an atom (a file to load, patch:Set, state) bound for the
 * plugin's control port, which only reaches it through the ring. */
static void lv2_ui_write(SuilController c, uint32_t port, uint32_t size,
                         uint32_t protocol, const void *buffer)
{
    PluginInstance *pi = (PluginInstance *)c;
    Lv2Backend *b = pi->backend;
    if (protocol == 0 && size == sizeof(float) && port < b->n_ports)
        b->ctl[port] = *(const float *)buffer;
    else if (protocol == b->urid_ev_transfer)
        ph_lv2_atom_to_dsp(pi, port, buffer, size);
}

static uint32_t lv2_ui_port_index(SuilController c, const char *symbol)
{
    PluginInstance *pi = (PluginInstance *)c;
    Lv2Backend *b = pi->backend;
    for (guint i = 0; i < b->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(b->plugin, i);
        const LilvNode *sym  = lilv_port_get_symbol(b->plugin, port);
        if (sym && !g_strcmp0(lilv_node_as_string(sym), symbol)) return i;
    }
    return LV2UI_INVALID_PORT_INDEX;
}

/* Push what the DSP has to say to the UI — the jalv_update analog. Two kinds:
 * control-OUTPUT values (meters, tuner readouts) and whatever the plugin forged
 * on its notify port this cycle. Does NOT drive idle (suil does that internally
 * for X11 UIs). */
static gboolean lv2_ui_push_cb(gpointer data)
{
    Lv2Backend *b = data;
    if (!b->ui) return G_SOURCE_REMOVE;

    /* Notify-port atoms first: a state or capture-list reply should land before
     * the meter values that follow it. Main thread only, so a static buffer. */
    {
        static char atom_buf[LV2_ATOM_RING_BYTES];
        guint32 port, size;
        PluginInstance *pi = b->ui_owner;
        while (pi && ph_lv2_atom_from_dsp(pi, &port, atom_buf,
                                          sizeof atom_buf, &size))
            suil_instance_port_event(b->ui, port, size, b->urid_ev_transfer,
                                     atom_buf);
    }

    for (guint i = 0; i < b->n_ctl_out; i++) {
        guint idx = b->ctl_out[i];
        float v = b->ctl[idx];
        /* Never hand a non-finite meter value to the UI: a NaN/Inf from the DSP
         * (e.g. a tuner's detected frequency) can drive the plugin's own drawing
         * code out of bounds and crash it (gxtuner aborts in cairo). RT-side
         * denormal flushing should prevent these, but guard the UI regardless. */
        if (!isfinite(v)) continue;
        suil_instance_port_event(b->ui, idx, sizeof(float), 0, &v);
    }
    return G_SOURCE_CONTINUE;
}

static void lv2_destroy_gui(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!b) return;
    if (b->ui_push_id) { g_source_remove(b->ui_push_id); b->ui_push_id = 0; }
    ph_lv2_atom_ui_open(pi, FALSE);
    b->ui_owner = NULL;
    if (b->ui) {
        /* suil's x11_in_gtk3 wrapper only removes its internal idle timer in its
         * "plug-removed" handler, but suil_instance_free destroys the socket
         * BEFORE the plugin window, so that never fires — the idle then leaks and
         * fires on freed memory in a long-running host (jalv survives only by
         * quitting). Fire plug-removed first so suil tears the idle down. */
        GtkWidget *w = (GtkWidget *)suil_instance_get_widget(b->ui);
        if (w && GTK_IS_SOCKET(w)) {
            gboolean ret = FALSE;
            g_signal_emit_by_name(w, "plug-removed", &ret);
        }
        suil_instance_free(b->ui);
        b->ui = NULL;
    }
}

static GtkWidget *lv2_make_gui(PluginInstance *pi)
{
    Lv2Backend *b = pi->backend;
    if (!suil_host)
        suil_host = suil_host_new(lv2_ui_write, lv2_ui_port_index, NULL, NULL);
    if (!suil_host) return NULL;

    /* Host ONLY native Gtk3UI editors in-process. These are GTK/pango based
     * (e.g. guitarix) and share our libcairo/pango font caches safely. Toolkit-
     * agnostic X11UI editors are deliberately NOT hosted here: many draw with
     * cairo's "toy" font API (cairo_select_font_face/text_extents), which fights
     * our pango usage over libcairo's single global font-face cache and triggers
     * a use-after-free *inside libcairo* (confirmed by ASan on gxtuner). Those go
     * out-of-process via the bridge (jackdaw-lv2ui-gtk3), where the plugin is the
     * sole cairo consumer — the same isolation that makes them work in Reaper.
     *
     * Exception: an X11UI that REQUIRES instance-access or data-access needs a
     * pointer into our DSP instance, which no out-of-process helper can give it
     * (JUCE's LV2 UI wrapper returns NULL from instantiate without it — e.g.
     * TONE3000). Those can only ever run in-process, so host them here via
     * suil's x11_in_gtk3 wrapper. */
    LilvUIs *uis = lilv_plugin_get_uis(b->plugin);
    if (!uis) return NULL;
    LilvNode *gtk3 = lilv_new_uri(world, LV2_GTK3_UI_URI);
    LilvNode *x11  = lilv_new_uri(world, LV2_UI__X11UI);
    LilvNode *req  = lilv_new_uri(world, LV2_CORE__requiredFeature);
    LilvNode *ia   = lilv_new_uri(world, LV2_INSTANCE_ACCESS_URI);
    LilvNode *da   = lilv_new_uri(world, LV2_DATA_ACCESS_URI);
    const LilvUI   *use_ui   = NULL;
    const LilvNode *use_type = NULL;
    LILV_FOREACH(uis, it, uis) {
        const LilvUI *ui = lilv_uis_get(uis, it);
        if (lilv_ui_is_a(ui, gtk3)) {   /* native Gtk3UI: always preferred */
            use_ui = ui; use_type = gtk3; break;
        }
        if (!use_ui && lilv_ui_is_a(ui, x11)) {
            const LilvNode *u = lilv_ui_get_uri(ui);
            lilv_world_load_resource(world, u);   /* ui.ttl via rdfs:seeAlso */
            if (lilv_world_ask(world, u, req, ia) || lilv_world_ask(world, u, req, da)) {
                use_ui = ui; use_type = x11;   /* keep looking for a Gtk3UI */
            }
        }
    }
    lilv_node_free(req); lilv_node_free(ia); lilv_node_free(da);
    if (!use_ui) {
        lilv_node_free(gtk3); lilv_node_free(x11); lilv_uis_free(uis);
        return NULL;
    }

    /* Parent the editor in a GtkEventBox (its own native X window) — this is
     * what jalv does and what makes embedding reliable. */
    GtkWidget *box = gtk_event_box_new();
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(box, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);

    /* UI options (jalv passes these): sample rate + UI update rate. */
    float f_sr   = (float)pi->sample_rate;
    float f_rate = 30.0f;
    const LV2_URID urid_float = urid_map_cb(NULL, LV2_ATOM__Float);
    LV2_Options_Option ui_opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, urid_map_cb(NULL, LV2_PARAMETERS__sampleRate),
          sizeof(float), urid_float, &f_sr },
        { LV2_OPTIONS_INSTANCE, 0, urid_map_cb(NULL, LV2_UI__updateRate),
          sizeof(float), urid_float, &f_rate },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
    };

    b->ui_ext_data.data_access =
        lilv_instance_get_descriptor(b->inst[0])->extension_data;

    LV2_Feature feat_inst = { LV2_INSTANCE_ACCESS_URI,
                              lilv_instance_get_handle(b->inst[0]) };
    LV2_Feature feat_data = { LV2_DATA_ACCESS_URI, &b->ui_ext_data };
    LV2_Feature feat_parent = { LV2_UI__parent, box };
    LV2_Feature feat_uiopts = { LV2_OPTIONS__options, ui_opts };
    LV2_Feature feat_idle = { LV2_UI__idleInterface, NULL };
    const LV2_Feature *ui_features[] = {
        &feat_map, &feat_unmap, &feat_inst, &feat_data, &feat_log,
        &feat_parent, &feat_uiopts, &feat_idle, NULL };

    char *bundle = lilv_node_get_path(lilv_ui_get_bundle_uri(use_ui), NULL);
    char *binary = lilv_node_get_path(lilv_ui_get_binary_uri(use_ui), NULL);

    b->ui = suil_instance_new(
        suil_host, pi, LV2_GTK3_UI_URI,
        lilv_node_as_uri(lilv_plugin_get_uri(b->plugin)),
        lilv_node_as_uri(lilv_ui_get_uri(use_ui)),
        lilv_node_as_uri(use_type),
        bundle ? bundle : "", binary ? binary : "", ui_features);

    lilv_free(bundle); lilv_free(binary);
    lilv_node_free(gtk3); lilv_node_free(x11); lilv_uis_free(uis);

    if (!b->ui) { gtk_widget_destroy(box); return NULL; }
    GtkWidget *w = (GtkWidget *)suil_instance_get_widget(b->ui);
    if (!w) { suil_instance_free(b->ui); b->ui = NULL; gtk_widget_destroy(box); return NULL; }
    gtk_container_add(GTK_CONTAINER(box), w);

    /* Seed the UI with current control-input values so knobs start correct. */
    for (guint i = 0; i < b->n_ctl_in; i++) {
        guint idx = b->ctl_in[i];
        suil_instance_port_event(b->ui, idx, sizeof(float), 0, &b->ctl[idx]);
    }
    b->ui_owner = pi;
    ph_lv2_atom_ui_open(pi, TRUE);   /* the RT thread may start filling the ring */
    b->ui_push_id = g_timeout_add(33, lv2_ui_push_cb, b);
    return box;
}
#else  /* !HAVE_SUIL */
static GtkWidget *lv2_make_gui(PluginInstance *pi) { (void)pi; return NULL; }
static void lv2_destroy_gui(PluginInstance *pi) { (void)pi; }
#endif

/* ---- Opaque state (project save/reload) ----
 *
 * The control-port values alone are NOT an LV2 plugin's full state: anything
 * behind state:interface — a convolver's loaded IR, a sampler's loaded file, a
 * neural amp model — lives only in the plugin's own state. Without these ops
 * project_save_fx() wrote no `state` blob for LV2 at all, so every such plugin
 * came back empty on reload. Serialised as Turtle text via lilv, which is what
 * lilv_state_new_from_string() consumes on the way back in. */

#define LV2_STATE_MAX (16u << 20)   /* bound untrusted project-file input */

/* Port symbol for a control-port index, or NULL. */
static const char *lv2_port_sym(Lv2Backend *b, guint port_index)
{
    const LilvPort *p = lilv_plugin_get_port_by_index(b->plugin, port_index);
    const LilvNode *s = p ? lilv_port_get_symbol(b->plugin, p) : NULL;
    return s ? lilv_node_as_string(s) : NULL;
}

static const void *lv2_get_port_value(const char *sym, void *user,
                                      uint32_t *size, uint32_t *type)
{
    PluginInstance *pi = user;
    Lv2Backend *b = pi->backend;
    for (guint i = 0; i < b->n_params; i++) {
        const char *s = lv2_port_sym(b, b->params[i].port_index);
        if (s && !strcmp(s, sym)) {
            *size = sizeof(float);
            *type = urid_map_cb(NULL, LV2_ATOM__Float);
            return &b->ctl[b->params[i].port_index];
        }
    }
    *size = 0; *type = 0;
    return NULL;
}

static void lv2_set_port_value(const char *sym, void *user, const void *value,
                               uint32_t size, uint32_t type)
{
    PluginInstance *pi = user;
    Lv2Backend *b = pi->backend;
    float v;

    if (type == urid_map_cb(NULL, LV2_ATOM__Float) && size == sizeof(float))
        v = *(const float *)value;
    else if (type == urid_map_cb(NULL, LV2_ATOM__Int) && size == sizeof(int32_t))
        v = (float)*(const int32_t *)value;
    else if (type == urid_map_cb(NULL, LV2_ATOM__Double) && size == sizeof(double))
        v = (float)*(const double *)value;
    else if (type == urid_map_cb(NULL, LV2_ATOM__Bool) && size == sizeof(int32_t))
        v = (*(const int32_t *)value) ? 1.0f : 0.0f;
    else
        return;                                   /* unknown encoding: ignore */

    for (guint i = 0; i < b->n_params; i++) {
        const char *s = lv2_port_sym(b, b->params[i].port_index);
        if (s && !strcmp(s, sym)) {
            b->ctl[b->params[i].port_index] =
                CLAMP(v, b->params[i].min, b->params[i].max);
            return;
        }
    }
}

static gboolean lv2_state_save(PluginInstance *pi, void **out, gsize *out_len)
{
    Lv2Backend *b = pi->backend;
    if (!b || !b->inst[0] || !out || !out_len) return FALSE;

    LilvState *st = lilv_state_new_from_instance(
        b->plugin, b->inst[0], &urid_map,
        NULL, NULL, NULL, NULL,
        lv2_get_port_value, pi,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, NULL);
    if (!st) return FALSE;

    char *str = lilv_state_to_string(world, &urid_map, &urid_unmap, st,
                                     "urn:jackdaw:state", NULL);
    lilv_state_free(st);
    if (!str) return FALSE;

    gsize len = strlen(str);
    *out = g_memdup2(str, len);
    *out_len = len;
    lilv_free(str);
    return *out != NULL;
}

static gboolean lv2_state_load(PluginInstance *pi, const void *data, gsize len)
{
    Lv2Backend *b = pi->backend;
    if (!b || !b->inst[0] || !data || len == 0 || len > LV2_STATE_MAX)
        return FALSE;

    /* Untrusted project-file input: bounded above, and NUL-terminated in a
     * private copy before the Turtle parser sees it. */
    char *str = g_malloc(len + 1);
    memcpy(str, data, len);
    str[len] = 0;

    LilvState *st = lilv_state_new_from_string(world, &urid_map, str);
    g_free(str);
    if (!st) return FALSE;

    for (int k = 0; k < b->n_inst; k++)
        lilv_state_restore(st, b->inst[k], lv2_set_port_value, pi, 0, NULL);
    lilv_state_free(st);
    return TRUE;
}

static const PhOps lv2_ops = {
    .process     = lv2_process,
    .process_midi = lv2_process_midi,
    .destroy     = lv2_destroy,
    .make_gui    = lv2_make_gui,
    .destroy_gui = lv2_destroy_gui,
    .param_count = lv2_param_count,
    .param_name  = lv2_param_name,
    .param_get   = lv2_param_get,
    .param_set   = lv2_param_set,
    .param_range = lv2_param_range,
    .reset       = lv2_reset,
    .state_save  = lv2_state_save,
    .state_load  = lv2_state_load,
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
    b->ain       = g_new0(int,   b->n_ports ? b->n_ports : 1);
    b->aout      = g_new0(int,   b->n_ports ? b->n_ports : 1);
    b->ctl_in    = g_new0(guint, b->n_ports ? b->n_ports : 1);
    b->ctl_out   = g_new0(guint, b->n_ports ? b->n_ports : 1);

    /* Default control values */
    float *mins = g_new0(float, b->n_ports);
    float *maxs = g_new0(float, b->n_ports);
    float *defs = g_new0(float, b->n_ports);
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    GArray *params = g_array_new(FALSE, FALSE, sizeof(Lv2Param));
    /* Collect atom ports so each can get its own properly-sized, reset-per-run
     * LV2_Atom_Sequence buffer (jalv model). Capacity is generous for MIDI. */
    const uint32_t atom_cap = (uint32_t)MAX((gsize)max_block * sizeof(float), (gsize)8192);
    GArray *atomg = g_array_new(FALSE, FALSE, sizeof(struct Lv2AtomPort));

    for (guint i = 0; i < b->n_ports; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
        gboolean is_audio = lilv_port_is_a(p, port, n_audio);
        gboolean is_ctl   = lilv_port_is_a(p, port, n_control);
        gboolean is_in    = lilv_port_is_a(p, port, n_input);
        gboolean is_out   = lilv_port_is_a(p, port, n_output);

        if (is_audio) {
            if (is_in)  b->ain [b->n_audio_in++ ] = (int)i;
            if (is_out) b->aout[b->n_audio_out++] = (int)i;
        } else if (lilv_port_is_a(p, port, n_atom_port)) {
            struct Lv2AtomPort ap;
            memset(&ap, 0, sizeof ap);
            ap.index = i; ap.is_input = is_in; ap.capacity = atom_cap;
            /* rsz:minimumSize is the port saying how much it needs to say what
             * it has to say — a notify port that carries a state blob or a file
             * list asks for far more than the MIDI-sized default. Give it what
             * it asked for; short-changing it silently truncates those
             * messages. */
            LilvNodes *ms = lilv_port_get_value(p, port, n_min_size);
            if (ms) {
                const LilvNode *v = lilv_nodes_get_first(ms);
                if (v && lilv_node_is_int(v)) {
                    int want = lilv_node_as_int(v);
                    if (want > 0 && (uint32_t)want > ap.capacity)
                        ap.capacity = (uint32_t)want;
                }
                lilv_nodes_free(ms);
            }
            g_array_append_val(atomg, ap);
        } else if (is_ctl) {
            float d = defs[i];
            if (d != d) d = 0.0f;   /* NaN guard */
            b->ctl[i] = d;
            if (is_in) {
                b->ctl_in[b->n_ctl_in++] = i;     /* knobs/sliders (UI sync) */
                Lv2Param pr;
                pr.port_index = i;
                LilvNode *pn = lilv_port_get_name(p, port);
                pr.name = g_strdup(pn ? lilv_node_as_string(pn) : "param");
                if (pn) lilv_node_free(pn);
                pr.min = mins[i]; pr.max = maxs[i];
                if (pr.max <= pr.min) pr.max = pr.min + 1.0f;
                g_array_append_val(params, pr);
            } else if (is_out) {
                b->ctl_out[b->n_ctl_out++] = i;   /* meters/tuner feed UI */
            }
        }
    }
    g_free(mins); g_free(maxs); g_free(defs);

    b->n_params = params->len;
    b->params   = (Lv2Param *)g_array_free(params, FALSE);

    b->n_atoms    = atomg->len;
    b->atoms      = (struct Lv2AtomPort *)g_array_free(atomg, FALSE);
    b->urid_seq   = urid_map_cb(NULL, LV2_ATOM__Sequence);
    b->urid_chunk = urid_map_cb(NULL, LV2_ATOM__Chunk);
    b->urid_midi  = urid_map_cb(NULL, LV2_MIDI__MidiEvent);
    b->urid_ev_transfer = urid_map_cb(NULL, LV2_ATOM__eventTransfer);
    lv2_atom_forge_init(&b->forge, &urid_map);

    /* Editor <-> DSP atom rings. Sized for the largest thing that travels on
     * them (a state blob or a capture list, not a MIDI note), so a big message
     * is delivered rather than dropped. */
    b->atoms_to_dsp    = rt_ringbuffer_create(LV2_ATOM_RING_BYTES);
    b->atoms_to_ui     = rt_ringbuffer_create(LV2_ATOM_RING_BYTES);
    b->atom_scratch_sz = LV2_ATOM_RING_BYTES;
    b->atom_scratch    = g_malloc0(b->atom_scratch_sz);
    if (b->atoms_to_dsp) rt_ringbuffer_mlock(b->atoms_to_dsp);
    if (b->atoms_to_ui)  rt_ringbuffer_mlock(b->atoms_to_ui);
    /* The instrument's MIDI sink = the first atom INPUT port. */
    b->midi_in_atom = -1;
    for (guint a = 0; a < b->n_atoms; a++)
        if (b->atoms[a].is_input) { b->midi_in_atom = (int)a; break; }

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
            for (guint a = 0; a < b->n_atoms; a++) { g_free(b->atoms[a].buf[0]); g_free(b->atoms[a].buf[1]); }
            g_free(b->atoms);
            for (guint q = 0; q < b->n_params; q++) g_free(b->params[q].name);
            g_free(b->params); g_free(b->ctl);
            g_free(b->ain); g_free(b->aout); g_free(b->ctl_in); g_free(b->ctl_out);
            g_free(b->outA); g_free(b->outB);
            g_free(b->dummy_in); g_free(b->dummy_out); g_free(b->misc);
            g_free(b);
            return NULL;
        }
        /* This instance's atom buffers (own buffer per port, so dual-mono
         * instances never share — and never the old single misc buffer). */
        for (guint a = 0; a < b->n_atoms; a++)
            b->atoms[a].buf[k] =
                g_malloc0(sizeof(LV2_Atom_Sequence) + b->atoms[a].capacity);
        lv2_reset_atoms(b, k);

        /* Connect EVERY port up front. Control → its slot in the value store;
         * audio/CV → scratch (audio is re-bound per process call); atom → its own
         * sequence buffer; anything else → the zeroed misc buffer. */
        for (guint i = 0; i < b->n_ports; i++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
            if (lilv_port_is_a(p, port, n_control)) {
                lilv_instance_connect_port(b->inst[k], i, &b->ctl[i]);
            } else if (lilv_port_is_a(p, port, n_audio)) {
                lilv_instance_connect_port(b->inst[k], i,
                    lilv_port_is_a(p, port, n_input) ? b->dummy_in : b->dummy_out);
            } else if (lilv_port_is_a(p, port, n_atom_port)) {
                void *buf = b->misc;
                for (guint a = 0; a < b->n_atoms; a++)
                    if (b->atoms[a].index == i) { buf = b->atoms[a].buf[k]; break; }
                lilv_instance_connect_port(b->inst[k], i, buf);
            } else if (lilv_port_is_a(p, port, n_cv)) {
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
