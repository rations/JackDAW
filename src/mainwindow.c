#define _GNU_SOURCE
#include <config.h>
#include <string.h>
#include <math.h>

#include "mainwindow.h"
#include "tempomap.h"
#include "jackdaw-engine.h"
#include "audio_clip.h"
#include "clipregion.h"
#include "mixer.h"
#include "main.h"
#include "message.h"
#include "settings.h"
#include "fxwindow.h"
#include "pluginhost.h"
#include "midicontrol.h"
#include "render.h"
#include "render_dialog.h"

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
    /* Track strip selected for render ("Selected tracks" source). */
    "box.ts-selected {"
    "  background-color:#2d4a6b;"
    "  border:1px solid #5b9bd5; }"
    /* The single active/primary track — stronger highlight than ts-selected. */
    "box.ts-active {"
    "  background-color:#36608f;"
    "  border:1px solid #8ec3ff; }"
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

/* Refresh the window title from the project's current save state: the project
 * name (file basename without .jdaw) when saved, else "untitled". Call after
 * new/open/save so the title tracks the project. */
static void mw_update_title(JackDawMainWindow *win)
{
    const gchar *cur = jackdaw_project_get_file(win->project);
    gchar *name;
    if (cur) {
        gchar *base = g_path_get_basename(cur);
        if (g_str_has_suffix(base, ".jdaw"))
            base[strlen(base) - 5] = '\0';
        name = base;
    } else {
        name = g_strdup("untitled");
    }
    gchar *title = g_strdup_printf("%s — JackDAW " VERSION, name);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title);
    g_free(name);
}

/* ---- File menu ---- */

static void mw_new_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    /* Drop undo history first: its mementos reference the tracks we're about to
     * tear down and must not resurrect them into the empty project. */
    undo_manager_clear(jackdaw_project_get_undo(win->project));
    guint n = jackdaw_project_track_count(win->project);
    while (n-- > 0) {
        JackDawTrack *t = jackdaw_project_get_track(win->project, 0);
        jackdaw_engine_remove_track(t);
        jackdaw_project_remove_track(win->project, t);
    }
    win->track_counter = 0;
    jackdaw_project_set_file(win->project, NULL);
    mw_update_title(win);
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
                jackdaw_error(msg);
                g_free(msg);
                if (err) g_error_free(err);
            } else {
                win->track_counter++;
                gchar *name = g_strdup_printf("Track %u", win->track_counter);
                /* clip ownership transferred to track */
                JackDawTrack *t = jackdaw_track_new(name, clip);
                g_free(name);

                if (jackdaw_engine_add_track(t)) {
                    jackdaw_error("Engine: could not add track (slot limit reached)");
                    g_object_unref(t);
                } else {
                    jackdaw_project_push_structural_undo(win->project,
                                                         "Import audio track");
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
                jackdaw_error("Could not save project.");
            else
                mw_update_title(win);
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
            jackdaw_error("Could not save project.");
    } else {
        mw_save_as_project_cb(item, data);
    }
}

static void mw_render_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    render_dialog_open(GTK_WINDOW(win), win->project, RENDER_SCOPE_PROJECT);
}

static void mw_render_region_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    render_dialog_open(GTK_WINDOW(win), win->project, RENDER_SCOPE_REGION);
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
                jackdaw_error("Could not open project.");
            else {
                win->track_counter = jackdaw_project_track_count(win->project);
                mw_update_title(win);
            }
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
        jackdaw_error("Engine: could not add track (slot limit reached)");
        g_object_unref(t);
        return;
    }
    jackdaw_project_push_structural_undo(win->project, "Add track");
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
        jackdaw_error("Engine: could not add track (slot limit reached)");
        g_object_unref(t);
        return;
    }
    jackdaw_project_push_structural_undo(win->project, "Add MIDI track");
    jackdaw_project_add_track(win->project, t);
    g_object_unref(t);
}

