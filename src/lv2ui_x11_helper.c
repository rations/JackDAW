/* lv2ui_x11_helper.c — out-of-process host for LV2 X11UI editors.
 *
 * Why this exists (and why it is pure Xlib, NOT GTK):
 *   Many X11UI plugins draw with cairo's "toy" font API
 *   (cairo_select_font_face/cairo_text_extents). That path shares libcairo's one
 *   process-global font-face cache. In a GTK/pango host (jackdaw, or even a GTK3
 *   helper) pango ALSO drives that cache, and the two consumers race over it:
 *   libcairo destroys a toy-font FcPattern that is still referenced -> a
 *   use-after-free *inside libcairo* (confirmed by ASan on gxtuner). Reaper is
 *   immune because its process has no GTK/pango at all — the plugin is the only
 *   cairo consumer. This helper reproduces that isolation: it links NO GTK and NO
 *   pango, only Xlib + glib (glib has no font/cairo dependency) + suil. suil
 *   instantiates an X11UI in an X11 host NATIVELY (no wrapper module, so no GTK is
 *   ever dlopened). The plugin therefore owns libcairo alone -> no UAF.
 *
 *   The editor window is exported to JackDAW as an XEmbed client (a plain Xlib
 *   window carrying _XEMBED_INFO) which the FX window's GtkSocket adopts.
 *
 * argv: <binary> <plugin_uri> <ui_uri> <sample_rate>
 * Protocol (lv2ui_ipc.h): stdout -> host: WID <xid> <w> <h> | SIZE <w> <h> |
 *                                          PORT <idx> <float>
 *                         stdin  <- host: PORT <idx> <float> | QUIT
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <glib.h>
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

#define X11_UI_URI "http://lv2plug.in/ns/extensions/ui#X11UI"

/* ---- URID map (private to this process) ---- */
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

/* ---- Globals ---- */
static const LilvPlugin            *g_plugin;
static SuilInstance                *g_ui;
static const LV2UI_Idle_Interface  *g_idle;
static LV2UI_Handle                 g_idle_handle;
static FILE                        *g_proto;   /* private protocol (not stdout) */
static Display                     *g_dpy;
static Window                       g_parent;  /* XEmbed client adopted by host  */
static GMainLoop                   *g_loop;

static void quit_now(void) { if (g_loop) g_main_loop_quit(g_loop); }

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

/* The plugin calls this (ui:resize feature) to request a new editor size. We
 * resize our XEmbed window and tell JackDAW to refit the socket. */
static int ui_resize_cb(LV2UI_Feature_Handle h, int w, int ht)
{
    (void)h;
    if (g_dpy && g_parent && w > 0 && ht > 0) {
        XResizeWindow(g_dpy, g_parent, (unsigned)w, (unsigned)ht);
        XFlush(g_dpy);
        fprintf(g_proto, "SIZE %d %d\n", w, ht);
        fflush(g_proto);
    }
    return 0;
}

/* stdin: control values from the DSP/host, or QUIT. */
static gboolean stdin_cb(GIOChannel *src, GIOCondition cond, gpointer d)
{
    (void)d;
    if (cond & (G_IO_HUP | G_IO_ERR)) { quit_now(); return G_SOURCE_REMOVE; }
    char *line = NULL; gsize len = 0;
    GIOStatus st = g_io_channel_read_line(src, &line, &len, NULL, NULL);
    if (st == G_IO_STATUS_EOF) { quit_now(); return G_SOURCE_REMOVE; }
    if (st != G_IO_STATUS_NORMAL || !line) return G_SOURCE_CONTINUE;
    g_strchomp(line);
    if (!strcmp(line, "QUIT")) { g_free(line); quit_now(); return G_SOURCE_REMOVE; }
    guint32 idx; float val;
    if (g_ui && lv2ui_ipc_parse_port(line, &idx, &val))
        suil_instance_port_event(g_ui, idx, sizeof(float), 0, &val);
    g_free(line);
    return G_SOURCE_CONTINUE;
}

/* Drain our own X connection (parent-window events only; the plugin pumps its
 * own connection inside idle()). */
static gboolean x11_cb(GIOChannel *src, GIOCondition cond, gpointer d)
{
    (void)src; (void)d;
    if (cond & (G_IO_HUP | G_IO_ERR)) { quit_now(); return G_SOURCE_REMOVE; }
    while (g_dpy && XPending(g_dpy)) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        /* Nothing required here yet: the host (GtkSocket) drives mapping via
         * _XEMBED_INFO, and the plugin owns its child window's drawing. We still
         * must drain the queue so the connection never blocks. */
    }
    return G_SOURCE_CONTINUE;
}

