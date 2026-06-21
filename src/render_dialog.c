#define _GNU_SOURCE
#include <config.h>

#include <string.h>
#include <glib/gstdio.h>

#include "render_dialog.h"
#include "render.h"
#include "jackdaw-engine.h"
#include "project.h"
#include "track.h"
#include "settings.h"
#include "message.h"

/* -----------------------------------------------------------------------
 * Progress popup — shared by offline and realtime renders.
 * ----------------------------------------------------------------------- */

typedef struct {
    GtkWidget             *dialog;
    GtkWidget             *bar;
    GtkWidget             *label;
    GtkWidget             *button;     /* Cancel while running, Close when done */
    JackDawRenderProgress  prog;
    RenderMethod           method;
    GThread               *worker;     /* offline only */
    guint                  timer;
    gboolean               done;
} ProgressUI;

static void progress_ui_finish(ProgressUI *ui)
{
    gboolean cancelled = g_atomic_int_get(&ui->prog.cancel) != 0;
    gboolean failed    = g_atomic_int_get(&ui->prog.failed) != 0;

    if (ui->worker) { g_thread_join(ui->worker); ui->worker = NULL; }

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->bar),
                                  (cancelled || failed) ? 0.0 : 1.0);
    if (cancelled)   gtk_label_set_text(GTK_LABEL(ui->label), "Cancelled");
    else if (failed) gtk_label_set_text(GTK_LABEL(ui->label),
                                        "Render failed — check the path/format");
    else             gtk_label_set_text(GTK_LABEL(ui->label), "Finished");

    gtk_button_set_label(GTK_BUTTON(ui->button), "Close");
    ui->done = TRUE;
}

static gboolean progress_tick(gpointer data)
{
    ProgressUI *ui = data;

    if (ui->method == RENDER_METHOD_REALTIME)
        jackdaw_render_realtime_poll(&ui->prog);

    off_t total = ui->prog.frames_total;
    off_t done  = ui->prog.frames_done;
    if (total > 0) {
        double frac = (double)done / (double)total;
        if (frac < 0.0) frac = 0.0; else if (frac > 1.0) frac = 1.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->bar), frac);
        if (!g_atomic_int_get(&ui->prog.finished)) {
            char buf[64];
            g_snprintf(buf, sizeof buf, "Rendering… %d%%", (int)(frac * 100.0));
            gtk_label_set_text(GTK_LABEL(ui->label), buf);
        }
    }

    if (g_atomic_int_get(&ui->prog.finished)) {
        ui->timer = 0;
        progress_ui_finish(ui);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Cancel (while running) or Close (when finished). */
static void progress_response(GtkDialog *dlg, gint response, gpointer data)
{
    (void)response;
    ProgressUI *ui = data;
    if (!ui->done) {
        /* Request cancel; wait for the worker/poll to actually finish. */
        g_atomic_int_set(&ui->prog.cancel, 1);
        if (ui->method == RENDER_METHOD_OFFLINE && ui->timer) {
            /* Let the tick observe finished + join the worker. */
            return;
        }
        return;
    }
    if (ui->timer) { g_source_remove(ui->timer); ui->timer = 0; }
    gtk_widget_destroy(GTK_WIDGET(dlg));
    g_free(ui);
}

/* Build + show the progress popup and start the chosen render. Takes ownership
 * of nothing; `o` is consumed by the render start calls (deep-copied there). */
static void start_render_with_progress(GtkWindow *parent, const RenderOptions *o)
{
    ProgressUI *ui = g_new0(ProgressUI, 1);
    ui->method = o->method;

    ui->dialog = gtk_dialog_new_with_buttons(
        "Rendering", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(ui->dialog), 360, 110);
    ui->button = gtk_dialog_get_widget_for_response(GTK_DIALOG(ui->dialog),
                                                    GTK_RESPONSE_CANCEL);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(ui->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_set_spacing(GTK_BOX(box), 8);

    gchar *base = g_path_get_basename(o->out_path);
    gchar *msg  = g_strdup_printf("Rendering to %s", base);
    ui->label = gtk_label_new(msg);
    gtk_label_set_ellipsize(GTK_LABEL(ui->label), PANGO_ELLIPSIZE_MIDDLE);
    g_free(msg); g_free(base);
    gtk_box_pack_start(GTK_BOX(box), ui->label, FALSE, FALSE, 0);

    ui->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), ui->bar, FALSE, FALSE, 0);

    g_signal_connect(ui->dialog, "response",
                     G_CALLBACK(progress_response), ui);

    gtk_widget_show_all(ui->dialog);

    gboolean started_ok = TRUE;
    if (o->method == RENDER_METHOD_OFFLINE) {
        ui->worker = jackdaw_render_offline_start(o, &ui->prog);
        started_ok = (ui->worker != NULL);
    } else {
        started_ok = (jackdaw_render_realtime_start(o, &ui->prog) == FALSE);
        if (!started_ok) {
            g_atomic_int_set(&ui->prog.failed, 1);
            g_atomic_int_set(&ui->prog.finished, 1);
        }
    }

    ui->timer = g_timeout_add(100, progress_tick, ui);
    (void)started_ok;
}

