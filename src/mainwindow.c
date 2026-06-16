#define _GNU_SOURCE
#include <config.h>
#include <string.h>
#include <math.h>

#include "mainwindow.h"
#include "jackdaw-engine.h"
#include "audio_clip.h"
#include "clipregion.h"
#include "mixer.h"
#include "main.h"
#include "um.h"
#include "settings.h"
#include "fxwindow.h"

G_DEFINE_TYPE(JackDawMainWindow, jackdaw_main_window, GTK_TYPE_WINDOW)

/* ---- helpers ---- */

static JackDawTimeline *mw_timeline(GtkWidget *widget)
{
    return JACKDAW_MAIN_WINDOW(widget)->timeline;
}

/* ========================================================================
 * Theme (light / dark) — one screen-global CSS provider, rebuilt on toggle.
 *
 * The functional/state colours (transport, track-strip, fader, VU) live in
 * the shared block and are IDENTICAL in both modes. Only the generic
 * text/background/outline rules differ, plus gtk-application-prefer-dark-theme
 * so the underlying theme's menus/scrollbars/entries follow along.
 * ======================================================================== */

static GtkCssProvider *g_app_css = NULL;

/* Functional + geometry rules — same in light and dark. */
static const char MW_CSS_SHARED[] =
    "tooltip label { color:#ffffff; }"
    /* Transport state colours override the generic button rules. */
    "button.transport-play {"
    "  background-image:none; background-color:#2e8b57; color:#ffffff; }"
    "button.transport-rec  {"
    "  background-image:none; background-color:#c0392b; color:#ffffff; }"
    "button.transport-loop {"
    "  background-image:none; background-color:#8ce68c; color:#101010; }"
    "label.transport-time  {"
    "  font-size:22px; font-weight:bold; font-family:monospace; }"
    /* Track strip buttons — compact size */
    "button.ts-arm, button.ts-mute, button.ts-solo,"
    "button.ts-mono, button.ts-fx {"
    "  padding:1px 3px; min-height:0; min-width:0; font-size:10px; }"
    /* Track strip button active colours */
    "button.ts-arm:checked  {"
    "  background-image:none; background-color:#c0392b; color:#ffffff; }"
    "button.ts-mute:checked {"
    "  background-image:none; background-color:#e67e22; color:#ffffff; }"
    "button.ts-solo:checked {"
    "  background-image:none; background-color:#ffe000; color:#101010; }"
    "button.ts-fx.ts-fx-active {"
    "  background-image:none; background-color:#2980b9; color:#ffffff; }"
    /* Mixer fader: flat horizontal cap (not the theme's round handle)
     * so its centre is a clear reference that lines up with the dB
     * labels. Trough margins = half the cap height so the cap centre
     * reaches the full travel; keep this in sync with FADER_SLIDER_HALF
     * (7px) in mixer.c. */
    "scale.mix-fader { padding:0; }"
    "scale.mix-fader trough {"
    "  margin:7px 0; min-width:5px;"
    "  background-image:none; background-color:#262629; }"
    "scale.mix-fader highlight {"
    "  background-image:none; background-color:#3a6ea5; }"
    "scale.mix-fader slider {"
    "  min-width:24px; min-height:12px; margin:-7px -10px;"
    "  border-radius:2px; border:1px solid #2a2a2e;"
    "  background-image:none; background-color:#d2d2d6; }"
    /* Permanent value read-out centered under each track-strip dial. */
    "label.ts-knob-val {"
    "  font-size:9px; font-family:monospace; color:#ffffff; }"
    /* Floating real-time dB read-out shown beside the fader. */
    "label.mix-db-pop {"
    "  font-size:11px; font-family:monospace; color:#ffffff;"
    "  background-color:#202024; padding:2px 5px;"
    "  border:1px solid #4a90d9; border-radius:3px; }";

/* Generic chrome rules — light variant (current look). */
static const char MW_CSS_LIGHT[] =
    "button { color:#101010; border:1px solid #808080; }"
    "button:checked { background-image:none; background-color:#b8c4d8;"
    "  color:#101010; }"
    "spinbutton, spinbutton entry { color:#101010; }"
    "label { color:#101010; }";

/* Generic chrome rules — dark variant (light text on near-black, not gray). */
static const char MW_CSS_DARK[] =
    "window, .background { background-color:#1a1a1a; color:#e6e6e6; }"
    "menubar, menu, toolbar, box, paned, scrolledwindow, viewport {"
    "  background-color:#1a1a1a; }"
    "label { color:#e6e6e6; }"
    "button { color:#e6e6e6; background-color:#2a2a2a;"
    "  border:1px solid #555555; }"
    "button:checked { background-image:none; background-color:#3a3f4a;"
    "  color:#e6e6e6; }"
    "spinbutton, spinbutton entry { color:#e6e6e6;"
    "  background-color:#262626; }";

static void mw_apply_theme(gboolean dark)
{
    GtkSettings *gs = gtk_settings_get_default();
    if (gs)
        g_object_set(gs, "gtk-application-prefer-dark-theme", dark, NULL);

    if (!g_app_css) {
        g_app_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(g_app_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    gchar *css = g_strconcat(dark ? MW_CSS_DARK : MW_CSS_LIGHT,
                             MW_CSS_SHARED, NULL);
    gtk_css_provider_load_from_data(g_app_css, css, -1, NULL);
    g_free(css);
}

/* ---- File menu ---- */

static void mw_new_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    guint n = jackdaw_project_track_count(win->project);
    while (n-- > 0) {
        JackDawTrack *t = jackdaw_project_get_track(win->project, 0);
        jackdaw_engine_remove_track(t);
        jackdaw_project_remove_track(win->project, t);
    }
    win->track_counter = 0;
    jackdaw_project_set_file(win->project, NULL);
}

static void mw_load_file_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Load Audio File",
        GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);

    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Audio files");
    gtk_file_filter_add_mime_type(ff, "audio/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GError    *err  = NULL;
            AudioClip *clip = audio_clip_new(path, &err);
            if (!clip) {
                gchar *msg = g_strdup_printf("Could not load file:\n%s\n%s",
                                             path,
                                             err ? err->message : "unknown error");
                user_error(msg);
                g_free(msg);
                if (err) g_error_free(err);
            } else {
                win->track_counter++;
                gchar *name = g_strdup_printf("Track %u", win->track_counter);
                /* clip ownership transferred to track */
                JackDawTrack *t = jackdaw_track_new(name, clip);
                g_free(name);

                if (jackdaw_engine_add_track(t)) {
                    user_error("Engine: could not add track (slot limit reached)");
                    g_object_unref(t);
                } else {
                    jackdaw_project_add_track(win->project, t);
                    g_object_unref(t);
                }
            }
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

/* Save As… — always prompts for a name/location. */
static void mw_save_as_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Project As", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    const gchar *cur = jackdaw_project_get_file(win->project);
    if (cur) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    } else {
        /* New project: default into ~/Music/JackDAW/Projects. The project name
         * the user types becomes its own bundle directory (handled on save). */
        gchar *dir = jackdaw_default_projects_dir();
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "untitled.jdaw");
        g_free(dir);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            if (jackdaw_project_save(win->project, path))   /* TRUE = failure */
                user_error("Could not save project.");
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