/* Drive the plugin's idle interface (it redraws / pumps its own X events here). */
static gboolean idle_cb(gpointer d)
{
    (void)d;
    if (g_idle && g_idle->idle && g_idle_handle)
        if (g_idle->idle(g_idle_handle)) { quit_now(); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

/* Mark our window as an XEmbed client so GtkSocket maps it after reparenting. */
static void set_xembed_info(Display *dpy, Window w)
{
    Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    unsigned long data[2] = { 0 /* version */, (1UL << 0) /* XEMBED_MAPPED */ };
    XChangeProperty(dpy, w, xembed_info, xembed_info, 32, PropModeReplace,
                    (unsigned char *)data, 2);
}

int main(int argc, char **argv)
{
    /* Keep the protocol clean: plugin UIs printf to stdout, so move the real
     * stdout aside for the protocol and point fd 1 at stderr. */
    int proto_fd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    g_proto = fdopen(proto_fd, "w");
    setvbuf(g_proto, NULL, _IOLBF, 0);

    if (argc < 4) { g_printerr("usage: %s plugin_uri ui_uri sample_rate\n", argv[0]); return 2; }
    const char *plugin_uri = argv[1];
    const char *want_ui    = argv[2];
    double sr = g_ascii_strtod(argv[3], NULL); if (sr <= 0) sr = 48000.0;

    XInitThreads();
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { g_printerr("lv2ui-x11: cannot open display\n"); return 3; }
    suil_init(&argc, &argv, SUIL_ARG_NONE);

    LilvWorld *world = lilv_world_new();
    lilv_world_load_all(world);
    LilvNode *puri = lilv_new_uri(world, plugin_uri);
    g_plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), puri);
    lilv_node_free(puri);
    if (!g_plugin) { g_printerr("lv2ui-x11: plugin not found %s\n", plugin_uri); return 4; }

    LilvUIs *uis = lilv_plugin_get_uis(g_plugin);
    const LilvUI *use_ui = NULL;
    if (uis) LILV_FOREACH (uis, i, uis) {
        const LilvUI *ui = lilv_uis_get(uis, i);
        if (!g_strcmp0(lilv_node_as_uri(lilv_ui_get_uri(ui)), want_ui)) { use_ui = ui; break; }
    }
    if (!use_ui) { g_printerr("lv2ui-x11: UI not found %s\n", want_ui); return 5; }

    /* Create the XEmbed client window that JackDAW's GtkSocket will adopt; the
     * plugin will create its own window as a child of this one. */
    int screen = DefaultScreen(g_dpy);
    g_parent = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen),
                                   0, 0, 320, 240, 0,
                                   BlackPixel(g_dpy, screen), BlackPixel(g_dpy, screen));
    XSelectInput(g_dpy, g_parent, StructureNotifyMask);
    set_xembed_info(g_dpy, g_parent);
    XFlush(g_dpy);

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
    LV2UI_Resize resize = { NULL, ui_resize_cb };
    LV2_Feature f_map    = { LV2_URID__map,   &urid_map };
    LV2_Feature f_unm    = { LV2_URID__unmap, &urid_unmap };
    LV2_Feature f_log    = { LV2_LOG__log,    &lv2_log };
    LV2_Feature f_opt    = { LV2_OPTIONS__options, opts };
    LV2_Feature f_idle   = { LV2_UI__idleInterface, NULL };
    LV2_Feature f_parent = { LV2_UI__parent, (void *)(uintptr_t)g_parent };
    LV2_Feature f_resize = { LV2_UI__resize, &resize };
    const LV2_Feature *features[] = {
        &f_map, &f_unm, &f_log, &f_opt, &f_idle, &f_parent, &f_resize, NULL };

    SuilHost *host = suil_host_new(ui_write_cb, ui_index_cb, NULL, NULL);
    /* container == ui type == X11UI -> suil instantiates natively (no wrapper,
     * so no GTK/Qt is ever loaded into this process). */
    g_ui = suil_instance_new(host, NULL, X11_UI_URI,
                             plugin_uri, lilv_node_as_uri(lilv_ui_get_uri(use_ui)),
                             X11_UI_URI,
                             bundle ? bundle : "", binary ? binary : "", features);
    lilv_free(bundle); lilv_free(binary);
    if (!g_ui) { g_printerr("lv2ui-x11: suil_instance_new failed\n"); return 6; }

    g_idle = (const LV2UI_Idle_Interface *)
             suil_instance_extension_data(g_ui, LV2_UI__idleInterface);
    g_idle_handle = suil_instance_get_handle(g_ui);

    /* The plugin created its window as a child of g_parent. Size our XEmbed
     * window to the plugin's natural size and report it to JackDAW. */
    Window child = (Window)(uintptr_t)suil_instance_get_widget(g_ui);
    int rw = 320, rh = 240;
    if (child) {
        Window root_r; int xx, yy; unsigned int cw, ch, bw, depth;
        if (XGetGeometry(g_dpy, child, &root_r, &xx, &yy, &cw, &ch, &bw, &depth)
            && cw > 1 && ch > 1) {
            rw = (int)cw; rh = (int)ch;
            XResizeWindow(g_dpy, g_parent, cw, ch);
        }
        XMapWindow(g_dpy, child);
    }
    XFlush(g_dpy);

    fprintf(g_proto, "WID %lu %d %d\n", (unsigned long)g_parent, rw, rh);
    fflush(g_proto);

    /* glib main loop: stdin protocol + our X connection + plugin idle. */
    GIOChannel *in = g_io_channel_unix_new(0);
    g_io_channel_set_encoding(in, NULL, NULL);
    g_io_channel_set_flags(in, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_cb, NULL);

    GIOChannel *xc = g_io_channel_unix_new(ConnectionNumber(g_dpy));
    g_io_add_watch(xc, G_IO_IN | G_IO_HUP | G_IO_ERR, x11_cb, NULL);

    g_timeout_add(33, idle_cb, NULL);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);

    if (g_ui) { suil_instance_free(g_ui); g_ui = NULL; }
    if (g_dpy) XCloseDisplay(g_dpy);
    return 0;
}
