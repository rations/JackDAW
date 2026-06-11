/* lv2ui_bridge.c — see lv2ui_bridge.h. */
#define _GNU_SOURCE
#include "lv2ui_bridge.h"
#include "lv2ui_ipc.h"

#include <gtk/gtkx.h>          /* GtkSocket */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

typedef struct {
    PluginInstance *inst;
    GtkWidget      *socket;
    GPid            pid;
    int             in_fd;      /* -> helper stdin  */
    GIOChannel     *out_ch;     /* <- helper stdout */
    guint           out_watch;
    guint           child_watch;
    guint           push_timer;
    gboolean        embedded;
} Bridge;

static char *find_helper(const char *name)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *dir  = g_path_get_dirname(exe);
        char *cand = g_build_filename(dir, name, NULL);
        g_free(dir);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) return cand;
        g_free(cand);
    }
    return g_find_program_in_path(name);
}

static const char *helper_for_ui_type(const char *ui_type)
{
    if (ui_type && strstr(ui_type, "#GtkUI")) return "jackdaw-lv2ui-gtk2";
    if (ui_type && strstr(ui_type, "#Qt5UI")) return "jackdaw-lv2ui-qt5";
    if (ui_type && strstr(ui_type, "#Qt6UI")) return "jackdaw-lv2ui-qt6";
    return NULL;   /* X11/Gtk3 are hosted in-process, not here */
}

static void bridge_send(Bridge *b, const char *line)
{
    if (b->in_fd < 0) return;
    gsize len = strlen(line); ssize_t off = 0;
    while ((gsize)off < len) {
        ssize_t w = write(b->in_fd, line + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += w;
    }
}

static void push_ports(Bridge *b, gboolean outputs)
{
    const guint *ports = NULL; guint n = 0;
    pluginhost_ctl_ports(b->inst, outputs, &ports, &n);
    for (guint i = 0; i < n; i++) {
        char line[LV2UI_IPC_MAXLINE];
        lv2ui_ipc_fmt_port(line, sizeof line, ports[i],
                           pluginhost_ctl_get(b->inst, ports[i]));
        bridge_send(b, line);
    }
}

static gboolean push_outputs(gpointer data)
{
    Bridge *b = data;
    if (b->embedded) push_ports(b, TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean on_helper_out(GIOChannel *src, GIOCondition cond, gpointer data)
{
    Bridge *b = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) { b->out_watch = 0; return G_SOURCE_REMOVE; }
    char *line = NULL; gsize len = 0;
    GIOStatus st = g_io_channel_read_line(src, &line, &len, NULL, NULL);
    if (st == G_IO_STATUS_EOF) { b->out_watch = 0; return G_SOURCE_REMOVE; }
    if (st != G_IO_STATUS_NORMAL || !line) return G_SOURCE_CONTINUE;
    g_strchomp(line);

    if (!strncmp(line, "WID ", 4)) {
        unsigned long xid = 0; int w = 0, h = 0;
        sscanf(line + 4, "%lu %d %d", &xid, &w, &h);
        if (xid && GTK_IS_SOCKET(b->socket)) {
            /* Give the socket the plugin's natural size so the non-homogeneous
             * stack and the FX window can fit themselves to it. */
            if (w > 0 && h > 0) gtk_widget_set_size_request(b->socket, w, h);
            gtk_socket_add_id(GTK_SOCKET(b->socket), (Window)xid);
            b->embedded = TRUE;
            push_ports(b, FALSE);   /* seed control inputs once */
        }
    } else {
        guint32 idx; float val;
        if (lv2ui_ipc_parse_port(line, &idx, &val))
            pluginhost_ctl_set(b->inst, idx, val);   /* UI -> DSP */
    }
    g_free(line);
    return G_SOURCE_CONTINUE;
}

static void on_child_exit(GPid pid, gint status, gpointer data)
{
    Bridge *b = data; (void)status;
    g_spawn_close_pid(pid); b->pid = 0; b->child_watch = 0;
}

static void bridge_free(gpointer data, GObject *where)
{
    (void)where;
    Bridge *b = data;
    if (b->push_timer)  g_source_remove(b->push_timer);
    if (b->out_watch)   g_source_remove(b->out_watch);
    if (b->in_fd >= 0)  { bridge_send(b, "QUIT\n"); close(b->in_fd); b->in_fd = -1; }
    if (b->out_ch)      { g_io_channel_shutdown(b->out_ch, FALSE, NULL);
                          g_io_channel_unref(b->out_ch); b->out_ch = NULL; }
    if (b->child_watch) g_source_remove(b->child_watch);
    if (b->pid)         { kill(b->pid, SIGTERM); g_spawn_close_pid(b->pid); }
    g_free(b);
}

GtkWidget *lv2ui_bridge_new(PluginInstance *inst)
{
    const char *plugin_uri = NULL, *ui_uri = NULL, *ui_type = NULL;
    if (!pluginhost_ui_meta(inst, &plugin_uri, &ui_uri, &ui_type))
        return NULL;
    const char *helper_name = helper_for_ui_type(ui_type);
    if (!helper_name) return NULL;            /* in-process toolkit, not ours */
    char *helper = find_helper(helper_name);
    if (!helper) return NULL;                 /* helper not built/installed */

    char sr[32];
    g_ascii_dtostr(sr, sizeof sr, pluginhost_sample_rate(inst));
    char *argv[] = { helper, (char *)plugin_uri, (char *)ui_uri, sr, NULL };

    GPid pid = 0; int in_fd = -1, out_fd = -1; GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
        &pid, &in_fd, &out_fd, NULL, &err);
    g_free(helper);
    if (!ok) {
        g_warning("lv2ui: helper spawn failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return NULL;
    }

    Bridge *b = g_new0(Bridge, 1);
    b->inst   = inst;
    b->pid    = pid;
    b->in_fd  = in_fd;
    b->socket = gtk_socket_new();
    gtk_widget_set_hexpand(b->socket, TRUE);
    gtk_widget_set_vexpand(b->socket, TRUE);

    b->out_ch = g_io_channel_unix_new(out_fd);
    g_io_channel_set_encoding(b->out_ch, NULL, NULL);
    g_io_channel_set_flags(b->out_ch, G_IO_FLAG_NONBLOCK, NULL);
    b->out_watch   = g_io_add_watch(b->out_ch, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    on_helper_out, b);
    b->child_watch = g_child_watch_add(pid, on_child_exit, b);
    b->push_timer  = g_timeout_add(50, push_outputs, b);

    g_object_weak_ref(G_OBJECT(b->socket), bridge_free, b);
    return b->socket;
}
