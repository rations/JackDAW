/* lv2ui_helper.c — out-of-process LV2 UI host for a toolkit that can't run in
 * JackDAW's GTK3 process (GtkUI/GTK2). Built per toolkit with HELPER_CONTAINER_URI
 * set (e.g. jackdaw-lv2ui-gtk2 -> GtkUI). Loads ONE plugin UI via suil, embeds it
 * in a GtkPlug, and reports the plug's X11 window id to JackDAW (which adopts it
 * with gtk_socket_add_id). Control ports are forwarded over stdio (lv2ui_ipc.h).
 *
 * argv: <binary> <plugin_uri> <ui_uri> <sample_rate>
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION >= 3
#  include <gtk/gtkx.h>          /* GtkPlug/GtkSocket (GTK2 has them in gtk.h) */
#endif
#include <gdk/gdkx.h>
#include <lilv/lilv.h>
#include <suil/suil.h>
#include <lv2/urid/urid.h>
#include <lv2/ui/ui.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/atom/atom.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <glib/gprintf.h>
#include "lv2ui_ipc.h"

#ifndef HELPER_CONTAINER_URI
#define HELPER_CONTAINER_URI "http://lv2plug.in/ns/extensions/ui#GtkUI"
#endif

static GHashTable *urid_table;
static GPtrArray  *urid_rev;
static guint32     urid_next = 1;
static LV2_URID urid_map_cb(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    if (!urid_table) {
        urid_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        urid_rev   = g_ptr_array_new();
        g_ptr_array_add(urid_rev, (gpointer)"");
    }
    gpointer v = g_hash_table_lookup(urid_table, uri);
    if (v) return GPOINTER_TO_UINT(v);
    guint32 id = urid_next++;
    char *dup = g_strdup(uri);
    g_hash_table_insert(urid_table, dup, GUINT_TO_POINTER(id));
    g_ptr_array_add(urid_rev, dup);
    return id;
}
static const char *urid_unmap_cb(LV2_URID_Unmap_Handle h, LV2_URID urid)
{ (void)h; return (urid_rev && urid < urid_rev->len) ? g_ptr_array_index(urid_rev, urid) : NULL; }
static LV2_URID_Map   urid_map   = { NULL, urid_map_cb };
static LV2_URID_Unmap urid_unmap = { NULL, urid_unmap_cb };

static int log_vprintf_cb(LV2_Log_Handle h, LV2_URID t, const char *fmt, va_list ap)
{ (void)h; (void)t; return g_vfprintf(stderr, fmt, ap); }
static int log_printf_cb(LV2_Log_Handle h, LV2_URID t, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = log_vprintf_cb(h, t, fmt, ap); va_end(ap); return r; }
static LV2_Log_Log lv2_log = { NULL, log_printf_cb, log_vprintf_cb };

static const LilvPlugin *g_plugin;
static SuilInstance     *g_ui;
static GtkWidget        *g_plug;
static FILE             *g_proto;   /* private protocol channel (not stdout) */

static void ui_write_cb(SuilController c, uint32_t port, uint32_t size,
                        uint32_t protocol, const void *buffer)
{
    (void)c;
    if (protocol != 0 || size != sizeof(float)) return;
    char line[LV2UI_IPC_MAXLINE];
    lv2ui_ipc_fmt_port(line, sizeof line, port, *(const float *)buffer);
    fputs(line, g_proto);
    fflush(g_proto);
}

static uint32_t ui_index_cb(SuilController c, const char *symbol)
{
    (void)c;
    if (!g_plugin) return LV2UI_INVALID_PORT_INDEX;
    uint32_t n = lilv_plugin_get_num_ports(g_plugin);
    for (uint32_t i = 0; i < n; i++) {
        const LilvPort *p = lilv_plugin_get_port_by_index(g_plugin, i);
        const LilvNode *s = lilv_port_get_symbol(g_plugin, p);
        if (s && !g_strcmp0(lilv_node_as_string(s), symbol)) return i;
    }
    return LV2UI_INVALID_PORT_INDEX;
}

