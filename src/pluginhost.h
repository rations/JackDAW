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
} PluginInfo;

typedef struct PluginInstance PluginInstance;

/* Call once at startup with the engine sample rate / max JACK block. */
void          pluginhost_init(double sample_rate, int max_block);

/* One-time GUI-toolkit init for native plugin editors (calls suil_init for LV2).
 * Must be called once after gtk_init() and before any plugin editor is built. */
void          pluginhost_ui_init(int *argc, char ***argv);
void          pluginhost_shutdown(void);

/* Catalog (scans lazily on first call). GList of PluginInfo* (borrowed). */
const GList  *pluginhost_catalog(void);
void          pluginhost_rescan(void);

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

/* Generic parameter access (used by the fallback parameter panel). */
guint           pluginhost_param_count(PluginInstance *inst);
const char     *pluginhost_param_name (PluginInstance *inst, guint i);
float           pluginhost_param_get  (PluginInstance *inst, guint i);
void            pluginhost_param_set  (PluginInstance *inst, guint i, float v);
void            pluginhost_param_range(PluginInstance *inst, guint i,
                                       float *min, float *max);

G_END_DECLS

#endif /* PLUGINHOST_H_INCLUDED */