/* Save — writes straight to the current file once the project has one; for a
 * never-saved project it falls back to Save As… to get a name. */
static void mw_save_project_cb(GtkMenuItem *item, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    const gchar *cur = jackdaw_project_get_file(win->project);
    if (cur) {
        if (jackdaw_project_save(win->project, cur))   /* TRUE = failure */
            user_error("Could not save project.");
    } else {
        mw_save_as_project_cb(item, data);
    }
}

static void mw_open_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Project", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "JackDAW projects (*.jdaw)");
    gtk_file_filter_add_pattern(ff, "*.jdaw");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    /* Start in the projects folder unless we have a current project to anchor to. */
    const gchar *cur = jackdaw_project_get_file(win->project);
    if (cur) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    } else {
        gchar *dir = jackdaw_default_projects_dir();
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        g_free(dir);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            if (jackdaw_project_load(win->project, path))   /* TRUE = failure */
                user_error("Could not open project.");
            else
                win->track_counter = jackdaw_project_track_count(win->project);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void mw_quit_cb(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    gtk_main_quit();
}

/* ---- Track menu ---- */

static void mw_add_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    win->track_counter++;
    gchar *name = g_strdup_printf("Track %u", win->track_counter);
    JackDawTrack *t = jackdaw_track_new(name, NULL);
    g_free(name);

    if (jackdaw_engine_add_track(t)) {
        user_error("Engine: could not add track (slot limit reached)");
        g_object_unref(t);
        return;
    }
    jackdaw_project_add_track(win->project, t);
    g_object_unref(t);
}

static void mw_show_master_cb(GtkCheckMenuItem *item, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_timeline_set_master_visible(win->timeline,
                                        gtk_check_menu_item_get_active(item));
}

static void mw_add_instrument_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    win->track_counter++;
    gchar *name = g_strdup_printf("MIDI %u", win->track_counter);
    JackDawTrack *t = jackdaw_track_new(name, NULL);
    g_free(name);
    jackdaw_track_set_kind(t, JACKDAW_TRACK_INSTRUMENT);

    if (jackdaw_engine_add_track(t)) {
        user_error("Engine: could not add track (slot limit reached)");
        g_object_unref(t);
        return;
    }
    jackdaw_project_add_track(win->project, t);
    g_object_unref(t);
}

static void mw_remove_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    JackDawTrack *t = jackdaw_timeline_get_focused(win->timeline);
    if (!t) return;
    jackdaw_engine_remove_track(t);
    jackdaw_project_remove_track(win->project, t);
}

static gboolean mw_tracks_box_press_cb(GtkWidget *widget, GdkEventButton *ev,
                                        gpointer data)
{
    (void)widget;
    if (ev->button != 3) return FALSE;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *mi_add = gtk_menu_item_new_with_label("Add Track");
    g_signal_connect(mi_add, "activate", G_CALLBACK(mw_add_track_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_add);

    GtkWidget *mi_midi = gtk_menu_item_new_with_label("Add MIDI Track");
    g_signal_connect(mi_midi, "activate",
                     G_CALLBACK(mw_add_instrument_track_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_midi);

    GtkWidget *mi_sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_sep);

    GtkWidget *mi_rem = gtk_menu_item_new_with_label("Remove Focused Track");
    g_signal_connect(mi_rem, "activate", G_CALLBACK(mw_remove_track_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_rem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mi_master = gtk_check_menu_item_new_with_label("Show Master Track");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi_master),
                                   win->timeline && win->timeline->master_row != NULL);
    g_signal_connect(mi_master, "toggled", G_CALLBACK(mw_show_master_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_master);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* ---- Transport ---- */

/* Toggle a CSS style class on a widget based on a boolean. */
static void mw_set_class(GtkWidget *w, const char *cls, gboolean on)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    if (on) gtk_style_context_add_class(ctx, cls);
    else    gtk_style_context_remove_class(ctx, cls);
}

static void mw_transport_play_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (on) jackdaw_engine_start_playback();
    else    jackdaw_engine_stop_playback();
    mw_set_class(widget, "transport-play", on);
}

static void mw_transport_loop_cb(GtkWidget *widget, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    jackdaw_engine_set_loop_enabled(on);
    mw_set_class(widget, "transport-loop", on);
    if (win->timeline) {
        jackdaw_timeline_redraw_all(win->timeline);
        gtk_widget_queue_draw(GTK_WIDGET(win->timeline));  /* refresh ruler band */
    }
}

static void mw_pause_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

static void mw_transport_stop_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

/* Transport menu items drive the toolbar toggle buttons so that the play/loop/
 * record callbacks (which read GtkToggleButton state from their widget) run with
 * the correct widget and all UI stays in sync. */
static void mw_menu_play_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(win->play_button);
    gtk_toggle_button_set_active(b, !gtk_toggle_button_get_active(b));
}

static void mw_menu_record_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(win->record_button);
    gtk_toggle_button_set_active(b, !gtk_toggle_button_get_active(b));
}

static void mw_menu_loop_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(win->loop_button);
    gtk_toggle_button_set_active(b, !gtk_toggle_button_get_active(b));
}