/* -----------------------------------------------------------------------
 * Options dialog
 * ----------------------------------------------------------------------- */

typedef struct {
    JackDawProject *project;
    GtkWidget      *dialog;
    GtkWidget      *combo_format;     /* indices map via fmt_map[] */
    RenderFormat    fmt_map[3];
    int             fmt_count;
    GtkWidget      *combo_bits;
    GtkWidget      *rb_src_master, *rb_src_selected;
    GtkWidget      *rb_scope_project, *rb_scope_region;
    GtkWidget      *combo_sr;
    GtkWidget      *rb_ch_stereo, *rb_ch_mono;
    GtkWidget      *rb_m_offline, *rb_m_realtime;
    GtkWidget      *path_entry;
} OptUI;

static const int  sr_values[]  = { 44100, 48000, 88200, 96000 };
static const int  sr_n         = 4;

static RenderFormat opt_format(OptUI *u)
{
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(u->combo_format));
    if (idx < 0 || idx >= u->fmt_count) idx = 0;
    return u->fmt_map[idx];
}

static gchar *unique_out_path(const gchar *path);

/* Replace the extension of the path entry to match the selected format. */
static void opt_sync_extension(OptUI *u)
{
    const char *cur = gtk_entry_get_text(GTK_ENTRY(u->path_entry));
    if (!cur || !*cur) return;
    const char *ext = jackdaw_render_extension(opt_format(u));
    gchar *dir  = g_path_get_dirname(cur);
    gchar *base = g_path_get_basename(cur);
    char  *dot  = strrchr(base, '.');
    if (dot) *dot = '\0';
    /* Strip any existing " (N)" disambiguator before re-uniquifying so toggling
     * formats back and forth doesn't accumulate counters. */
    char *p = base + strlen(base);
    if (p > base && p[-1] == ')') {
        char *open = strrchr(base, '(');
        if (open && open > base && open[-1] == ' ') {
            gboolean digits = (open[1] != ')');
            for (char *q = open + 1; *q && *q != ')'; q++)
                if (*q < '0' || *q > '9') { digits = FALSE; break; }
            if (digits) open[-1] = '\0';   /* drop " (N)" */
        }
    }
    gchar *name = g_strdup_printf("%s.%s", base, ext);
    gchar *full = g_build_filename(dir, name, NULL);
    gchar *uniq = unique_out_path(full);
    gtk_entry_set_text(GTK_ENTRY(u->path_entry), uniq);
    g_free(dir); g_free(base); g_free(name); g_free(full); g_free(uniq);
}

