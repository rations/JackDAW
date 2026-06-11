/* lv2ui_ipc.h — line protocol between JackDAW and the out-of-process LV2 UI
 * helper binaries (used ONLY for toolkits that can't run in JackDAW's GTK3
 * process: GtkUI/GTK2, Qt). X11/Gtk3 UIs are hosted in-process via suil.
 *
 * Messages are newline-terminated ASCII on the helper's stdin/stdout pipes
 * (g_spawn_async_with_pipes). The helper redirects the plugin's stdout to stderr
 * and keeps a private dup of the real stdout for the protocol, so plugin chatter
 * cannot corrupt it. Floats are locale-independent.
 *
 *   helper -> host (helper stdout): WID <xid> <w> <h> | PORT <idx> <float>
 *   host -> helper (helper stdin):  PORT <idx> <float> | QUIT
 */
#ifndef LV2UI_IPC_H_INCLUDED
#define LV2UI_IPC_H_INCLUDED

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#define LV2UI_IPC_MAXLINE 512

static inline void
lv2ui_ipc_fmt_port(char *buf, gsize buflen, guint32 idx, float value)
{
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(num, sizeof num, (double)value);
    g_snprintf(buf, buflen, "PORT %u %s\n", idx, num);
}

static inline gboolean
lv2ui_ipc_parse_port(const char *line, guint32 *idx, float *value)
{
    if (strncmp(line, "PORT ", 5) != 0) return FALSE;
    const char *p = line + 5;
    char *end = NULL;
    unsigned long i = strtoul(p, &end, 10);
    if (end == p) return FALSE;
    *idx   = (guint32)i;
    *value = (float)g_ascii_strtod(end, NULL);
    return TRUE;
}

#endif /* LV2UI_IPC_H_INCLUDED */