static void mw_transport_record_cb(GtkWidget *widget, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

    /* In punch mode the Record button is only a mode indicator — punch is driven
     * by Play (auto-records over the tab region). Ignore manual toggles; the
     * transport timer reflects the live recording state on the glyph. */
    if (jackdaw_engine_get_record_mode() == RECORD_MODE_PUNCH) {
        if (on) {
            g_signal_handlers_block_by_func(widget, mw_transport_record_cb, win);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
            g_signal_handlers_unblock_by_func(widget, mw_transport_record_cb, win);
        }
        return;
    }

    if (on) {
        jackdaw_engine_start_recording();
        /* start_recording sets ENGINE_PLAYING; keep the play button in sync */
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), TRUE);
    } else {
        jackdaw_engine_stop_recording();
    }
    mw_set_class(widget, "transport-rec", on);
    if (win->record_glyph) gtk_widget_queue_draw(win->record_glyph);
}

/* Record glyph: a hollow ring "O" (record), plus a filled centre dot in punch
 * mode "◉". Drawn white over the red recording background, red otherwise. */
static gboolean mw_record_glyph_draw_cb(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    double cx = a.width / 2.0, cy = a.height / 2.0;
    double r  = (MIN(a.width, a.height) / 2.0) - 2.0;
    if (r < 3.0) r = 3.0;

    gboolean rec   = jackdaw_engine_is_recording();
    gboolean punch = jackdaw_engine_get_record_mode() == RECORD_MODE_PUNCH;

    if (rec) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    else     cairo_set_source_rgb(cr, 0.80, 0.20, 0.17);

    cairo_set_line_width(cr, 2.0);
    cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    if (punch) {
        cairo_arc(cr, cx, cy, r * 0.42, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
    }
    return FALSE;
}

static void mw_record_set_mode(JackDawMainWindow *win, int mode)
{
    jackdaw_engine_set_record_mode(mode);
    gtk_widget_set_tooltip_text(win->record_button,
        mode == RECORD_MODE_PUNCH ? "Record — Punch In/Out (Play to punch)"
                                  : "Record");
    if (win->record_glyph) gtk_widget_queue_draw(win->record_glyph);
}

static void mw_record_mode_normal_cb(GtkMenuItem *m, gpointer data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(m)))
        mw_record_set_mode(JACKDAW_MAIN_WINDOW(data), RECORD_MODE_NORMAL);
}

static void mw_record_mode_punch_cb(GtkMenuItem *m, gpointer data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(m)))
        mw_record_set_mode(JACKDAW_MAIN_WINDOW(data), RECORD_MODE_PUNCH);
}

/* Right-click on the Record button: choose Normal / Punch In/Out. */
static gboolean mw_record_button_press_cb(GtkWidget *w, GdkEventButton *ev,
                                          gpointer data)
{
    (void)w;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;

    int mode = jackdaw_engine_get_record_mode();
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_n = gtk_radio_menu_item_new_with_label(NULL, "Normal");
    GtkWidget *mi_p = gtk_radio_menu_item_new_with_label_from_widget(
                          GTK_RADIO_MENU_ITEM(mi_n), "Punch In/Out");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(
        mode == RECORD_MODE_PUNCH ? mi_p : mi_n), TRUE);
    g_signal_connect(mi_n, "toggled", G_CALLBACK(mw_record_mode_normal_cb), win);
    g_signal_connect(mi_p, "toggled", G_CALLBACK(mw_record_mode_punch_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_n);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_p);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE; /* don't toggle recording on right-click */
}

/* ---- Function toolbar callbacks ---- */

static void mw_split_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    jackdaw_timeline_split_at_cursor(mw_timeline(GTK_WIDGET(data)));
}

static void mw_grid_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_grid_enabled(win->project,
                                     gtk_toggle_button_get_active(b));
}

static void mw_snap_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_snap_enabled(win->project,
                                     gtk_toggle_button_get_active(b));
}

static void mw_metro_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_metronome(win->project,
                                  gtk_toggle_button_get_active(b));
}

/* Live update of the value label next to the metronome volume slider. */
static void mw_metro_vol_changed(GtkRange *r, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gdouble db = gtk_range_get_value(r);
    jackdaw_project_set_metronome_volume(win->project, db);

    GtkWidget *lbl = g_object_get_data(G_OBJECT(r), "value-label");
    if (lbl) {
        gchar buf[32];
        g_snprintf(buf, sizeof buf, "%+.1f dB", db);
        gtk_label_set_text(GTK_LABEL(lbl), buf);
    }
}

/* Keep the metronome window alive (singleton) when the user closes it. */
static gboolean mw_metro_window_delete_cb(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    return TRUE; /* don't destroy */
}

/* ---- Inputs/Outputs window (audio JACK port counts) ---------------------- */

static gboolean mw_io_window_delete_cb(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    return TRUE; /* keep for reuse */
}

static void mw_io_in_changed(GtkSpinButton *sb, gpointer data)
{
    (void)data;
    jackdaw_engine_set_audio_in_count((guint)gtk_spin_button_get_value_as_int(sb));
}

static void mw_io_out_changed(GtkSpinButton *sb, gpointer data)
{
    (void)data;
    jackdaw_engine_set_audio_out_count((guint)gtk_spin_button_get_value_as_int(sb));
}

/* Open (creating lazily) the Inputs/Outputs settings window. Lets the user
 * grow/shrink the number of audio JACK ports jackdaw exposes. The engine
 * setters re-register ports, persist the count, and emit "ports-changed",
 * which refreshes every track's input combo automatically. */