static void mw_remove_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    JackDawTrack *t = jackdaw_project_get_active_track(win->project);
    if (!t) return;
    jackdaw_project_delete_track(win->project, t);
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
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (on) {
        guint beats = jackdaw_project_get_countin_before_play(win->project);
        if (!(beats > 0 && jackdaw_engine_begin_countin(beats, FALSE)))
            jackdaw_engine_start_playback();
    } else {
        jackdaw_engine_stop_playback();
    }
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
        guint beats = jackdaw_project_get_countin_before_record(win->project);
        if (!(beats > 0 && jackdaw_engine_begin_countin(beats, TRUE)))
            jackdaw_engine_start_recording();
        /* Recording engages ENGINE_PLAYING (now, or when the count-in ends);
         * light the play button to match WITHOUT firing its handler — otherwise
         * it would launch a second, playback count-in. */
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button))) {
            g_signal_handlers_block_by_func(win->play_button,
                                            mw_transport_play_cb, win);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), TRUE);
            mw_set_class(win->play_button, "transport-play", TRUE);
            g_signal_handlers_unblock_by_func(win->play_button,
                                              mw_transport_play_cb, win);
        }
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

static void mw_grid_unit_changed(GtkComboBox *c, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gint a = gtk_combo_box_get_active(c);
    if (a >= 0) jackdaw_project_set_grid_unit(win->project, a);
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

/* ---- Count-in window (metronome pre-roll before play / record) ----------- */

static gboolean mw_countin_window_delete_cb(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    return TRUE; /* keep the singleton alive */
}

static void mw_countin_record_changed(GtkSpinButton *sb, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_countin_before_record(
        win->project, (guint)gtk_spin_button_get_value_as_int(sb));
}

static void mw_countin_play_changed(GtkSpinButton *sb, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_countin_before_play(
        win->project, (guint)gtk_spin_button_get_value_as_int(sb));
}

/* Open (creating lazily) the count-in settings window. Two spin buttons: the
 * number of metronome clicks before recording, and before playback (0 = off). */
static void mw_open_countin_window(JackDawMainWindow *win)
{
    if (!win->countin_window) {
        win->countin_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win->countin_window), "Count In");
        gtk_window_set_resizable(GTK_WINDOW(win->countin_window), FALSE);
        gtk_window_set_transient_for(GTK_WINDOW(win->countin_window),
                                     GTK_WINDOW(win));
        g_signal_connect(win->countin_window, "delete-event",
                         G_CALLBACK(mw_countin_window_delete_cb), win);

        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
        gtk_container_add(GTK_CONTAINER(win->countin_window), grid);

        GtkWidget *l_rec = gtk_label_new("Count in before record:");
        gtk_widget_set_halign(l_rec, GTK_ALIGN_START);
        GtkWidget *sp_rec = gtk_spin_button_new_with_range(0, 32, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_rec),
            jackdaw_project_get_countin_before_record(win->project));
        g_signal_connect(sp_rec, "value-changed",
                         G_CALLBACK(mw_countin_record_changed), win);
        gtk_grid_attach(GTK_GRID(grid), l_rec,  0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), sp_rec, 1, 0, 1, 1);

        GtkWidget *l_play = gtk_label_new("Count in before playback:");
        gtk_widget_set_halign(l_play, GTK_ALIGN_START);
        GtkWidget *sp_play = gtk_spin_button_new_with_range(0, 32, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_play),
            jackdaw_project_get_countin_before_play(win->project));
        g_signal_connect(sp_play, "value-changed",
                         G_CALLBACK(mw_countin_play_changed), win);
        gtk_grid_attach(GTK_GRID(grid), l_play,  0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), sp_play, 1, 1, 1, 1);

        GtkWidget *hint = gtk_label_new(
            "Number of metronome clicks before transport starts (0 = off).");
        gtk_widget_set_halign(hint, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), hint, 0, 2, 2, 1);

        g_object_set_data(G_OBJECT(win->countin_window), "sp-rec",  sp_rec);
        g_object_set_data(G_OBJECT(win->countin_window), "sp-play", sp_play);
    }

    /* Re-sync to the current project (it may differ after loading a session). */
    {
        GtkWidget *sp_rec  =
            g_object_get_data(G_OBJECT(win->countin_window), "sp-rec");
        GtkWidget *sp_play =
            g_object_get_data(G_OBJECT(win->countin_window), "sp-play");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_rec),
            jackdaw_project_get_countin_before_record(win->project));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_play),
            jackdaw_project_get_countin_before_play(win->project));
    }

    gtk_widget_show_all(win->countin_window);
    gtk_window_present(GTK_WINDOW(win->countin_window));
}

/* Metro right-click menu: "Volume…" opens the slider window. */
static void mw_metro_volume_item_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    mw_open_metronome_window(JACKDAW_MAIN_WINDOW(data));
}

