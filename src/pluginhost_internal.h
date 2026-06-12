#ifndef PLUGINHOST_INTERNAL_H_INCLUDED
#define PLUGINHOST_INTERNAL_H_INCLUDED

#include "pluginhost.h"

G_BEGIN_DECLS

/* Backend vtable. Every backend fills one of these on a PluginInstance. */
typedef struct {
    void        (*process)    (PluginInstance *, float *L, float *R, int n);
    void        (*destroy)    (PluginInstance *);
    GtkWidget  *(*make_gui)   (PluginInstance *);   /* NULL = use generic panel */
    void        (*destroy_gui)(PluginInstance *);   /* tear down native editor */
    guint       (*param_count)(PluginInstance *);
    const char *(*param_name) (PluginInstance *, guint);
    float       (*param_get)  (PluginInstance *, guint);
    void        (*param_set)  (PluginInstance *, guint, float);
    void        (*param_range)(PluginInstance *, guint, float *, float *);
} PhOps;

struct PluginInstance {
    PluginFormat  format;
    char         *name;
    volatile gint active;       /* 1 = processing, 0 = bypassed */
    volatile gint mix_q15;      /* wet/dry: 0=fully dry .. 32768=fully wet */
    double        sample_rate;
    int           max_block;
    const PhOps  *ops;
    void         *backend;      /* backend-private state */

    /* Dry-signal scratch for the wet/dry mix (allocated to max_block). */
    float        *dry_L, *dry_R;

    /* Cached editor widget (owned by the instance, ref-sunk). gui_native means
     * the backend's destroy() frees the widget (e.g. suil); otherwise it is a
     * generic panel owned by the host. */
    GtkWidget    *gui;
    gboolean      gui_native;
};

/* Allocate a blank instance for a backend to populate. */
PluginInstance *ph_instance_alloc(PluginFormat fmt, const char *name,
                                  double sr, int max_block);

/* Append PluginInfo* entries to *catalog. extra = GList of char* dirs.
 * For the dlopen formats (vst2/vst3/clap/ladspa) these only ENUMERATE files and
 * route each through ph_scan_cached() — they never load plugin code in-process.
 * LV2 reads .ttl text via lilv (no code load) so it scans fully in-process. */
void ph_lv2_scan   (GList **catalog, const GList *extra);
void ph_vst2_scan  (GList **catalog, const GList *extra);
void ph_vst3_scan  (GList **catalog, const GList *extra);
void ph_clap_scan  (GList **catalog, const GList *extra);
void ph_ladspa_scan(GList **catalog, const GList *extra);

/* Per-file describe: LOAD one plugin file and append its PluginInfo* classes to
 * *out. Runs ONLY in the out-of-process scanner (jackdaw --scan-plugin), never
 * in the main process, so Wine/yabridge code stays isolated. */
void ph_vst2_describe  (const char *path, GList **out);
void ph_vst3_describe  (const char *path, GList **out);
void ph_clap_describe  (const char *path, GList **out);
void ph_ladspa_describe(const char *path, GList **out);

/* Shared out-of-process scan + on-disk cache (in pluginhost.c). A dlopen
 * backend's directory walk calls this per plugin file: on a cache miss (by
 * path+mtime) it spawns `jackdaw --scan-plugin <fmt> <path>`, reads the
 * metadata, caches it, and appends PluginInfo* to *catalog. */
void ph_scan_cached(PluginFormat fmt, const char *path, GList **catalog);

/* One-time native-UI toolkit init. No-op now that UIs are out-of-process. */
void ph_lv2_ui_init(int *argc, char ***argv);

/* Out-of-process UI support (consumed by lv2ui_bridge via pluginhost.c).
 * ph_lv2_ui_meta returns FALSE if the plugin has no UI we have a helper for. */
gboolean ph_lv2_ui_meta(PluginInstance *, const char **plugin_uri,
                        const char **ui_uri, const char **ui_type);
void  ph_lv2_ctl_set  (PluginInstance *, guint port, float v);
float ph_lv2_ctl_get  (PluginInstance *, guint port);
void  ph_lv2_ctl_ports(PluginInstance *, gboolean outputs,
                       const guint **ports, guint *n);

/* Instantiate (return NULL on failure). */
PluginInstance *ph_lv2_instantiate   (const PluginInfo *, double sr, int max_block);
PluginInstance *ph_vst2_instantiate  (const PluginInfo *, double sr, int max_block);
PluginInstance *ph_vst3_instantiate  (const PluginInfo *, double sr, int max_block);
PluginInstance *ph_clap_instantiate  (const PluginInfo *, double sr, int max_block);
PluginInstance *ph_ladspa_instantiate(const PluginInfo *, double sr, int max_block);

/* Helpers shared by backends. */
PluginInfo *ph_info_new(PluginFormat fmt, const char *key,
                        const char *name, const char *category);

/* Path validation before dlopen: absolute, no "..", no NUL. (CLAUDE.md) */
gboolean ph_path_is_safe(const char *path);

/* Build a generic parameter panel GtkWidget from the instance's param API. */
GtkWidget *ph_generic_param_panel(PluginInstance *inst);

G_END_DECLS

#endif /* PLUGINHOST_INTERNAL_H_INCLUDED */
