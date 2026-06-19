#ifndef PLUGINHOST_H_INCLUDED
#define PLUGINHOST_H_INCLUDED

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Unified plugin host — one interface over LV2, VST2, VST3 and CLAP.
 * Discovery/instantiation/GUI happen on the main thread; pluginhost_process()
 * is RT-safe (no malloc/locks). See CLAUDE.md "Plugin Formats".
 */

typedef enum {
    PH_LV2 = 0,
    PH_VST2,
    PH_VST3,
    PH_CLAP,
    PH_LADSPA,
    PH_NFORMATS
} PluginFormat;

/* A catalog entry produced by scanning. */
typedef struct {
    PluginFormat format;
    char        *key;        /* LV2 URI; abs file path ("path" or "path\nN") else */
    char        *name;
    char        *category;   /* declared class/category, else format name */
    gboolean     is_instrument; /* synth/instrument (takes MIDI, makes audio) */
} PluginInfo;

typedef struct PluginInstance PluginInstance;

/* One MIDI event for delivery to an instrument plugin. `time` is the sample
 * offset within the current process block; size is 1..3 bytes. */
typedef struct {
    guint32 time;
    guint8  size;
    guint8  data[3];
} PhMidiEvent;

/* Call once at startup with the engine sample rate / max JACK block. */
void          pluginhost_init(double sample_rate, int max_block);

/* One-time GUI-toolkit init for native plugin editors (calls suil_init for LV2).
 * Must be called once after gtk_init() and before any plugin editor is built. */
void          pluginhost_ui_init(int *argc, char ***argv);
void          pluginhost_shutdown(void);

/* Catalog (scans lazily on first call). GList of PluginInfo* (borrowed). */
const GList  *pluginhost_catalog(void);
void          pluginhost_rescan(void);

/* Rescan, then report plugins newly discovered since the previous call (or
 * since the last run — the baseline persists in ~/.jackdaw/pluginindex).
 * Returns a newly-allocated GList of g_strdup'd display names; free with
 * g_list_free_full(list, g_free). Empty list means nothing new. The first run
 * has no baseline, so it reports every plugin found. */
GList        *pluginhost_scan_report_new(void);

/* Optional progress callback, fired with each plugin path as it is scanned
 * out-of-process (used to drive a scan-progress dialog). */
void          pluginhost_set_scan_progress(void (*cb)(const char *plugin,
                                                      void *user), void *user);

/* `jackdaw --scan-plugin <FMT> <path>`: load+describe one plugin in this
 * throwaway process and print its metadata. Call at the very top of main()
 * (before GTK/locale init); exit with the returned code if argv matches. */
int           pluginhost_scan_helper_main(int argc, char **argv);

/* Extra user search directories (persisted by the caller via settings). */
void          pluginhost_add_search_path(PluginFormat fmt, const char *dir);
void          pluginhost_remove_search_path(PluginFormat fmt, const char *dir);
const GList  *pluginhost_search_paths(PluginFormat fmt);  /* GList of char* */
const char   *pluginhost_format_name(PluginFormat fmt);

/* Persistence of user search paths via settings.{h,c}. */
void          pluginhost_load_paths_from_settings(void);
void          pluginhost_save_paths_to_settings(void);

/* Lifecycle (main thread). */
PluginInstance *pluginhost_instantiate(const PluginInfo *info);
void            pluginhost_free(PluginInstance *inst);

/* RT processing — in-place stereo. Does nothing when bypassed. */
void            pluginhost_process(PluginInstance *inst,
                                   float *L, float *R, int nframes);

/* RT processing for an INSTRUMENT: deliver this block's MIDI events then render
 * audio into L/R (which the caller has pre-filled, typically with silence).
 * Falls back to plain audio process if the backend has no MIDI path. */
void            pluginhost_process_midi(PluginInstance *inst,
                                        const PhMidiEvent *ev, int n_ev,
                                        float *L, float *R, int nframes);

/* Clear the plugin's internal DSP state — reverb/delay buffers and any held
 * synth voices — without touching parameters. Used after an offline render so
 * resuming the live engine doesn't flush the render's leftover tail (an audible
 * pop). NOT RT-safe: call only when the live RT thread is not processing this
 * instance (e.g. while the engine is render-suspended). No-op if the backend
 * offers no reset. */
void            pluginhost_reset(PluginInstance *inst);