/* Metro right-click menu: "Count in…" opens the pre-roll settings window. */
static void mw_metro_countin_item_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    mw_open_countin_window(JACKDAW_MAIN_WINDOW(data));
}

/* Metro right-click menu: "Headphones only" toggles the routing mode. When on,
 * the click is emitted only on the dedicated "metronome" JACK port; when off it
 * is also mixed onto the main outputs. */
static void mw_metro_headphones_item_cb(GtkCheckMenuItem *m, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_set_metronome_route(
        win->project,
        gtk_check_menu_item_get_active(m) ? METRONOME_ROUTE_CLICK_PORT
                                          : METRONOME_ROUTE_MAIN);
}

/* Right-click on the Metro toolbar button opens its options menu. New
 * metronome features can be added here as additional menu items. */
static gboolean mw_metro_button_press_cb(GtkWidget *w, GdkEventButton *ev,
                                         gpointer data)
{
    (void)w;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *mi_vol = gtk_menu_item_new_with_label("Volume…");
    g_signal_connect(mi_vol, "activate",
                     G_CALLBACK(mw_metro_volume_item_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_vol);

    GtkWidget *mi_ci = gtk_menu_item_new_with_label("Count in…");
    g_signal_connect(mi_ci, "activate",
                     G_CALLBACK(mw_metro_countin_item_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_ci);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    GtkWidget *mi_hp = gtk_check_menu_item_new_with_label(
        "Headphones only (click output)");
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(mi_hp),
        jackdaw_project_get_metronome_route(win->project)
            == METRONOME_ROUTE_CLICK_PORT);
    g_signal_connect(mi_hp, "toggled",
                     G_CALLBACK(mw_metro_headphones_item_cb), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_hp);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE; /* don't toggle on right-click */
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
    /* One step = 10 ms (1/100 s).
     * Falls back to 480 (48000/100) when JACK is not connected. */
    jack_nframes_t sr = jackdaw_engine_is_running()
                        ? jackdaw_engine_get_sample_rate() : 48000u;
    return (off_t)(sr / 100);
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
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_undo(win->project);
}

static void mw_redo_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_project_redo(win->project);
}

/* Region operations on the Edit menu. All of these already had keyboard
 * bindings (mw_key_press) and context-menu entries on the timeline, but were
 * unreachable — and undiscoverable — from the menu bar. They act on the current
 * selection and the focused track, exactly as the key bindings do. */
static void mw_edit_copy_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_copy_selection(mw_timeline(GTK_WIDGET(data)));
}

static void mw_edit_paste_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_paste_at_cursor(mw_timeline(GTK_WIDGET(data)));
}

static void mw_edit_delete_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_delete_selection(mw_timeline(GTK_WIDGET(data)));
}

static void mw_edit_group_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_group_selection(mw_timeline(GTK_WIDGET(data)));
}

static void mw_edit_split_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_split_at_cursor(mw_timeline(GTK_WIDGET(data)));
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
    format_timecode(sr, (off_t)sample, (off_t)sample, tbuf, default_timescale_mode);
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
    /* Honour the ruler mode the user already chose with the Bars toggle: in
     * bars mode the readout is bars.beats.ticks, matching what the ruler shows.
     * It used to print timecode unconditionally, so the two disagreed. */
    if (win->project && win->project->ruler_mode == JACKDAW_RULER_BARS) {
        TempoMap tm;
        TempoMapBBT bbt;
        tempomap_from_project(&tm, win->project, sr);
        tempomap_frame_to_bbt(&tm, pos, &bbt);
        g_snprintf(tbuf, sizeof tbuf, "%u.%u.%03u", bbt.bar, bbt.beat, bbt.tick);
    } else {
        format_timecode(sr, pos, pos, tbuf, default_timescale_mode);
    }
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
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        mw_step_back_cb(NULL, win);
        return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        mw_step_forward_cb(NULL, win);
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
    /* Returning FALSE lets GTK destroy the window, which frees the project and
     * every plugin instance — while JACK is still active. Suspend the graph first
     * so the RT thread stops touching plugins before they are torn down. */
    jackdaw_engine_set_suspended(TRUE);
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

/* ---- MIDI control-surface window (Options -> MIDI Control) ---------------- */

static const char *const midictl_action_labels[] = {
    "Toggle FX Bypass",
    "Momentary FX Bypass",
    "Set Wet/Dry Mix",
    "Set FX Parameter",
    "Toggle Track Mute",
    "Switch Group (gapless)",
    "Transport: Play/Stop",
    "Transport: Stop",
    "Transport: Record",
};