static void mw_open_io_window(JackDawMainWindow *win)
{
    if (!win->io_window) {
        win->io_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win->io_window), "Inputs / Outputs");
        gtk_window_set_resizable(GTK_WINDOW(win->io_window), FALSE);
        gtk_window_set_transient_for(GTK_WINDOW(win->io_window),
                                     GTK_WINDOW(win));
        g_signal_connect(win->io_window, "delete-event",
                         G_CALLBACK(mw_io_window_delete_cb), win);

        GtkWidget *grid = gtk_grid_new();
        gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
        gtk_container_add(GTK_CONTAINER(win->io_window), grid);

        GtkWidget *in_label = gtk_label_new("Inputs");
        gtk_widget_set_halign(in_label, GTK_ALIGN_START);
        GtkWidget *in_spin = gtk_spin_button_new_with_range(1.0, 64.0, 1.0);
        gtk_grid_attach(GTK_GRID(grid), in_label, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), in_spin,  1, 0, 1, 1);

        GtkWidget *out_label = gtk_label_new("Outputs");
        gtk_widget_set_halign(out_label, GTK_ALIGN_START);
        GtkWidget *out_spin = gtk_spin_button_new_with_range(1.0, 64.0, 1.0);
        gtk_grid_attach(GTK_GRID(grid), out_label, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), out_spin,  1, 1, 1, 1);

        g_signal_connect(in_spin,  "value-changed",
                         G_CALLBACK(mw_io_in_changed),  win);
        g_signal_connect(out_spin, "value-changed",
                         G_CALLBACK(mw_io_out_changed), win);

        g_object_set_data(G_OBJECT(win->io_window), "in-spin",  in_spin);
        g_object_set_data(G_OBJECT(win->io_window), "out-spin", out_spin);
    }

    /* Re-sync spins to the live engine counts. Block the handlers so this
     * resync doesn't trigger a needless port re-register cycle. */
    {
        GtkWidget *in_spin  =
            g_object_get_data(G_OBJECT(win->io_window), "in-spin");
        GtkWidget *out_spin =
            g_object_get_data(G_OBJECT(win->io_window), "out-spin");

        g_signal_handlers_block_by_func(in_spin,
                                        G_CALLBACK(mw_io_in_changed), win);
        g_signal_handlers_block_by_func(out_spin,
                                        G_CALLBACK(mw_io_out_changed), win);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(in_spin),
                                  jackdaw_engine_get_audio_in_count());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(out_spin),
                                  jackdaw_engine_get_audio_out_count());
        g_signal_handlers_unblock_by_func(in_spin,
                                          G_CALLBACK(mw_io_in_changed), win);
        g_signal_handlers_unblock_by_func(out_spin,
                                          G_CALLBACK(mw_io_out_changed), win);
    }

    gtk_widget_show_all(win->io_window);
    gtk_window_present(GTK_WINDOW(win->io_window));
}

static void mw_io_menu_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    mw_open_io_window(JACKDAW_MAIN_WINDOW(data));
}

/* Open the global plugin paths / rescan dialog (add/remove scan folders,
 * load the plugin cache). This manages where plugins live, not per-track FX. */
static void mw_plugins_menu_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    jackdaw_fx_paths_dialog(GTK_WINDOW(data));
}

/* Open (creating lazily) the metronome settings window. */
static void mw_open_metronome_window(JackDawMainWindow *win)
{
    if (!win->metro_window) {
        win->metro_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win->metro_window), "Metronome");
        gtk_window_set_resizable(GTK_WINDOW(win->metro_window), FALSE);
        gtk_window_set_transient_for(GTK_WINDOW(win->metro_window),
                                     GTK_WINDOW(win));
        g_signal_connect(win->metro_window, "delete-event",
                         G_CALLBACK(mw_metro_window_delete_cb), win);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
        gtk_container_add(GTK_CONTAINER(win->metro_window), vbox);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Volume"),
                           FALSE, FALSE, 0);

        gdouble db = jackdaw_project_get_metronome_volume(win->project);

        GtkWidget *vlabel = gtk_label_new(NULL);
        {
            gchar buf[32];
            g_snprintf(buf, sizeof buf, "%+.1f dB", db);
            gtk_label_set_text(GTK_LABEL(vlabel), buf);
        }

        GtkWidget *scale = gtk_scale_new_with_range(
            GTK_ORIENTATION_HORIZONTAL, -25.0, 25.0, 0.5);
        gtk_range_set_value(GTK_RANGE(scale), db);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_scale_add_mark(GTK_SCALE(scale), -25.0, GTK_POS_BOTTOM, "-25");
        gtk_scale_add_mark(GTK_SCALE(scale),   0.0, GTK_POS_BOTTOM, "0");
        gtk_scale_add_mark(GTK_SCALE(scale),  25.0, GTK_POS_BOTTOM, "+25");
        gtk_widget_set_size_request(scale, 240, -1);
        g_object_set_data(G_OBJECT(scale), "value-label", vlabel);
        g_signal_connect(scale, "value-changed",
                         G_CALLBACK(mw_metro_vol_changed), win);
        gtk_box_pack_start(GTK_BOX(vbox), scale, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), vlabel, FALSE, FALSE, 0);

        /* Remember the slider so subsequent opens can re-sync to the project. */
        g_object_set_data(G_OBJECT(win->metro_window), "vol-scale", scale);
    }

    /* Re-sync the slider to the current project's stored value (it may differ
     * after loading a different session). value-changed updates the label. */
    {
        GtkWidget *scale =
            g_object_get_data(G_OBJECT(win->metro_window), "vol-scale");
        gtk_range_set_value(GTK_RANGE(scale),
                            jackdaw_project_get_metronome_volume(win->project));
    }

    gtk_widget_show_all(win->metro_window);
    gtk_window_present(GTK_WINDOW(win->metro_window));
}

/* Right-click on the Metro toolbar button opens the settings window. */
static gboolean mw_metro_button_press_cb(GtkWidget *w, GdkEventButton *ev,
                                         gpointer data)
{
    (void)w;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        mw_open_metronome_window(win);
        return TRUE; /* don't toggle on right-click */
    }
    return FALSE;
}

static void mw_metro_menu_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    mw_open_metronome_window(JACKDAW_MAIN_WINDOW(data));
}

static void mw_bars_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_ruler_mode(win->project,
        gtk_toggle_button_get_active(b) ? JACKDAW_RULER_BARS
                                        : JACKDAW_RULER_TIME);
}

static void mw_bpm_changed(GtkSpinButton *sb, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_bpm(win->project, gtk_spin_button_get_value(sb));
}

static void mw_timesig_changed(GtkSpinButton *sb, gpointer data)
{
    (void)sb;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkSpinButton *num = g_object_get_data(G_OBJECT(win), "ts-num");
    GtkSpinButton *den = g_object_get_data(G_OBJECT(win), "ts-den");
    if (num && den)
        jackdaw_project_set_time_signature(win->project,
            (guint)gtk_spin_button_get_value_as_int(num),
            (guint)gtk_spin_button_get_value_as_int(den));
}