static gboolean stdin_cb(GIOChannel *src, GIOCondition cond, gpointer d)
{
    (void)d;
    if (cond & (G_IO_HUP | G_IO_ERR)) { gtk_main_quit(); return G_SOURCE_REMOVE; }
    char *line = NULL; gsize len = 0;
    GIOStatus st = g_io_channel_read_line(src, &line, &len, NULL, NULL);
    if (st == G_IO_STATUS_EOF) { gtk_main_quit(); return G_SOURCE_REMOVE; }
    if (st != G_IO_STATUS_NORMAL || !line) return G_SOURCE_CONTINUE;
    g_strchomp(line);
    if (!strcmp(line, "QUIT")) { g_free(line); gtk_main_quit(); return G_SOURCE_REMOVE; }
    guint32 idx; float val;
    if (g_ui && lv2ui_ipc_parse_port(line, &idx, &val))
        suil_instance_port_event(g_ui, idx, sizeof(float), 0, &val);
    g_free(line);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    /* Keep the protocol clean: plugin UIs printf to stdout, so move the real
     * stdout aside for the protocol and point fd 1 at stderr. */
    int proto_fd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    g_proto = fdopen(proto_fd, "w");
    setvbuf(g_proto, NULL, _IOLBF, 0);

    gtk_init(&argc, &argv);
    suil_init(&argc, &argv, SUIL_ARG_NONE);
    if (argc < 4) { g_printerr("usage: %s plugin_uri ui_uri sample_rate\n", argv[0]); return 2; }
    const char *plugin_uri = argv[1];
    const char *want_ui    = argv[2];
    double sr = g_ascii_strtod(argv[3], NULL); if (sr <= 0) sr = 48000.0;

    LilvWorld *world = lilv_world_new();
    lilv_world_load_all(world);
    LilvNode *puri = lilv_new_uri(world, plugin_uri);
    g_plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), puri);
    lilv_node_free(puri);
    if (!g_plugin) { g_printerr("lv2ui: plugin not found %s\n", plugin_uri); return 3; }

    LilvUIs *uis = lilv_plugin_get_uis(g_plugin);
    const LilvUI *use_ui = NULL;
    if (uis) LILV_FOREACH (uis, i, uis) {
        const LilvUI *ui = lilv_uis_get(uis, i);
        if (!g_strcmp0(lilv_node_as_uri(lilv_ui_get_uri(ui)), want_ui)) { use_ui = ui; break; }
    }
    if (!use_ui) { g_printerr("lv2ui: UI not found %s\n", want_ui); return 4; }

    LilvNode *container = lilv_new_uri(world, HELPER_CONTAINER_URI);
    const LilvNode *ui_type = NULL;
    if (!lilv_ui_is_supported(use_ui, suil_ui_supported, container, &ui_type)) {
        g_printerr("lv2ui: UI not wrappable into %s\n", HELPER_CONTAINER_URI);
        return 5;
    }

    char *bundle = lilv_node_get_path(lilv_ui_get_bundle_uri(use_ui), NULL);
    char *binary = lilv_node_get_path(lilv_ui_get_binary_uri(use_ui), NULL);

    float f_sr = (float)sr, f_rate = 30.0f;
    LV2_Options_Option opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, urid_map_cb(NULL, LV2_PARAMETERS__sampleRate),
          sizeof(float), urid_map_cb(NULL, LV2_ATOM__Float), &f_sr },
        { LV2_OPTIONS_INSTANCE, 0, urid_map_cb(NULL, LV2_UI__updateRate),
          sizeof(float), urid_map_cb(NULL, LV2_ATOM__Float), &f_rate },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
    };
    LV2_Feature f_map  = { LV2_URID__map,   &urid_map };
    LV2_Feature f_unm  = { LV2_URID__unmap, &urid_unmap };
    LV2_Feature f_log  = { LV2_LOG__log,    &lv2_log };
    LV2_Feature f_opt  = { LV2_OPTIONS__options, opts };
    LV2_Feature f_idle = { LV2_UI__idleInterface, NULL };
    const LV2_Feature *features[] = { &f_map, &f_unm, &f_log, &f_opt, &f_idle, NULL };

    SuilHost *host = suil_host_new(ui_write_cb, ui_index_cb, NULL, NULL);
    g_ui = suil_instance_new(host, NULL, HELPER_CONTAINER_URI,
                             plugin_uri, lilv_node_as_uri(lilv_ui_get_uri(use_ui)),
                             lilv_node_as_uri(ui_type),
                             bundle ? bundle : "", binary ? binary : "", features);
    lilv_free(bundle); lilv_free(binary); lilv_node_free(container);
    if (!g_ui) { g_printerr("lv2ui: suil_instance_new failed\n"); return 6; }

    GtkWidget *w = (GtkWidget *)suil_instance_get_widget(g_ui);
    if (!w) { g_printerr("lv2ui: no widget\n"); return 7; }

    g_plug = gtk_plug_new(0);
    gtk_container_add(GTK_CONTAINER(g_plug), w);
    gtk_widget_show_all(g_plug);

    GtkRequisition req;
#if GTK_MAJOR_VERSION >= 3
    gtk_widget_get_preferred_size(w, NULL, &req);
#else
    gtk_widget_size_request(w, &req);
#endif
    fprintf(g_proto, "WID %lu %d %d\n",
            (unsigned long)gtk_plug_get_id(GTK_PLUG(g_plug)),
            req.width > 0 ? req.width : 320, req.height > 0 ? req.height : 240);
    fflush(g_proto);

    GIOChannel *in = g_io_channel_unix_new(0);
    g_io_channel_set_encoding(in, NULL, NULL);
    g_io_channel_set_flags(in, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_cb, NULL);

    gtk_main();
    if (g_ui) { suil_instance_free(g_ui); g_ui = NULL; }
    return 0;
}