/* Forward decls (the row callbacks and the rebuild are mutually recursive). */
static void mw_midictl_rebuild         (JackDawMainWindow *win);
static void mw_midictl_schedule_rebuild(JackDawMainWindow *win);
static void mw_midictl_populate_devices(JackDawMainWindow *win);

static void midictl_trigger_text(const MidiCtlMapping *m, char *buf, gsize n)
{
    if (m->msg_type == MIDI_CTL_UNLEARNED) {
        g_strlcpy(buf, "(unlearned)", n);
        return;
    }
    const char *t = m->msg_type == MIDI_CTL_CC      ? "CC"   :
                    m->msg_type == MIDI_CTL_NOTE    ? "Note" :
                    m->msg_type == MIDI_CTL_PROGRAM ? "PGM"  : "Bend";
    if (m->msg_type == MIDI_CTL_PITCHBEND) {
        if (m->channel < 0) g_snprintf(buf, n, "%s (any ch)", t);
        else                g_snprintf(buf, n, "%s ch%d", t, m->channel + 1);
    } else if (m->channel < 0) {
        g_snprintf(buf, n, "%s %d (any ch)", t, m->number);
    } else {
        g_snprintf(buf, n, "%s %d ch%d", t, m->number, m->channel + 1);
    }
}

/* Resolve a mapping's track from the live project (for the FX/param combos). */
static JackDawTrack *midictl_row_track(JackDawMainWindow *win, MidiCtlMapping *m)
{
    if (m->track_index < 0 ||
        (guint)m->track_index >= jackdaw_project_track_count(win->project))
        return NULL;
    return jackdaw_project_get_track(win->project, (guint)m->track_index);
}

static void mw_midictl_learn_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "row"));
    midicontrol_set_learn(i);
    gtk_button_set_label(b, "Press a control…");
}

static void mw_midictl_action_changed(GtkComboBox *c, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "row"));
    MidiCtlMapping *m = midicontrol_get((guint)i);
    if (!m) return;
    gint a = gtk_combo_box_get_active(c);
    if (a < 0) return;
    m->action = (guint8)a;
    if (a == ACT_SWITCH_GROUP && m->switch_group < 0) m->switch_group = 0;
    mw_midictl_schedule_rebuild(win);
}

static void mw_midictl_track_changed(GtkComboBox *c, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "row"));
    MidiCtlMapping *m = midicontrol_get((guint)i);
    if (!m) return;
    gint a = gtk_combo_box_get_active(c);   /* 0 = None */
    m->track_index = a - 1;
    m->fx_index    = -1;                     /* FX slot is track-relative */
    m->param_index = -1;
    mw_midictl_schedule_rebuild(win);
}

static void mw_midictl_fx_changed(GtkComboBox *c, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "row"));
    MidiCtlMapping *m = midicontrol_get((guint)i);
    if (!m) return;
    gint a = gtk_combo_box_get_active(c);   /* 0 = None */
    m->fx_index    = a - 1;
    m->param_index = -1;
    mw_midictl_schedule_rebuild(win);
}

static void mw_midictl_param_changed(GtkComboBox *c, gpointer data)
{
    (void)data;
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "row"));
    MidiCtlMapping *m = midicontrol_get((guint)i);
    if (!m) return;
    m->param_index = gtk_combo_box_get_active(c) - 1;  /* 0 = None */
}

static void mw_midictl_group_changed(GtkSpinButton *sb, gpointer data)
{
    (void)data;
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sb), "row"));
    MidiCtlMapping *m = midicontrol_get((guint)i);
    if (m) m->switch_group = gtk_spin_button_get_value_as_int(sb);
}

static void mw_midictl_remove_clicked(GtkButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gint i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "row"));
    midicontrol_remove((guint)i);
    mw_midictl_schedule_rebuild(win);
}

static void mw_midictl_add_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    midicontrol_add(NULL);
    mw_midictl_schedule_rebuild(win);
}

/* Build one mapping row's widgets. External strings (track / plugin / param
 * names) go through plain-text combo appends — never markup (security rule). */