static void on_format_changed(GtkComboBox *c, gpointer data)
{
    (void)c;
    OptUI *u = data;
    gtk_widget_set_sensitive(u->combo_bits, opt_format(u) == RENDER_FMT_WAV);
    opt_sync_extension(u);
}

static void on_browse_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    OptUI *u = data;
    GtkWidget *fc = gtk_file_chooser_dialog_new(
        "Render to file", GTK_WINDOW(u->dialog),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);

    const char *cur = gtk_entry_get_text(GTK_ENTRY(u->path_entry));
    if (cur && *cur) {
        gchar *dir  = g_path_get_dirname(cur);
        gchar *base = g_path_get_basename(cur);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), base);
        g_free(dir); g_free(base);
    }
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path) { gtk_entry_set_text(GTK_ENTRY(u->path_entry), path); g_free(path); }
    }
    gtk_widget_destroy(fc);
}

/* Compute the default output path: project dir + project name + ext, falling
 * back to the last-used render dir / projects dir + "render". */
static gchar *default_out_path(JackDawProject *p, RenderFormat fmt)
{
    const char *ext = jackdaw_render_extension(fmt);
    gchar *dir = NULL, *stem = NULL;

    const gchar *pf = jackdaw_project_get_file(p);
    if (pf && *pf) {
        dir = g_path_get_dirname(pf);
        gchar *base = g_path_get_basename(pf);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        stem = base;   /* takes ownership */
    } else {
        dir  = settings_get_string("render_last_dir", NULL);
        if (!dir || !*dir) {
            g_free(dir);
            dir = jackdaw_default_projects_dir();
        }
        stem = g_strdup("render");
    }
    gchar *name = g_strdup_printf("%s.%s", stem, ext);
    gchar *full = g_build_filename(dir, name, NULL);
    gchar *uniq = unique_out_path(full);
    g_free(dir); g_free(stem); g_free(name); g_free(full);
    return uniq;
}

/* Return a newly-allocated path that does not yet exist on disk. If `path` is
 * already free it is returned as a copy; otherwise a " (N)" counter is inserted
 * before the extension: "song.wav" -> "song (1).wav" -> "song (2).wav" … so an
 * auto-filled name never silently overwrites a previous render. */
static gchar *unique_out_path(const gchar *path)
{
    if (!path || !*path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return g_strdup(path ? path : "");

    gchar *dir  = g_path_get_dirname(path);
    gchar *base = g_path_get_basename(path);
    char  *dot  = strrchr(base, '.');
    gchar *stem, *ext;
    if (dot && dot != base) {       /* keep the extension; ignore leading-dot files */
        *dot = '\0';
        stem = g_strdup(base);
        ext  = g_strdup(dot + 1);
    } else {
        stem = g_strdup(base);
        ext  = NULL;
    }

    gchar *result = NULL;
    for (int n = 1; n < 100000; n++) {
        gchar *name = ext ? g_strdup_printf("%s (%d).%s", stem, n, ext)
                          : g_strdup_printf("%s (%d)", stem, n);
        gchar *full = g_build_filename(dir, name, NULL);
        g_free(name);
        if (!g_file_test(full, G_FILE_TEST_EXISTS)) { result = full; break; }
        g_free(full);
    }
    if (!result) result = g_strdup(path);   /* implausible: give up, keep original */

    g_free(dir); g_free(base); g_free(stem); g_free(ext);
    return result;
}

static GtkWidget *labeled_row(const char *text, GtkWidget *w)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l   = gtk_label_new(text);
    gtk_widget_set_size_request(l, 90, -1);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), w, TRUE, TRUE, 0);
    return row;
}