/* Move the mixer back into the bottom dock (paned pack2) if it isn't already. */
static void mw_mixer_dock(JackDawMainWindow *win)
{
    GtkWidget *parent = gtk_widget_get_parent(win->mixer);
    if (parent == win->paned) return;
    g_object_ref(win->mixer);
    if (parent) gtk_container_remove(GTK_CONTAINER(parent), win->mixer);
    gtk_paned_pack2(GTK_PANED(win->paned), win->mixer, FALSE, FALSE);
    g_object_unref(win->mixer);
}

static gboolean mw_mixer_window_delete_cb(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    /* Closing the window via the WM behaves like toggling the Mixer button off;
       the toggle handler hides the window for us. Returning TRUE prevents the
       default destroy so the window can be reused. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->mixer_button), FALSE);
    return TRUE;
}

/* Move the mixer into its own top-level window, creating it lazily. */
static void mw_mixer_undock(JackDawMainWindow *win)
{
    if (!win->mixer_window) {
        win->mixer_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win->mixer_window), "Mixer");
        gtk_window_set_default_size(GTK_WINDOW(win->mixer_window), 700, 320);
        gtk_window_set_transient_for(GTK_WINDOW(win->mixer_window),
                                     GTK_WINDOW(win));
        g_signal_connect(win->mixer_window, "delete-event",
                         G_CALLBACK(mw_mixer_window_delete_cb), win);
    }
    GtkWidget *parent = gtk_widget_get_parent(win->mixer);
    if (parent == win->mixer_window) return;
    g_object_ref(win->mixer);
    if (parent) gtk_container_remove(GTK_CONTAINER(parent), win->mixer);
    gtk_container_add(GTK_CONTAINER(win->mixer_window), win->mixer);
    gtk_widget_show(win->mixer);
    g_object_unref(win->mixer);
}

/* Apply the current mixer-button state under the current docked/windowed mode. */
static void mw_mixer_apply(JackDawMainWindow *win)
{
    gboolean active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(win->mixer_button));
    if (win->mixer_in_window) {
        if (active) {
            mw_mixer_undock(win);
            gtk_widget_show_all(win->mixer_window);
            gtk_window_present(GTK_WINDOW(win->mixer_window));
        } else if (win->mixer_window) {
            gtk_widget_hide(win->mixer_window);
        }
    } else {
        if (win->mixer_window) gtk_widget_hide(win->mixer_window);
        mw_mixer_dock(win);
        gtk_widget_set_visible(win->mixer, active);
    }
}

static void mw_mixer_toggled(GtkToggleButton *b, gpointer data)
{
    (void)b;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (!win->mixer) return;
    mw_mixer_apply(win);
}

static void mw_mixer_window_mode_cb(GtkCheckMenuItem *item, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gboolean on = gtk_check_menu_item_get_active(item);
    settings_set_uint32("mixer_in_window", on ? 1 : 0);
    win->mixer_in_window = on;
    /* Live switch: move an already-open mixer to the new location. */
    mw_mixer_apply(win);
}

static void mw_locate_start_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_locate(0);
    jackdaw_timeline_set_cursor(win->timeline, 0);
    gtk_adjustment_set_value(win->timeline->time_adj, 0.0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

static void mw_locate_next_boundary_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    off_t cursor = (off_t)gtk_adjustment_get_value(win->timeline->cursor_adj);
    off_t next = G_MAXINT64;
    guint n = jackdaw_project_track_count(win->project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(win->project, i);
        GPtrArray *regions = jackdaw_track_get_regions(t);
        for (guint j = 0; j < regions->len; j++) {
            ClipRegion *r = g_ptr_array_index(regions, j);
            if (r->tl_pos > cursor && r->tl_pos < next)
                next = r->tl_pos;
            off_t end = clip_region_end(r);
            if (end > cursor && end < next)
                next = end;
        }
    }
    if (next != G_MAXINT64) {
        jackdaw_engine_locate(next);
        jackdaw_timeline_set_cursor(win->timeline, next);
    }
}

static off_t mw_one_frame(void)
{
    /* One frame at 25 fps (the standard audio production frame size).
     * Falls back to 1920 (48000/25) when JACK is not connected. */
    jack_nframes_t sr = jackdaw_engine_is_running()
                        ? jackdaw_engine_get_sample_rate() : 48000u;
    return (off_t)(sr / 25);
}

static void mw_step_back_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    off_t pos = (off_t)gtk_adjustment_get_value(win->timeline->cursor_adj);
    off_t step = mw_one_frame();
    pos = (pos > step) ? pos - step : 0;
    jackdaw_engine_locate(pos);
    jackdaw_timeline_set_cursor(win->timeline, pos);
}

static void mw_step_forward_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    off_t pos = (off_t)gtk_adjustment_get_value(win->timeline->cursor_adj);
    pos += mw_one_frame();
    jackdaw_engine_locate(pos);
    jackdaw_timeline_set_cursor(win->timeline, pos);
}

/* ---- Edit menu — undo/redo (Phase 4+ when editing is added) ---- */

static void mw_undo_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_undo(mw_timeline(GTK_WIDGET(data)));
}

static void mw_redo_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_redo(mw_timeline(GTK_WIDGET(data)));
}

/* ---- View menu ---- */

static void mw_zoom_in_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_zoom_in(mw_timeline(GTK_WIDGET(data)));
}

static void mw_zoom_out_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_zoom_out(mw_timeline(GTK_WIDGET(data)));
}