static void mw_midictl_build_row(JackDawMainWindow *win, guint i)
{
    GtkWidget *box = g_object_get_data(G_OBJECT(win->midictl_window), "rows-box");
    MidiCtlMapping *m = midicontrol_get(i);
    if (!box || !m) return;

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    char tb[64];
    midictl_trigger_text(m, tb, sizeof tb);
    GtkWidget *trig = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(trig), tb);
    gtk_widget_set_size_request(trig, 120, -1);
    gtk_widget_set_halign(trig, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), trig, FALSE, FALSE, 0);

    GtkWidget *learn = gtk_button_new_with_label(
        midicontrol_get_learn() == (gint)i ? "Press a control…" : "Learn");
    g_object_set_data(G_OBJECT(learn), "row", GINT_TO_POINTER(i));
    g_signal_connect(learn, "clicked",
                     G_CALLBACK(mw_midictl_learn_clicked), win);
    gtk_box_pack_start(GTK_BOX(row), learn, FALSE, FALSE, 0);

    /* Action combo. set_active before connect so the resync isn't a real edit. */
    GtkWidget *act = gtk_combo_box_text_new();
    for (guint a = 0; a < G_N_ELEMENTS(midictl_action_labels); a++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(act),
                                       midictl_action_labels[a]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(act), m->action);
    g_object_set_data(G_OBJECT(act), "row", GINT_TO_POINTER(i));
    g_signal_connect(act, "changed",
                     G_CALLBACK(mw_midictl_action_changed), win);
    gtk_box_pack_start(GTK_BOX(row), act, FALSE, FALSE, 0);

    gboolean is_transport = (m->action >= ACT_TRANSPORT_PLAY);
    gboolean track_only   = (m->action == ACT_TOGGLE_MUTE ||
                             m->action == ACT_SWITCH_GROUP);

    /* Track combo (everything except transport targets a track). */
    if (!is_transport) {
        GtkWidget *trk = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(trk), "None");
        guint tc = jackdaw_project_track_count(win->project);
        for (guint t = 0; t < tc; t++) {
            JackDawTrack *tt = jackdaw_project_get_track(win->project, t);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(trk),
                                           jackdaw_track_get_name(tt));
        }
        gint sel = (m->track_index >= 0 && (guint)m->track_index < tc)
                   ? m->track_index + 1 : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(trk), sel);
        g_object_set_data(G_OBJECT(trk), "row", GINT_TO_POINTER(i));
        g_signal_connect(trk, "changed",
                         G_CALLBACK(mw_midictl_track_changed), win);
        gtk_box_pack_start(GTK_BOX(row), trk, FALSE, FALSE, 0);
    }

    /* FX slot combo (FX actions only). */
    if (!is_transport && !track_only) {
        GtkWidget *fx = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fx), "None");
        JackDawTrack *tt = midictl_row_track(win, m);
        guint fc = tt ? jackdaw_track_fx_count(tt) : 0;
        for (guint f = 0; f < fc; f++) {
            PluginInstance *inst = jackdaw_track_fx_get(tt, f);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fx),
                inst ? pluginhost_name(inst) : "?");
        }
        gint sel = (m->fx_index >= 0 && (guint)m->fx_index < fc)
                   ? m->fx_index + 1 : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(fx), sel);
        g_object_set_data(G_OBJECT(fx), "row", GINT_TO_POINTER(i));
        g_signal_connect(fx, "changed",
                         G_CALLBACK(mw_midictl_fx_changed), win);
        gtk_box_pack_start(GTK_BOX(row), fx, FALSE, FALSE, 0);
    }

    /* Parameter combo (Set FX Parameter only). */
    if (m->action == ACT_SET_PARAM) {
        GtkWidget *par = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(par), "None");
        JackDawTrack *tt = midictl_row_track(win, m);
        PluginInstance *inst = (tt && m->fx_index >= 0 &&
                                (guint)m->fx_index < jackdaw_track_fx_count(tt))
                               ? jackdaw_track_fx_get(tt, (guint)m->fx_index)
                               : NULL;
        guint pc = inst ? pluginhost_param_count(inst) : 0;
        for (guint pi = 0; pi < pc && pi < 512; pi++) {
            const char *pn = pluginhost_param_name(inst, pi);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(par),
                                           pn ? pn : "?");
        }
        gint sel = (m->param_index >= 0 && (guint)m->param_index < pc)
                   ? m->param_index + 1 : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(par), sel);
        g_object_set_data(G_OBJECT(par), "row", GINT_TO_POINTER(i));
        g_signal_connect(par, "changed",
                         G_CALLBACK(mw_midictl_param_changed), win);
        gtk_box_pack_start(GTK_BOX(row), par, FALSE, FALSE, 0);
    }

    /* Switch-group selector (Switch Group only). */
    if (m->action == ACT_SWITCH_GROUP) {
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Group"),
                           FALSE, FALSE, 0);
        GtkWidget *grp = gtk_spin_button_new_with_range(0, 63, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(grp),
                                  m->switch_group >= 0 ? m->switch_group : 0);
        g_object_set_data(G_OBJECT(grp), "row", GINT_TO_POINTER(i));
        g_signal_connect(grp, "value-changed",
                         G_CALLBACK(mw_midictl_group_changed), win);
        gtk_box_pack_start(GTK_BOX(row), grp, FALSE, FALSE, 0);
    }

    GtkWidget *rm = gtk_button_new_with_label("✕");
    gtk_widget_set_tooltip_text(rm, "Remove mapping");
    g_object_set_data(G_OBJECT(rm), "row", GINT_TO_POINTER(i));
    g_signal_connect(rm, "clicked",
                     G_CALLBACK(mw_midictl_remove_clicked), win);
    gtk_box_pack_end(GTK_BOX(row), rm, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

static void mw_midictl_rebuild(JackDawMainWindow *win)
{
    if (!win->midictl_window) return;
    GtkWidget *box = g_object_get_data(G_OBJECT(win->midictl_window), "rows-box");
    if (!box) return;

    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    guint n = midicontrol_count();
    for (guint i = 0; i < n; i++)
        mw_midictl_build_row(win, i);

    gtk_widget_show_all(box);
}

/* Rebuilding tears down the very widget whose signal we're handling, so defer
 * it to an idle (one pending rebuild at a time). */
static gboolean mw_midictl_rebuild_idle(gpointer data)
{
    JackDawMainWindow *win = data;
    if (JACKDAW_IS_MAIN_WINDOW(win) && win->midictl_window) {
        g_object_set_data(G_OBJECT(win->midictl_window),
                          "rebuild-pending", NULL);
        mw_midictl_rebuild(win);
    }
    return G_SOURCE_REMOVE;
}

static void mw_midictl_schedule_rebuild(JackDawMainWindow *win)
{
    if (!win->midictl_window) return;
    if (g_object_get_data(G_OBJECT(win->midictl_window), "rebuild-pending"))
        return;
    g_object_set_data(G_OBJECT(win->midictl_window),
                      "rebuild-pending", GINT_TO_POINTER(1));
    g_idle_add(mw_midictl_rebuild_idle, win);
}

/* midicontrol fires this after a learn capture: refresh the list (which also
 * re-labels the armed Learn button back to "Learn"). */
static void mw_midictl_changed_cb(gpointer data)
{
    mw_midictl_schedule_rebuild(JACKDAW_MAIN_WINDOW(data));
}

/* midicontrol transport hook: drive the toolbar buttons so the UI stays in
 * sync (mirrors the Transport menu callbacks). */
static void mw_midictl_transport(int which, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    switch (which) {
    case ACT_TRANSPORT_PLAY: {
        GtkToggleButton *b = GTK_TOGGLE_BUTTON(win->play_button);
        gtk_toggle_button_set_active(b, !gtk_toggle_button_get_active(b));
        break;
    }
    case ACT_TRANSPORT_REC: {
        GtkToggleButton *b = GTK_TOGGLE_BUTTON(win->record_button);
        gtk_toggle_button_set_active(b, !gtk_toggle_button_get_active(b));
        break;
    }
    case ACT_TRANSPORT_STOP:
        jackdaw_engine_stop_playback();
        jackdaw_engine_stop_recording();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
        break;
    }
}

static void mw_midictl_device_changed(GtkComboBox *c, gpointer data)
{
    (void)data;
    gchar *txt = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
    if (!txt) return;
    if (g_strcmp0(txt, "None") == 0) {
        jackdaw_engine_set_control_source(NULL);
        settings_set_string("control_in_source", "");
    } else {
        jackdaw_engine_set_control_source(txt);
        settings_set_string("control_in_source", txt);
    }
    settings_save();
    g_free(txt);
}

static void mw_midictl_populate_devices(JackDawMainWindow *win)
{
    GtkWidget *combo =
        g_object_get_data(G_OBJECT(win->midictl_window), "dev-combo");
    if (!combo) return;

    g_signal_handlers_block_by_func(combo,
        G_CALLBACK(mw_midictl_device_changed), win);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "None");

    const gchar *cur   = jackdaw_engine_get_control_source();
    gchar       *saved = settings_get_string("control_in_source", "");
    const gchar *want  = (cur && *cur) ? cur :
                         (saved && *saved ? saved : NULL);

    gint sel = 0, idx = 1;
    gchar **srcs = jackdaw_engine_list_midi_sources();
    if (srcs) {
        for (gchar **s = srcs; *s; s++, idx++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), *s);
            if (want && g_strcmp0(*s, want) == 0) sel = idx;
        }
        g_strfreev(srcs);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), sel);
    g_free(saved);
    g_signal_handlers_unblock_by_func(combo,
        G_CALLBACK(mw_midictl_device_changed), win);
}