void render_dialog_open(GtkWindow *parent, JackDawProject *project,
                        RenderScope initial_scope)
{
    OptUI *u = g_new0(OptUI, 1);
    u->project = project;

    RenderOptions saved;
    memset(&saved, 0, sizeof saved);
    jackdaw_render_options_load(&saved);

    u->dialog = gtk_dialog_new_with_buttons(
        "Render", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Render", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(u->dialog), 420, -1);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(u->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_set_spacing(GTK_BOX(box), 6);

    /* Format (WAV/FLAC/MP3 — MP3 only if libsndfile can encode it). */
    u->combo_format = gtk_combo_box_text_new();
    u->fmt_count = 0;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_format), "WAV");
    u->fmt_map[u->fmt_count++] = RENDER_FMT_WAV;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_format), "FLAC");
    u->fmt_map[u->fmt_count++] = RENDER_FMT_FLAC;
    if (jackdaw_render_mp3_available(saved.sample_rate, 2)) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_format), "MP3");
        u->fmt_map[u->fmt_count++] = RENDER_FMT_MP3;
    }
    {
        int sel = 0;
        for (int i = 0; i < u->fmt_count; i++)
            if (u->fmt_map[i] == saved.format) { sel = i; break; }
        gtk_combo_box_set_active(GTK_COMBO_BOX(u->combo_format), sel);
    }
    gtk_box_pack_start(GTK_BOX(box), labeled_row("Format", u->combo_format),
                       FALSE, FALSE, 0);

    /* WAV bit depth. */
    u->combo_bits = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_bits), "16-bit PCM");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_bits), "24-bit PCM");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_bits), "32-bit PCM");
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->combo_bits), (int)saved.bit_depth);
    gtk_box_pack_start(GTK_BOX(box), labeled_row("WAV depth", u->combo_bits),
                       FALSE, FALSE, 0);

    /* Source: master mix / selected tracks. */
    {
        guint nsel = jackdaw_project_get_selected_tracks(project)->len;
        u->rb_src_master = gtk_radio_button_new_with_label(NULL, "Master mix");
        gchar *sl = g_strdup_printf("Selected tracks (%u)", nsel);
        u->rb_src_selected = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(u->rb_src_master), sl);
        g_free(sl);
        gtk_widget_set_sensitive(u->rb_src_selected, nsel > 0);
        gboolean want_sel = (saved.source == RENDER_SRC_SELECTED) && nsel > 0;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(want_sel ? u->rb_src_selected : u->rb_src_master), TRUE);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r), u->rb_src_master, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r), u->rb_src_selected, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), labeled_row("Source", r), FALSE, FALSE, 0);
    }

    /* Scope: entire project / selected region. */
    {
        u->rb_scope_project = gtk_radio_button_new_with_label(NULL, "Entire project");
        u->rb_scope_region  = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(u->rb_scope_project), "Selected region");
        gboolean has_region = jackdaw_engine_has_loop_region();
        gtk_widget_set_sensitive(u->rb_scope_region, has_region);
        gboolean want_region = (initial_scope == RENDER_SCOPE_REGION) && has_region;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(want_region ? u->rb_scope_region : u->rb_scope_project),
            TRUE);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r), u->rb_scope_project, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r), u->rb_scope_region, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), labeled_row("Scope", r), FALSE, FALSE, 0);
    }

    /* Sample rate. */
    u->combo_sr = gtk_combo_box_text_new();
    {
        int sel = 1;   /* default 48000 */
        for (int i = 0; i < sr_n; i++) {
            char b[16]; g_snprintf(b, sizeof b, "%d", sr_values[i]);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->combo_sr), b);
            if (sr_values[i] == saved.sample_rate) sel = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(u->combo_sr), sel);
    }
    gtk_box_pack_start(GTK_BOX(box), labeled_row("Sample rate", u->combo_sr),
                       FALSE, FALSE, 0);

    /* Channels. */
    {
        u->rb_ch_stereo = gtk_radio_button_new_with_label(NULL, "Stereo");
        u->rb_ch_mono   = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(u->rb_ch_stereo), "Mono");
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(saved.channels == 1 ? u->rb_ch_mono : u->rb_ch_stereo),
            TRUE);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r), u->rb_ch_stereo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r), u->rb_ch_mono, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), labeled_row("Channels", r), FALSE, FALSE, 0);
    }

    /* Method. */
    {
        u->rb_m_offline  = gtk_radio_button_new_with_label(NULL, "Offline (fast)");
        u->rb_m_realtime = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(u->rb_m_offline), "Realtime");
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(saved.method == RENDER_METHOD_REALTIME
                              ? u->rb_m_realtime : u->rb_m_offline), TRUE);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r), u->rb_m_offline, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r), u->rb_m_realtime, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), labeled_row("Method", r), FALSE, FALSE, 0);
    }

    /* Output file. */
    {
        u->path_entry = gtk_entry_new();
        gchar *def = default_out_path(project, opt_format(u));
        gtk_entry_set_text(GTK_ENTRY(u->path_entry), def);
        g_free(def);
        GtkWidget *browse = gtk_button_new_with_label("Browse…");
        g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), u);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(r), u->path_entry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(r), browse, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), labeled_row("Output", r), FALSE, FALSE, 0);
    }

    gtk_widget_set_sensitive(u->combo_bits, opt_format(u) == RENDER_FMT_WAV);
    g_signal_connect(u->combo_format, "changed",
                     G_CALLBACK(on_format_changed), u);

    gtk_widget_show_all(u->dialog);

    /* Run modally, looping until a valid render starts or the user cancels. */
    while (gtk_dialog_run(GTK_DIALOG(u->dialog)) == GTK_RESPONSE_ACCEPT) {
        RenderOptions o;
        memset(&o, 0, sizeof o);
        o.project   = project;
        o.format    = opt_format(u);
        o.bit_depth = (RenderBitDepth)gtk_combo_box_get_active(GTK_COMBO_BOX(u->combo_bits));
        o.source    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->rb_src_selected))
                      ? RENDER_SRC_SELECTED : RENDER_SRC_MASTER;
        o.scope     = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->rb_scope_region))
                      ? RENDER_SCOPE_REGION : RENDER_SCOPE_PROJECT;
        o.method    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->rb_m_realtime))
                      ? RENDER_METHOD_REALTIME : RENDER_METHOD_OFFLINE;
        o.channels  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->rb_ch_mono)) ? 1 : 2;
        int sridx = gtk_combo_box_get_active(GTK_COMBO_BOX(u->combo_sr));
        o.sample_rate = (sridx >= 0 && sridx < sr_n) ? sr_values[sridx] : 48000;
        o.out_path  = g_strdup(gtk_entry_get_text(GTK_ENTRY(u->path_entry)));

        if (!o.out_path || !*o.out_path) {
            jackdaw_error("Please choose an output file.");
            g_free(o.out_path);
            continue;
        }
        if (!jackdaw_render_format_supported(&o)) {
            jackdaw_error("That format / sample-rate / depth combination is not "
                       "supported by libsndfile.");
            g_free(o.out_path);
            continue;
        }
        if (o.method == RENDER_METHOD_OFFLINE && jackdaw_engine_is_playing()) {
            jackdaw_error("Stop playback before an offline render.");
            g_free(o.out_path);
            continue;
        }

        /* Selected-tracks source: borrow the project's current selection. */
        if (o.source == RENDER_SRC_SELECTED) {
            GPtrArray *sel = jackdaw_project_get_selected_tracks(project);
            o.selected_tracks = g_ptr_array_new();
            for (guint i = 0; i < sel->len; i++)
                g_ptr_array_add(o.selected_tracks, g_ptr_array_index(sel, i));
        }

        jackdaw_render_options_save(&o);

        GtkWindow *parent_win = parent;
        gtk_widget_hide(u->dialog);
        start_render_with_progress(parent_win, &o);

        /* start_* deep-copy the options; free our transient copy. */
        render_options_free_contents(&o);
        break;
    }

    gtk_widget_destroy(u->dialog);
    g_free(u);
}