static void mw_dark_mode_cb(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    gboolean on = gtk_check_menu_item_get_active(item);
    settings_set_uint32("dark_mode", on ? 1 : 0);
    mw_apply_theme(on);
    /* Force Cairo-drawn surfaces (e.g. the mixer dB scale, which picks its
     * text colour from the theme) to repaint with the new scheme. */
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

/* ---- Timeline position-changed signal ---- */

static void mw_on_position_changed(JackDawTimeline *tl, gint64 sample,
                                    gpointer data)
{
    (void)tl;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;
    gchar tbuf[64];
    get_time(sr, (off_t)sample, (off_t)sample, tbuf, default_timescale_mode);
    gtk_label_set_text(GTK_LABEL(win->time_label), tbuf);
}

/* ---- 100 ms transport display timer ---- */

static gboolean mw_transport_timer(gpointer data)
{
    JackDawMainWindow *win = data;
    if (!JACKDAW_IS_MAIN_WINDOW(win)) return G_SOURCE_REMOVE;

    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;
    off_t pos = jackdaw_engine_get_play_pos();
    gchar tbuf[64];
    get_time(sr, pos, pos, tbuf, default_timescale_mode);
    gtk_label_set_text(GTK_LABEL(win->time_label), tbuf);

    /* Keep the loop toggle in sync (it may be toggled from the MIDI window). */
    if (win->loop_button) {
        gboolean loop_on = jackdaw_engine_get_loop_enabled();
        gboolean btn_on  =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->loop_button));
        if (btn_on != loop_on) {
            g_signal_handlers_block_by_func(win->loop_button,
                                            mw_transport_loop_cb, win);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->loop_button),
                                         loop_on);
            mw_set_class(win->loop_button, "transport-loop", loop_on);
            g_signal_handlers_unblock_by_func(win->loop_button,
                                              mw_transport_loop_cb, win);
        }
    }

    /* Reflect live recording state on the Record button. In punch mode the button
     * is never toggled by hand, so the engine drives its red highlight + glyph as
     * the playhead crosses the tab region. Only act on a transition. */
    if (win->record_button) {
        gboolean rec = jackdaw_engine_is_recording();
        GtkStyleContext *ctx = gtk_widget_get_style_context(win->record_button);
        gboolean had = gtk_style_context_has_class(ctx, "transport-rec");
        if (rec != had) {
            mw_set_class(win->record_button, "transport-rec", rec);
            if (win->record_glyph) gtk_widget_queue_draw(win->record_glyph);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- Key press (Space = play/stop, Home = locate start) ---- */

static gboolean mw_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer data)
{
    (void)data;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(widget);

    switch (event->keyval) {
    case GDK_KEY_space:
    case GDK_KEY_KP_Space: {
        gboolean active =
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), active);
        return TRUE;
    }
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        mw_locate_start_cb(NULL, win);
        return TRUE;
    case GDK_KEY_s:
    case GDK_KEY_g: {
        /* Plain-letter shortcuts must not fire while text is being edited. */
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus && GTK_IS_EDITABLE(focus)) return FALSE;
        JackDawTimeline *tl = mw_timeline(GTK_WIDGET(win));
        if (event->keyval == GDK_KEY_s)
            jackdaw_timeline_split_at_cursor(tl);
        else
            jackdaw_timeline_group_selection(tl);
        return TRUE;
    }
    case GDK_KEY_r:
    case GDK_KEY_R:
    case GDK_KEY_l:
    case GDK_KEY_L: {
        /* Plain-letter shortcuts must not fire while text is being edited. */
        if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) return FALSE;
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus && GTK_IS_EDITABLE(focus)) return FALSE;
        GtkWidget *btn = (event->keyval == GDK_KEY_r || event->keyval == GDK_KEY_R)
                             ? win->record_button : win->loop_button;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(btn),
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn)));
        return TRUE;
    }
    case GDK_KEY_c:
    case GDK_KEY_C:
    case GDK_KEY_v:
    case GDK_KEY_V: {
        if (!(event->state & GDK_CONTROL_MASK)) return FALSE;
        /* Let text widgets keep their own clipboard behaviour. */
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus && GTK_IS_EDITABLE(focus)) return FALSE;
        JackDawTimeline *tl = mw_timeline(GTK_WIDGET(win));
        if (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)
            jackdaw_timeline_copy_selection(tl);
        else
            jackdaw_timeline_paste_at_cursor(tl);
        return TRUE;
    }
    default:
        break;
    }
    return FALSE;
}

/* ---- Delete-event → quit gtk_main loop ---- */

static gboolean mw_delete_event(GtkWidget *widget, GdkEvent *event,
                                  gpointer data)
{
    (void)widget; (void)event; (void)data;
    gtk_main_quit();
    return FALSE;
}

/* ---- Menu construction helper ---- */

static GtkWidget *make_submenu_item(GtkWidget *menubar, const gchar *label)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), item);
    return menu;
}