static void mw_midictl_ports_changed(JackDawProject *p, gpointer data)
{
    (void)p;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (win->midictl_window) mw_midictl_populate_devices(win);
}

static gboolean mw_midictl_window_delete_cb(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    midicontrol_set_learn(-1);   /* disarm any pending learn */
    return TRUE;                 /* keep the singleton alive */
}

static void mw_open_midictl_window(JackDawMainWindow *win)
{
    if (!win->midictl_window) {
        win->midictl_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win->midictl_window), "MIDI Control");
        gtk_window_set_default_size(GTK_WINDOW(win->midictl_window), 680, 380);
        gtk_window_set_transient_for(GTK_WINDOW(win->midictl_window),
                                     GTK_WINDOW(win));
        g_signal_connect(win->midictl_window, "delete-event",
                         G_CALLBACK(mw_midictl_window_delete_cb), win);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
        gtk_container_add(GTK_CONTAINER(win->midictl_window), vbox);

        GtkWidget *drow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(drow), gtk_label_new("Control device:"),
                           FALSE, FALSE, 0);
        GtkWidget *dev = gtk_combo_box_text_new();
        g_object_set_data(G_OBJECT(win->midictl_window), "dev-combo", dev);
        g_signal_connect(dev, "changed",
                         G_CALLBACK(mw_midictl_device_changed), win);
        gtk_box_pack_start(GTK_BOX(drow), dev, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), drow, FALSE, FALSE, 0);

        GtkWidget *hint = gtk_label_new(
            "Connect a footswitch / controller above, then map its buttons and "
            "pedals below. Click Learn and move the control to bind it.");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_widget_set_halign(hint, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
        GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_add(GTK_CONTAINER(scroll), rows);
        g_object_set_data(G_OBJECT(win->midictl_window), "rows-box", rows);

        GtkWidget *add = gtk_button_new_with_label("Add Mapping");
        gtk_widget_set_halign(add, GTK_ALIGN_START);
        g_signal_connect(add, "clicked",
                         G_CALLBACK(mw_midictl_add_clicked), win);
        gtk_box_pack_start(GTK_BOX(vbox), add, FALSE, FALSE, 0);

        /* Refresh the device list when JACK ports come and go; refresh the row
         * list when a learn capture lands. Connected once, for the app's life. */
        g_signal_connect(win->project, "ports-changed",
                         G_CALLBACK(mw_midictl_ports_changed), win);
        midicontrol_set_changed_cb(mw_midictl_changed_cb, win);
    }

    mw_midictl_populate_devices(win);
    mw_midictl_rebuild(win);
    gtk_widget_show_all(win->midictl_window);
    gtk_window_present(GTK_WINDOW(win->midictl_window));
}

