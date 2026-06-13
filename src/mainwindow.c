#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#include "mainwindow.h"
#include "jackdaw-engine.h"
#include "audio_clip.h"
#include "clipregion.h"
#include "mixer.h"
#include "main.h"
#include "um.h"

G_DEFINE_TYPE(JackDawMainWindow, jackdaw_main_window, GTK_TYPE_WINDOW)

/* ---- helpers ---- */

static JackDawTimeline *mw_timeline(GtkWidget *widget)
{
    return JACKDAW_MAIN_WINDOW(widget)->timeline;
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

static void mw_save_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Project", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    const gchar *cur = jackdaw_project_get_file(win->project);
    if (cur) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    else     gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "untitled.jdaw");

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

static void mw_transport_record_cb(GtkWidget *widget, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (on) {
        jackdaw_engine_start_recording();
        /* start_recording sets ENGINE_PLAYING; keep the play button in sync */
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), TRUE);
    } else {
        jackdaw_engine_stop_recording();
    }
    mw_set_class(widget, "transport-rec", on);
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

static void mw_mixer_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (!win->mixer) return;
    gtk_widget_set_visible(win->mixer, gtk_toggle_button_get_active(b));
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
    win->time_label      = NULL;
    win->mixer           = NULL;
    win->paned           = NULL;
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

    /* Transport button state colours (play = green, record = red). */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            /* Readable controls: dark text, visible button outlines. */
            "button { color:#101010; border:1px solid #808080; }"
            "button:checked { background-image:none; background-color:#b8c4d8;"
            "  color:#101010; }"
            "spinbutton, spinbutton entry { color:#101010; }"
            "label { color:#101010; }"
            /* Transport state colours override the generic button rules. */
            "button.transport-play {"
            "  background-image:none; background-color:#2e8b57; color:#ffffff; }"
            "button.transport-rec  {"
            "  background-image:none; background-color:#c0392b; color:#ffffff; }"
            "label.transport-time  {"
            "  font-size:22px; font-weight:bold; font-family:monospace; }",
            -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

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
    menu_item(m, "_Load File as New Track…",
              G_CALLBACK(mw_load_file_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_Open Project…",
              G_CALLBACK(mw_open_project_cb), win,
              GDK_KEY_o, GDK_CONTROL_MASK, ag);
    menu_item(m, "_Save Project…",
              G_CALLBACK(mw_save_project_cb), win,
              GDK_KEY_s, GDK_CONTROL_MASK, ag);
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
    menu_item(m, "_Remove Focused Track",
              G_CALLBACK(mw_remove_track_cb), win, 0, 0, ag);

    /* Transport */
    m = make_submenu_item(menubar, "T_ransport");
    menu_item(m, "_Play / Stop  [Space]",
              NULL, NULL, 0, 0, ag);
    menu_item(m, "_Stop",
              G_CALLBACK(mw_transport_stop_cb), win, 0, 0, ag);
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

    GtkWidget *btn_pause = gtk_button_new_with_label("⏸");
    g_signal_connect(btn_pause, "clicked", G_CALLBACK(mw_pause_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_pause, FALSE, FALSE, 0);

    GtkWidget *btn_stop = gtk_button_new_with_label("■");
    g_signal_connect(btn_stop, "clicked",
                     G_CALLBACK(mw_transport_stop_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_stop, FALSE, FALSE, 0);

    win->record_button = gtk_toggle_button_new_with_label("⏺");
    g_signal_connect(win->record_button, "toggled",
                     G_CALLBACK(mw_transport_record_cb), win);
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
    g_signal_connect(tg_metro, "toggled", G_CALLBACK(mw_metro_toggled), win);
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
    g_signal_connect(tg_mixer, "toggled", G_CALLBACK(mw_mixer_toggled), win);
    gtk_box_pack_end(GTK_BOX(ftb), tg_mixer, FALSE, FALSE, 0);

    /* ---- Timeline + mixer dock (vertical paned) ---- */
    GtkWidget *tl_widget = jackdaw_timeline_new(project);
    win->timeline = JACKDAW_TIMELINE(tl_widget);
    win->mixer    = jackdaw_mixer_new(project);

    win->paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(win->paned), tl_widget,  TRUE,  FALSE);
    gtk_paned_pack2(GTK_PANED(win->paned), win->mixer, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), win->paned, TRUE, TRUE, 0);

    g_signal_connect(tl_widget, "position-changed",
                     G_CALLBACK(mw_on_position_changed), win);

    win->transport_timer = g_timeout_add(100, mw_transport_timer, win);

    gtk_widget_show_all(GTK_WIDGET(win));
    /* Mixer hidden until toggled on */
    gtk_widget_hide(win->mixer);

    return GTK_WIDGET(win);
}