static void menu_item(GtkWidget *menu, const gchar *label,
                       GCallback cb, gpointer data,
                       guint accel_key, GdkModifierType accel_mods,
                       GtkAccelGroup *ag)
{
    GtkWidget *item;
    if (!label) {
        item = gtk_separator_menu_item_new();
    } else {
        item = gtk_menu_item_new_with_mnemonic(label);
        if (cb) g_signal_connect(item, "activate", cb, data);
        if (accel_key && ag) {
            gtk_widget_add_accelerator(item, "activate", ag,
                                       accel_key, accel_mods,
                                       GTK_ACCEL_VISIBLE);
        }
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ---- GObject boilerplate ---- */

static void jackdaw_main_window_finalize(GObject *obj)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(obj);
    if (win->transport_timer) {
        g_source_remove(win->transport_timer);
        win->transport_timer = 0;
    }
    g_object_unref(win->project);
    G_OBJECT_CLASS(jackdaw_main_window_parent_class)->finalize(obj);
}

static void jackdaw_main_window_class_init(JackDawMainWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_main_window_finalize;
}

static void jackdaw_main_window_init(JackDawMainWindow *win)
{
    win->project         = NULL;
    win->timeline        = NULL;
    win->play_button     = NULL;
    win->record_button   = NULL;
    win->record_glyph    = NULL;
    win->loop_button     = NULL;
    win->time_label      = NULL;
    win->mixer           = NULL;
    win->paned           = NULL;
    win->mixer_window    = NULL;
    win->mixer_button    = NULL;
    win->mixer_in_window = FALSE;
    win->track_counter   = 0;
    win->transport_timer = 0;
}

/* ---- Constructor ---- */

GtkWidget *jackdaw_main_window_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawMainWindow *win =
        g_object_new(JACKDAW_TYPE_MAIN_WINDOW, NULL);

    win->project = g_object_ref(project);

    gtk_window_set_title(GTK_WINDOW(win), "JackDAW 0.1.0");
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 700);

    /* Transport/track state colours + light-or-dark chrome. Applies the
     * persisted dark-mode preference (default: light). */
    mw_apply_theme(settings_get_uint32("dark_mode", 1) != 0);

    g_signal_connect(win, "delete-event", G_CALLBACK(mw_delete_event), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(mw_key_press), NULL);

    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), ag);
    g_object_unref(ag);

    /* ---- Root vbox ---- */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* ---- Menu bar ---- */
    GtkWidget *menubar = gtk_menu_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* File */
    GtkWidget *m = make_submenu_item(menubar, "_File");
    menu_item(m, "_Open Project…",
              G_CALLBACK(mw_open_project_cb), win,
              GDK_KEY_o, GDK_CONTROL_MASK, ag);
    menu_item(m, "_Save Project",
              G_CALLBACK(mw_save_project_cb), win,
              GDK_KEY_s, GDK_CONTROL_MASK, ag);
    menu_item(m, "Save Project _As…",
              G_CALLBACK(mw_save_as_project_cb), win,
              GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_New Session",
              G_CALLBACK(mw_new_project_cb), win,
              GDK_KEY_n, GDK_CONTROL_MASK, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_Quit",
              G_CALLBACK(mw_quit_cb), win,
              GDK_KEY_q, GDK_CONTROL_MASK, ag);

    /* Edit */
    m = make_submenu_item(menubar, "_Edit");
    menu_item(m, "_Undo",
              G_CALLBACK(mw_undo_cb), win,
              GDK_KEY_z, GDK_CONTROL_MASK, ag);
    menu_item(m, "_Redo",
              G_CALLBACK(mw_redo_cb), win,
              GDK_KEY_y, GDK_CONTROL_MASK, ag);

    /* Track */
    m = make_submenu_item(menubar, "_Track");
    menu_item(m, "_Add Empty Track",
              G_CALLBACK(mw_add_track_cb), win, 0, 0, ag);
    menu_item(m, "Add _MIDI Track",
              G_CALLBACK(mw_add_instrument_track_cb), win, 0, 0, ag);
    menu_item(m, "_Load File as New Track…",
              G_CALLBACK(mw_load_file_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_Remove Focused Track",
              G_CALLBACK(mw_remove_track_cb), win, 0, 0, ag);

    /* Transport */
    m = make_submenu_item(menubar, "T_ransport");
    menu_item(m, "_Play / Stop  [Space]",
              G_CALLBACK(mw_menu_play_cb), win, 0, 0, ag);
    menu_item(m, "_Record  [R]",
              G_CALLBACK(mw_menu_record_cb), win, 0, 0, ag);
    menu_item(m, "_Loop  [L]",
              G_CALLBACK(mw_menu_loop_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "Locate to _Start  [Home]",
              G_CALLBACK(mw_locate_start_cb), win, 0, 0, ag);

    /* View */
    m = make_submenu_item(menubar, "_View");
    menu_item(m, "Zoom _In   [Ctrl++]",
              G_CALLBACK(mw_zoom_in_cb), win,
              GDK_KEY_equal, GDK_CONTROL_MASK, ag);
    menu_item(m, "Zoom _Out  [Ctrl+-]",
              G_CALLBACK(mw_zoom_out_cb), win,
              GDK_KEY_minus, GDK_CONTROL_MASK, ag);
    {
        GtkWidget *mi_dark = gtk_check_menu_item_new_with_label("Dark Mode");
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(mi_dark),
            settings_get_uint32("dark_mode", 1) != 0);
        g_signal_connect(mi_dark, "toggled",
                         G_CALLBACK(mw_dark_mode_cb), win);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi_dark);
    }
    {
        GtkWidget *mi_mixwin =
            gtk_check_menu_item_new_with_label("Open Mixer in Window");
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(mi_mixwin),
            settings_get_uint32("mixer_in_window", 0) != 0);
        g_signal_connect(mi_mixwin, "toggled",
                         G_CALLBACK(mw_mixer_window_mode_cb), win);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi_mixwin);
    }
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);  /* separator */
    menu_item(m, "_Metronome…",
              G_CALLBACK(mw_metro_menu_cb), win, 0, 0, ag);

    /* Options */
    m = make_submenu_item(menubar, "_Options");
    menu_item(m, "_Inputs/Outputs…",
              G_CALLBACK(mw_io_menu_cb), win, 0, 0, ag);
    menu_item(m, "_Plugins…",
              G_CALLBACK(mw_plugins_menu_cb), win, 0, 0, ag);

    /* ---- Transport toolbar ---- */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 3);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *btn_start = gtk_button_new_with_label("|◀");
    gtk_widget_set_tooltip_text(btn_start, "Return to start");
    g_signal_connect(btn_start, "clicked",
                     G_CALLBACK(mw_locate_start_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_start, FALSE, FALSE, 0);

    GtkWidget *btn_step_back = gtk_button_new_with_label("|<<");
    gtk_widget_set_tooltip_text(btn_step_back, "Step back one frame (25fps)");
    g_signal_connect(btn_step_back, "clicked",
                     G_CALLBACK(mw_step_back_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_step_back, FALSE, FALSE, 0);

    GtkWidget *btn_step_fwd = gtk_button_new_with_label(">>|");
    gtk_widget_set_tooltip_text(btn_step_fwd, "Step forward one frame (25fps)");
    g_signal_connect(btn_step_fwd, "clicked",
                     G_CALLBACK(mw_step_forward_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_step_fwd, FALSE, FALSE, 0);

    GtkWidget *btn_next = gtk_button_new_with_label("▶|");
    gtk_widget_set_tooltip_text(btn_next, "Jump to next clip boundary");
    g_signal_connect(btn_next, "clicked",
                     G_CALLBACK(mw_locate_next_boundary_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_next, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 2);

    win->play_button = gtk_toggle_button_new_with_label("▶");
    g_signal_connect(win->play_button, "toggled",
                     G_CALLBACK(mw_transport_play_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), win->play_button, FALSE, FALSE, 0);

    win->loop_button = gtk_toggle_button_new();
    {
        GtkWidget *loop_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(loop_lbl), "<span size='xx-large'>⟳</span>");
        gtk_container_add(GTK_CONTAINER(win->loop_button), loop_lbl);
    }
    gtk_widget_set_tooltip_text(win->loop_button, "Loop region");
    g_signal_connect(win->loop_button, "toggled",
                     G_CALLBACK(mw_transport_loop_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), win->loop_button, FALSE, FALSE, 0);

    GtkWidget *btn_pause = gtk_button_new_with_label("||");
    g_signal_connect(btn_pause, "clicked", G_CALLBACK(mw_pause_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_pause, FALSE, FALSE, 0);

    GtkWidget *btn_stop = gtk_button_new_with_label("■");
    g_signal_connect(btn_stop, "clicked",
                     G_CALLBACK(mw_transport_stop_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_stop, FALSE, FALSE, 0);

    win->record_button = gtk_toggle_button_new();
    win->record_glyph  = gtk_drawing_area_new();
    gtk_widget_set_size_request(win->record_glyph, 20, 20);
    gtk_container_add(GTK_CONTAINER(win->record_button), win->record_glyph);
    g_signal_connect(win->record_glyph, "draw",
                     G_CALLBACK(mw_record_glyph_draw_cb), win);
    gtk_widget_set_tooltip_text(win->record_button, "Record");
    g_signal_connect(win->record_button, "toggled",
                     G_CALLBACK(mw_transport_record_cb), win);
    g_signal_connect(win->record_button, "button-press-event",
                     G_CALLBACK(mw_record_button_press_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), win->record_button, FALSE, FALSE, 0);

    win->time_label = gtk_label_new("00:00.0");
    gtk_style_context_add_class(gtk_widget_get_style_context(win->time_label),
                                "transport-time");
    gtk_widget_set_size_request(win->time_label, 160, -1);
    gtk_box_pack_start(GTK_BOX(toolbar), win->time_label, FALSE, FALSE, 8);

    /* ---- Function toolbar (DAW tools) ---- */
    GtkWidget *ftb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(ftb), 3);
    gtk_box_pack_start(GTK_BOX(vbox), ftb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *btn_split = gtk_button_new_with_label("Split");
    gtk_widget_set_tooltip_text(btn_split, "Split focused track at the cursor");
    g_signal_connect(btn_split, "clicked", G_CALLBACK(mw_split_cb), win);
    gtk_box_pack_start(GTK_BOX(ftb), btn_split, FALSE, FALSE, 0);

    GtkWidget *tg_grid = gtk_toggle_button_new_with_label("Grid");
    g_signal_connect(tg_grid, "toggled", G_CALLBACK(mw_grid_toggled), win);
    gtk_box_pack_start(GTK_BOX(ftb), tg_grid, FALSE, FALSE, 0);

    GtkWidget *tg_snap = gtk_toggle_button_new_with_label("Snap");
    g_signal_connect(tg_snap, "toggled", G_CALLBACK(mw_snap_toggled), win);
    gtk_box_pack_start(GTK_BOX(ftb), tg_snap, FALSE, FALSE, 0);

    GtkWidget *tg_metro = gtk_toggle_button_new_with_label("Metro");
    gtk_widget_set_tooltip_text(tg_metro,
        "Toggle metronome  (right-click for settings)");
    g_signal_connect(tg_metro, "toggled", G_CALLBACK(mw_metro_toggled), win);
    g_signal_connect(tg_metro, "button-press-event",
                     G_CALLBACK(mw_metro_button_press_cb), win);
    gtk_box_pack_start(GTK_BOX(ftb), tg_metro, FALSE, FALSE, 0);

    GtkWidget *tg_bars = gtk_toggle_button_new_with_label("Bars");
    gtk_widget_set_tooltip_text(tg_bars, "Ruler: bars/beats vs time");
    g_signal_connect(tg_bars, "toggled", G_CALLBACK(mw_bars_toggled), win);
    gtk_box_pack_start(GTK_BOX(ftb), tg_bars, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ftb), gtk_label_new("BPM"), FALSE, FALSE, 4);
    GtkWidget *bpm = gtk_spin_button_new_with_range(20.0, 999.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bpm),
                              jackdaw_project_get_bpm(project));
    g_signal_connect(bpm, "value-changed", G_CALLBACK(mw_bpm_changed), win);
    gtk_box_pack_start(GTK_BOX(ftb), bpm, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ftb), gtk_label_new("Sig"), FALSE, FALSE, 4);
    GtkWidget *ts_num = gtk_spin_button_new_with_range(1.0, 32.0, 1.0);
    GtkWidget *ts_den = gtk_spin_button_new_with_range(1.0, 32.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ts_num), 4.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ts_den), 4.0);
    g_object_set_data(G_OBJECT(win), "ts-num", ts_num);
    g_object_set_data(G_OBJECT(win), "ts-den", ts_den);
    g_signal_connect(ts_num, "value-changed", G_CALLBACK(mw_timesig_changed), win);
    g_signal_connect(ts_den, "value-changed", G_CALLBACK(mw_timesig_changed), win);
    gtk_box_pack_start(GTK_BOX(ftb), ts_num, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ftb), gtk_label_new("/"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ftb), ts_den, FALSE, FALSE, 0);

    GtkWidget *tg_mixer = gtk_toggle_button_new_with_label("Mixer");
    win->mixer_button = tg_mixer;
    g_signal_connect(tg_mixer, "toggled", G_CALLBACK(mw_mixer_toggled), win);
    gtk_box_pack_end(GTK_BOX(ftb), tg_mixer, FALSE, FALSE, 0);

    /* ---- Timeline + mixer dock (vertical paned) ---- */
    GtkWidget *tl_widget = jackdaw_timeline_new(project);
    win->timeline = JACKDAW_TIMELINE(tl_widget);
    win->mixer    = jackdaw_mixer_new(project);
    win->mixer_in_window = settings_get_uint32("mixer_in_window", 0) != 0;

    win->paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(win->paned), tl_widget,  TRUE,  FALSE);
    gtk_paned_pack2(GTK_PANED(win->paned), win->mixer, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), win->paned, TRUE, TRUE, 0);

    g_signal_connect(tl_widget, "position-changed",
                     G_CALLBACK(mw_on_position_changed), win);

    /* tracks_scroll auto-wraps tracks_box in a GtkViewport; connect there
       since GtkBox has no GdkWindow and never receives raw button events. */
    GtkWidget *tl_viewport = gtk_bin_get_child(GTK_BIN(win->timeline->tracks_scroll));
    gtk_widget_add_events(tl_viewport, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(tl_viewport, "button-press-event",
                     G_CALLBACK(mw_tracks_box_press_cb), win);

    win->transport_timer = g_timeout_add(100, mw_transport_timer, win);

    gtk_widget_show_all(GTK_WIDGET(win));
    /* Mixer hidden until toggled on */
    gtk_widget_hide(win->mixer);

    return GTK_WIDGET(win);
}