static void mw_midictl_menu_cb(GtkMenuItem *m, gpointer data)
{
    (void)m;
    mw_open_midictl_window(JACKDAW_MAIN_WINDOW(data));
}

/* Drain control-surface events queued by the RT thread and dispatch them. */
static gboolean mw_midictl_timer(gpointer data)
{
    JackDawMainWindow *win = data;
    if (!JACKDAW_IS_MAIN_WINDOW(win)) return G_SOURCE_REMOVE;
    JackDawCtlEvent ev;
    while (jackdaw_engine_control_poll(&ev))
        midicontrol_dispatch_event(win->project, ev.data, ev.size);
    return G_SOURCE_CONTINUE;
}

/* ---- GObject boilerplate ---- */

static void jackdaw_main_window_finalize(GObject *obj)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(obj);
    if (win->transport_timer) {
        g_source_remove(win->transport_timer);
        win->transport_timer = 0;
    }
    if (win->midictl_timer) {
        g_source_remove(win->midictl_timer);
        win->midictl_timer = 0;
    }
    midicontrol_set_changed_cb(NULL, NULL);
    midicontrol_set_transport_cb(NULL, NULL);
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
    win->midictl_window  = NULL;
    win->midictl_timer   = 0;
}

/* ---- Constructor ---- */

