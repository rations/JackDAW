/* lv2ui_ipc.h — line protocol between JackDAW and the out-of-process LV2 UI
 * helper binaries (used ONLY for toolkits that can't run in JackDAW's GTK3
 * process: GtkUI/GTK2, Qt). X11/Gtk3 UIs are hosted in-process via suil.
 *
 * Messages are newline-terminated ASCII on the helper's stdin/stdout pipes
 * (g_spawn_async_with_pipes). The helper redirects the plugin's stdout to stderr
 * and keeps a private dup of the real stdout for the protocol, so plugin chatter
 * cannot corrupt it. Floats are locale-independent.
 *
 *   helper -> host (helper stdout): WID <xid> <w> <h> | SIZE <w> <h>
 *                                   | PORT <idx> <float> | ATOM <idx> <b64>
 *   host -> helper (helper stdin):  PORT <idx> <float> | ATOM <idx> <b64> | QUIT
 *
 * ATOM carries one atom:eventTransfer message (a model to load, patch:Set, the
 * plug-in's state, its replies). The atom cannot cross as bytes — its type and
 * key fields are URIDs, and a URID means nothing in a process that did not map
 * it — so each side serialises with sratom and the Turtle is base64'd to keep
 * one message on one line.
 */
#ifndef LV2UI_IPC_H_INCLUDED
#define LV2UI_IPC_H_INCLUDED

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#define LV2UI_IPC_MAXLINE 512

/* sratom hangs the atom off one RDF statement, and sratom_from_turtle looks
 * that statement up by subject and predicate — passing NULL for them crashes
 * inside sord rather than defaulting to anything. Both ends must serialise and
 * parse with the SAME three strings, so they live here with the protocol. */
#define LV2UI_IPC_ATOM_BASE    "file:///tmp/jackdaw/"
#define LV2UI_IPC_ATOM_SUBJECT "urn:jackdaw:atom"
#define LV2UI_IPC_ATOM_PRED    "urn:jackdaw:value"

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

/* Returns a newly allocated "ATOM <idx> <base64 turtle>\n" line (g_free). */
static inline char *
lv2ui_ipc_fmt_atom(guint32 idx, const char *turtle)
{
    gchar *b64  = g_base64_encode((const guchar *)turtle, strlen(turtle));
    gchar *line = g_strdup_printf("ATOM %u %s\n", idx, b64);
    g_free(b64);
    return line;
}

/* Parses an ATOM line; returns its Turtle text (g_free) or NULL. */
static inline char *
lv2ui_ipc_parse_atom(const char *line, guint32 *idx)
{
    if (strncmp(line, "ATOM ", 5) != 0) return NULL;
    const char *p = line + 5;
    char *end = NULL;
    unsigned long i = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return NULL;
    gsize len = 0;
    guchar *raw = g_base64_decode(end + 1, &len);
    if (!raw) return NULL;
    char *ttl = g_strndup((const char *)raw, len);   /* NUL-terminate for serd */
    g_free(raw);
    *idx = (guint32)i;
    return ttl;
}

#endif /* LV2UI_IPC_H_INCLUDED */