/* Opaque full-state save/restore for project save/reload. Captures the plug-in's
 * own state chunk — everything the generic parameter list misses (loaded IR/NAM
 * sample paths, internal modes, native-editor state) for VST2/VST3/CLAP. LV2 and
 * LADSPA expose their full state as parameters, so these return FALSE for them.
 * pluginhost_state_save: TRUE + a newly g_malloc'd blob in *out (caller g_free's)
 * of *out_len bytes. pluginhost_state_load: apply such a blob (also syncs the
 * native editor where applicable, so a reopened GUI reflects the restored state).
 * Both main-thread only; safe before the instance is added to an RT chain. */
gboolean        pluginhost_state_save(PluginInstance *inst,
                                      void **out, gsize *out_len);
gboolean        pluginhost_state_load(PluginInstance *inst,
                                      const void *data, gsize len);

/* --- Diagnostics (JACKDAW_DIAG) --- */
/* Mark the calling thread as inside/outside the JACK RT process callback, so the
 * VST3 host context can detect plugins that allocate messages on the RT thread. */
void            ph_rt_mark(int on);
int             ph_rt_active(void);
/* Read-and-reset the worst-case µs spent in this plugin's process() since the
 * last call. */
gint64          pluginhost_diag_take_max_us(PluginInstance *inst);
/* Count of IHostApplication::createInstance calls made on the RT thread by VST3
 * plugins (message/attribute-list allocations in the audio callback). */
guint64         ph_vst3_rt_alloc_count(void);

/* TRUE if this plugin is an instrument (synth) rather than an audio effect. */
gboolean        pluginhost_is_instrument(PluginInstance *inst);

/* Identity for project save/reload (recreate via a PluginInfo + instantiate). */
PluginFormat    pluginhost_format(PluginInstance *inst);
const char     *pluginhost_key(PluginInstance *inst);
const char     *pluginhost_category(PluginInstance *inst);

/* Publish transport state for plugins that query host time (e.g. VST2
 * audioMasterGetTime). Called from the RT thread at the top of each block. */
void            pluginhost_set_transport(double bpm, double sr,
                                         gint64 frame, gboolean playing);

/* Bypass (atomic; read in the RT thread). */
void            pluginhost_set_active(PluginInstance *inst, gboolean on);
gboolean        pluginhost_is_active(PluginInstance *inst);

/* Wet/dry mix in [0,1]: 0 = fully dry (effect inaudible), 1 = fully wet.
 * Atomic; read in the RT thread. Defaults to 1.0 (fully wet). */
void            pluginhost_set_mix(PluginInstance *inst, float mix);
float           pluginhost_get_mix(PluginInstance *inst);

const char     *pluginhost_name(PluginInstance *inst);

/* GUI: a GtkWidget to embed in the FX window — the plugin's native editor when
 * available, otherwise an auto-generated parameter panel. Never NULL. */
GtkWidget      *pluginhost_make_gui(PluginInstance *inst);

/* The cached editor widget if one was already built, else NULL (never builds). */
GtkWidget      *pluginhost_peek_gui(PluginInstance *inst);

/* --- Out-of-process native UI support (used by lv2ui_bridge) --- */
/* Returns TRUE + the plugin/UI URIs and UI type if a wrappable native editor
 * exists (LV2 only for now); the bridge spawns the matching helper process. */
gboolean        pluginhost_ui_meta(PluginInstance *inst, const char **plugin_uri,
                                   const char **ui_uri, const char **ui_type);
/* Raw control-port value access by LV2 port index (UI <-> DSP bridging). */
void            pluginhost_ctl_set(PluginInstance *inst, guint port, float v);
float           pluginhost_ctl_get(PluginInstance *inst, guint port);
/* Enumerate control input (outputs=FALSE) or output (TRUE) port indices. */
void            pluginhost_ctl_ports(PluginInstance *inst, gboolean outputs,
                                     const guint **ports, guint *n);
double          pluginhost_sample_rate(PluginInstance *inst);

/* Tear down the cached editor (frees a native UI / destroys the generic panel)
 * so the next pluginhost_make_gui() builds it fresh. The caller MUST have
 * removed the widget from any container first. DSP state is unaffected. */
void            pluginhost_release_gui(PluginInstance *inst);

/* Generic parameter access (used by the fallback parameter panel). */
guint           pluginhost_param_count(PluginInstance *inst);
const char     *pluginhost_param_name (PluginInstance *inst, guint i);
float           pluginhost_param_get  (PluginInstance *inst, guint i);
void            pluginhost_param_set  (PluginInstance *inst, guint i, float v);
void            pluginhost_param_range(PluginInstance *inst, guint i,
                                       float *min, float *max);

G_END_DECLS

#endif /* PLUGINHOST_H_INCLUDED */