GtkWidget *jackdaw_main_window_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawMainWindow *win =
        g_object_new(JACKDAW_TYPE_MAIN_WINDOW, NULL);

    win->project = g_object_ref(project);

    mw_update_title(win);
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
    menu_item(m, "_Render…",
              G_CALLBACK(mw_render_cb), win, 0, 0, ag);
    menu_item(m, "Render Re_gion…",
              G_CALLBACK(mw_render_region_cb), win, 0, 0, ag);
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
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    /* Ctrl+C / Ctrl+V are shown as hints rather than registered as accelerators:
     * mw_key_press deliberately declines them while a text entry has focus so
     * entries keep their own clipboard, and a real accelerator would fire in
     * that case anyway and copy the timeline selection out from under the user. */
    menu_item(m, "_Copy  [Ctrl+C]",
              G_CALLBACK(mw_edit_copy_cb), win, 0, 0, ag);
    menu_item(m, "_Paste at Playhead  [Ctrl+V]",
              G_CALLBACK(mw_edit_paste_cb), win, 0, 0, ag);
    menu_item(m, "_Delete Selected Area",
              G_CALLBACK(mw_edit_delete_cb), win, 0, 0, ag);
    menu_item(m, "_Group Sections  [G]",
              G_CALLBACK(mw_edit_group_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "Split at Play_head  [S]",
              G_CALLBACK(mw_edit_split_cb), win, 0, 0, ag);

    /* Track */
    m = make_submenu_item(menubar, "_Track");
    menu_item(m, "_Add Empty Track",
              G_CALLBACK(mw_add_track_cb), win,
              GDK_KEY_t, GDK_CONTROL_MASK, ag);
    menu_item(m, "Add _MIDI Track",
              G_CALLBACK(mw_add_instrument_track_cb), win,
              GDK_KEY_t, GDK_CONTROL_MASK | GDK_SHIFT_MASK, ag);
    menu_item(m, "_Load File as New Track…",
              G_CALLBACK(mw_load_file_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_Delete Active Track",
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
    menu_item(m, "_MIDI Control…",
              G_CALLBACK(mw_midictl_menu_cb), win, 0, 0, ag);

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
    gtk_widget_set_tooltip_text(btn_step_back, "Step back 10 ms (Left arrow)");
    g_signal_connect(btn_step_back, "clicked",
                     G_CALLBACK(mw_step_back_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_step_back, FALSE, FALSE, 0);

    GtkWidget *btn_step_fwd = gtk_button_new_with_label(">>|");
    gtk_widget_set_tooltip_text(btn_step_fwd, "Step forward 10 ms (Right arrow)");
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

    /* Grid/snap resolution. Snapping was fixed at whole beats before this. */
    GtkWidget *grid_unit = gtk_combo_box_text_new();
    for (int gi = 0; gi < TEMPOMAP_GRID_LAST; gi++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(grid_unit),
                                       tempomap_grid_name((TempoMapGrid)gi));
    gtk_combo_box_set_active(GTK_COMBO_BOX(grid_unit),
                             jackdaw_project_get_grid_unit(project));
    gtk_widget_set_tooltip_text(grid_unit, "Grid / snap resolution");
    g_signal_connect(grid_unit, "changed",
                     G_CALLBACK(mw_grid_unit_changed), win);
    gtk_box_pack_start(GTK_BOX(ftb), grid_unit, FALSE, FALSE, 0);

    GtkWidget *tg_metro = gtk_toggle_button_new_with_label("Metro");
    gtk_widget_set_tooltip_text(tg_metro,
        "Toggle metronome  (right-click for options)");
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

    /* Control-surface drain: poll the RT->main ring at ~15 ms so a footswitch
     * press feels immediate, and route transport actions through the toolbar. */
    midicontrol_set_transport_cb(mw_midictl_transport, win);
    win->midictl_timer = g_timeout_add(15, mw_midictl_timer, win);

    gtk_widget_show_all(GTK_WIDGET(win));
    /* Mixer hidden until toggled on */
    gtk_widget_hide(win->mixer);

    return GTK_WIDGET(win);
}
