#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#include "fxwindow.h"
#include "pluginhost.h"
#include "settings.h"
#include "jackdaw-engine.h"

/* ======================================================================
 * Plugin browser — categorised picker, returns a chosen PluginInfo*.
 * ====================================================================== */

enum { COL_LABEL, COL_INFO, N_COLS };

static PluginInfo *fx_browse_for_plugin(GtkWindow *parent)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Add Effect", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 460);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkTreeStore *store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_POINTER);

    /* Group catalog by "Format · Category". */
    const GList *cat = pluginhost_catalog();
    GHashTable *cats = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    for (const GList *l = cat; l; l = l->next) {
        PluginInfo *pi = l->data;
        /* Display instruments as "MIDI" (user-facing term); the internal
         * category string ("Instrument|…") still drives type detection. */
        const char *catdisp = pi->is_instrument ? "MIDI" : pi->category;
        gchar *grp = g_strdup_printf("%s \302\267 %s",
            pluginhost_format_name(pi->format), catdisp);
        GtkTreeIter *parent = g_hash_table_lookup(cats, grp);
        if (!parent) {
            parent = g_new0(GtkTreeIter, 1);
            gtk_tree_store_append(store, parent, NULL);
            gtk_tree_store_set(store, parent, COL_LABEL, grp, COL_INFO, NULL, -1);
            g_hash_table_insert(cats, g_strdup(grp), parent);
        }
        GtkTreeIter child;
        gtk_tree_store_append(store, &child, parent);
        gtk_tree_store_set(store, &child, COL_LABEL, pi->name, COL_INFO, pi, -1);
        g_free(grp);
    }
    /* free the GtkTreeIter values we stored */
    GHashTableIter hit; gpointer hk, hv;
    g_hash_table_iter_init(&hit, cats);
    while (g_hash_table_iter_next(&hit, &hk, &hv)) g_free(hv);
    g_hash_table_destroy(cats);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Plugin", r, "text", COL_LABEL, NULL));
    gtk_container_add(GTK_CONTAINER(scroll), tv);

    if (cat == NULL) {
        GtkWidget *empty = gtk_label_new(
            "No plugins found. Use Plugins\342\200\246 to add a folder and Rescan.");
        gtk_box_pack_start(GTK_BOX(box), empty, FALSE, FALSE, 4);
    }

    gtk_widget_show_all(dlg);

    PluginInfo *chosen = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));
        GtkTreeIter it; GtkTreeModel *m;
        if (gtk_tree_selection_get_selected(sel, &m, &it))
            gtk_tree_model_get(m, &it, COL_INFO, &chosen, -1);
    }
    gtk_widget_destroy(dlg);
    return chosen;   /* borrowed from the catalog; do not free */
}

/* ======================================================================
 * Paths / rescan dialog
 * ====================================================================== */

typedef struct {
    GtkWidget *combo;
    GtkWidget *list;
    GtkWidget *dialog;
} PathsUI;

static PluginFormat paths_fmt(PathsUI *ui)
{
    int a = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->combo));
    return (a >= 0 && a < PH_NFORMATS) ? (PluginFormat)a : PH_LV2;
}

static void paths_refresh(PathsUI *ui)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(ui->list));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
    g_list_free(kids);
    for (const GList *l = pluginhost_search_paths(paths_fmt(ui)); l; l = l->next) {
        GtkWidget *row = gtk_label_new((const char *)l->data);
        gtk_widget_set_halign(row, GTK_ALIGN_START);
        gtk_list_box_insert(GTK_LIST_BOX(ui->list), row, -1);
    }
    gtk_widget_show_all(ui->list);
}

static void paths_combo_changed(GtkComboBox *c, gpointer data)
{ (void)c; paths_refresh(data); }

static void paths_add_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    PathsUI *ui = data;
    GtkWidget *fc = gtk_file_chooser_dialog_new("Add Plugin Folder",
        GTK_WINDOW(ui->dialog), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (dir) {
            pluginhost_add_search_path(paths_fmt(ui), dir);
            pluginhost_save_paths_to_settings();
            g_free(dir);
            paths_refresh(ui);
        }
    }
    gtk_widget_destroy(fc);
}

static void paths_remove_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    PathsUI *ui = data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->list));
    if (!row) return;
    GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(row));
    const char *dir = lbl ? gtk_label_get_text(GTK_LABEL(lbl)) : NULL;
    if (dir) {
        pluginhost_remove_search_path(paths_fmt(ui), dir);
        pluginhost_save_paths_to_settings();
        paths_refresh(ui);
    }
}

/* ----------------------------------------------------------------------
 * Scan progress dialog — shown while plugins are (re)scanned. Scanning runs
 * synchronously on the main thread, so the per-plugin progress callback pulses
 * the bar and pumps the GTK loop to keep this window painted and responsive.
 * ---------------------------------------------------------------------- */

typedef struct {
    GtkWidget *win;
    GtkWidget *bar;
    GtkWidget *lbl;
} ScanProg;

static void scan_prog_cb(const char *plugin, void *user)
{
    ScanProg *sp = user;
    const char *base = strrchr(plugin, '/');
    base = base ? base + 1 : plugin;
    gtk_label_set_text(GTK_LABEL(sp->lbl), base);   /* external string: set_text */
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(sp->bar));
    while (gtk_events_pending()) gtk_main_iteration();
}

static ScanProg *scan_prog_begin(GtkWindow *parent)
{
    ScanProg *sp = g_new0(ScanProg, 1);
    sp->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sp->win), "Scanning Plugins");
    gtk_window_set_modal(GTK_WINDOW(sp->win), TRUE);
    gtk_window_set_deletable(GTK_WINDOW(sp->win), FALSE);
    gtk_window_set_position(GTK_WINDOW(sp->win),
                            parent ? GTK_WIN_POS_CENTER_ON_PARENT
                                   : GTK_WIN_POS_CENTER);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(sp->win), parent);
    gtk_window_set_default_size(GTK_WINDOW(sp->win), 360, -1);
    gtk_container_set_border_width(GTK_CONTAINER(sp->win), 12);

    GtkWidget *box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *head = gtk_label_new("Scanning for plugins\342\200\246");
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    sp->bar = gtk_progress_bar_new();
    sp->lbl = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(sp->lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(sp->lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), head,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sp->bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sp->lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(sp->win), box);
    gtk_widget_show_all(sp->win);

    pluginhost_set_scan_progress(scan_prog_cb, sp);
    while (gtk_events_pending()) gtk_main_iteration();
    return sp;
}

static void scan_prog_end(ScanProg *sp)
{
    pluginhost_set_scan_progress(NULL, NULL);
    gtk_widget_destroy(sp->win);
    g_free(sp);
}

static void paths_rescan_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    PathsUI *ui = data;

    ScanProg *sp = scan_prog_begin(GTK_WINDOW(ui->dialog));
    pluginhost_rescan();
    scan_prog_end(sp);

    guint n[PH_NFORMATS] = {0}, total = 0;
    for (const GList *l = pluginhost_catalog(); l; l = l->next) {
        PluginInfo *pi = (PluginInfo *)l->data;
        if (pi->format < PH_NFORMATS) n[pi->format]++;
        total++;
    }
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(ui->dialog),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Found %u plugin%s:\n\nLV2: %u\nVST2: %u\nVST3: %u\nCLAP: %u\nLADSPA: %u",
        total, total == 1 ? "" : "s",
        n[PH_LV2], n[PH_VST2], n[PH_VST3], n[PH_CLAP], n[PH_LADSPA]);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

void jackdaw_fx_paths_dialog(GtkWindow *parent)
{
    PathsUI ui = { 0 };
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Plugin Paths", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    ui.dialog = dlg;
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 360);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);

    ui.combo = gtk_combo_box_text_new();
    for (int f = 0; f < PH_NFORMATS; f++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.combo),
                                       pluginhost_format_name((PluginFormat)f));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.combo), 0);
    g_signal_connect(ui.combo, "changed", G_CALLBACK(paths_combo_changed), &ui);
    gtk_box_pack_start(GTK_BOX(box), ui.combo, FALSE, FALSE, 2);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    ui.list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), ui.list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 2);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *add  = gtk_button_new_with_label("Add Folder\342\200\246");
    GtkWidget *del  = gtk_button_new_with_label("Remove");
    GtkWidget *scan = gtk_button_new_with_label("Scan");
    g_signal_connect(add,  "clicked", G_CALLBACK(paths_add_clicked),    &ui);
    g_signal_connect(del,  "clicked", G_CALLBACK(paths_remove_clicked), &ui);
    g_signal_connect(scan, "clicked", G_CALLBACK(paths_rescan_clicked), &ui);
    gtk_box_pack_start(GTK_BOX(btns), add,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), del,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 2);

    gtk_widget_show_all(dlg);
    paths_refresh(&ui);

    gtk_dialog_run(GTK_DIALOG(dlg));   /* helper buttons act via their callbacks */
    gtk_widget_destroy(dlg);
}

void jackdaw_fx_startup_scan(GtkWindow *parent)
{
    ScanProg *sp = scan_prog_begin(parent);
    GList *added = pluginhost_scan_report_new();
    scan_prog_end(sp);
    if (!added) return;

    guint n = g_list_length(added);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "New Plugins", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, 320);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *head = gtk_label_new(NULL);
    char *t = g_strdup_printf("%u new plugin%s found:", n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(head), t);
    g_free(t);
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 4);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    for (GList *l = added; l; l = l->next) {
        GtkWidget *row = gtk_label_new(NULL);
        gtk_label_set_text(GTK_LABEL(row), (const char *)l->data);  /* external */
        gtk_label_set_xalign(GTK_LABEL(row), 0.0);
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
    }
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 4);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_list_free_full(added, g_free);
}

/* ======================================================================
 * FX window (per track)
 * ====================================================================== */

typedef struct {
    JackDawTrack   *track;
    JackDawProject *project;
    GtkWidget      *window;
    GtkWidget      *list_box;    /* chain rows */
    GtkWidget      *gui_holder;  /* shows the selected effect's GUI */
    GtkWidget      *shown;       /* currently displayed GUI (owned by instance) */
    GtkWidget      *mix_scale;   /* wet/dry for the selected effect */
    GtkWidget      *mix_row;     /* the dry/wet header (hidden when none selected) */
    guint           sel_index;   /* index of the selected effect */
    guint           fit_id;      /* deferred "fit window to plugin" timer */
    int             fit_ticks;   /* retries left to catch late-negotiating UIs */
    int             fit_stable;  /* consecutive ticks at an unchanged size */
    int             fit_w, fit_h;/* last natural size we resized to */
    gboolean        drop_active; /* a reorder drag is hovering the list */
    gint            drop_y;      /* insertion-line y in list_box coords */
} FxWindow;

static void fxwin_rebuild_list(FxWindow *fw);

/* Release (free) every effect's native editor before the window dies. Like jalv
 * (jalv_close -> suil_instance_free), the suil instance and its idle/push timers
 * MUST be torn down before the containing window is destroyed — keeping an
 * unparented editor alive leaves those timers running on a dead widget and
 * crashes. The plugin DSP instances stay alive; reopening rebuilds the editors
 * via pluginhost_make_gui. */
static void fxwin_release_all(FxWindow *fw)
{
    guint n = jackdaw_track_fx_count(fw->track);
    for (guint i = 0; i < n; i++)
        pluginhost_release_gui(jackdaw_track_fx_get(fw->track, i));
    fw->shown = NULL;
}

static void fxwin_mix_changed(GtkRange *r, gpointer data)
{
    FxWindow *fw = data;
    gpointer inst = jackdaw_track_fx_get(fw->track, fw->sel_index);
    if (inst) pluginhost_set_mix((PluginInstance *)inst,
                                 (float)gtk_range_get_value(r));
}

/* Resize the FX window to the currently shown plugin's natural size. Native UIs
 * (in-process suil, or the out-of-process bridge whose WID + size arrives a beat
 * later) report 1x1 until their embedded window negotiates, so we retry for a
 * short while and stop once the size holds steady. A GtkWindow never shrinks to
 * its content on its own, so this is what makes switching between differently
 * sized plugins re-fit instead of stretching the last (largest) size. */
static gboolean fxwin_fit_cb(gpointer data)
{
    FxWindow *fw = data;
    if (!fw->window || !fw->shown) { fw->fit_id = 0; return G_SOURCE_REMOVE; }

    /* Wait for the editor to report a real, steady natural size before touching
     * the window. A suil X11 editor reports ~1x1 until its embedded window comes
     * up; resizing to that would collapse the window (blank). We impose NO fixed
     * size floor — that forced short UIs (e.g. gxtuner's tuner) to a wrong height
     * and crashed their drawing. While the editor is still 1x1 we just leave the
     * window where it is, so FILL gives the UI room to negotiate; once it reports
     * a real size we resize ONCE and pin it to CENTER (so a later switch can't
     * stretch a fixed UI, and window resizes never churn it). */
    GtkRequisition cn;
    gtk_widget_get_preferred_size(fw->shown, NULL, &cn);
    gboolean real   = (cn.width >= 32 && cn.height >= 32);
    gboolean steady = (cn.width == fw->fit_w && cn.height == fw->fit_h);
    fw->fit_w = cn.width; fw->fit_h = cn.height;

    if ((!real || !steady) && --fw->fit_ticks > 0)
        return G_SOURCE_CONTINUE;   /* not negotiated/steady yet — keep waiting */

    if (real) {
        GtkWidget *content = gtk_bin_get_child(GTK_BIN(fw->window));  /* the paned */
        GtkRequisition nat;
        gtk_widget_get_preferred_size(content, NULL, &nat);
        if (nat.width > 0 && nat.height > 0)
            gtk_window_resize(GTK_WINDOW(fw->window), nat.width, nat.height);
        gtk_widget_set_halign(fw->shown, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(fw->shown, GTK_ALIGN_CENTER);
    }
    fw->fit_id = 0;
    return G_SOURCE_REMOVE;
}

static void fxwin_fit_later(FxWindow *fw)
{
    fw->fit_ticks = 12;   /* ~1.2s cap; stops early once the size is steady */
    fw->fit_stable = 0;
    fw->fit_w = fw->fit_h = -1;
    if (!fw->fit_id) fw->fit_id = g_timeout_add(100, fxwin_fit_cb, fw);
}

static void fxwin_show_gui(FxWindow *fw, guint index)
{
    gpointer inst = jackdaw_track_fx_get(fw->track, index);
    if (!inst) {
        if (fw->mix_row) gtk_widget_hide(fw->mix_row);
        return;
    }
    fw->sel_index = index;

    /* Reflect this effect's wet/dry without retriggering the change handler. */
    if (fw->mix_scale) {
        g_signal_handlers_block_by_func(fw->mix_scale, fxwin_mix_changed, fw);
        gtk_range_set_value(GTK_RANGE(fw->mix_scale),
                            pluginhost_get_mix((PluginInstance *)inst));
        g_signal_handlers_unblock_by_func(fw->mix_scale, fxwin_mix_changed, fw);
        gtk_widget_show_all(fw->mix_row);
    }

    /* Editors live in a GtkStack: add each one ONCE, then just switch the
     * visible child. This never reparents a live native UI. */
    GtkWidget *gui = pluginhost_make_gui((PluginInstance *)inst);
    if (gtk_widget_get_parent(gui) != fw->gui_holder) {
        if (gtk_widget_get_parent(gui))
            gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(gui)), gui);
        /* Start FILL + expand, exactly like jalv (jalv_gtk.c embeds the suil
         * widget GTK_ALIGN_FILL/expand): many X11/Gtk3 UIs report a 1x1 natural
         * size until their embedded window is given a real allocation to
         * negotiate against — without FILL the (non-homogeneous) stack would lock
         * them at 1x1 and they never display. Once the UI has negotiated a real,
         * steady size, fxwin_fit_cb flips it to CENTER so that switching back to
         * it (while the window is briefly sized for a larger plugin) no longer
         * stretches a fixed-size UI — suil's wrapper XResizeWindow()s the plugin
         * to whatever allocation it gets, and fixed pedals set no max-size hint to
         * clamp it, which is what produced the jumbled/oversized graphics. */
        gtk_widget_set_hexpand(gui, TRUE);
        gtk_widget_set_vexpand(gui, TRUE);
        gtk_widget_set_halign(gui, GTK_ALIGN_FILL);
        gtk_widget_set_valign(gui, GTK_ALIGN_FILL);
        gtk_container_add(GTK_CONTAINER(fw->gui_holder), gui);
    }
    gtk_widget_show_all(gui);
    gtk_stack_set_visible_child(GTK_STACK(fw->gui_holder), gui);
    gtk_widget_queue_resize(fw->gui_holder);   /* re-allocate the newly shown child */
    fw->shown = gui;
    fxwin_fit_later(fw);
}

typedef struct { FxWindow *fw; guint index; } RowLink;

/* Drag-and-drop reordering of the effect chain. Each row is both a drag source
 * and a drop target; the payload is the source row's index. App-local only. */
static const GtkTargetEntry FX_ROW_DND[] = {
    { (gchar *)"JACKDAW_FX_ROW", GTK_TARGET_SAME_APP, 0 }
};

/* Render a realized widget into a surface for use as the drag icon. */
static cairo_surface_t *fx_widget_snapshot(GtkWidget *w)
{
    if (!w || !gtk_widget_get_realized(w)) return NULL;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    if (a.width <= 0 || a.height <= 0) return NULL;

    GdkWindow *win = gtk_widget_get_window(w);
    cairo_surface_t *s = win
        ? gdk_window_create_similar_surface(win, CAIRO_CONTENT_COLOR_ALPHA,
                                            a.width, a.height)
        : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, a.width, a.height);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(w, cr);
    /* Ghost it: DEST_IN scales the snapshot alpha by the (opaque) source alpha
     * times the paint alpha, so only the 0.75 dims it. */
    cairo_set_source_rgba(cr, 0, 0, 0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
    cairo_paint_with_alpha(cr, 0.75);
    cairo_destroy(cr);
    return s;
}

/* Pointer over the top half of its row → drop lands before it, else after. */
static gboolean fxrow_drop_above(GtkWidget *w, gint y)
{
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    return y < a.height / 2;
}

static void fxrow_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)data;
    cairo_surface_t *s = fx_widget_snapshot(w);
    if (s) { gtk_drag_set_icon_surface(ctx, s); cairo_surface_destroy(s); }
}

static gboolean fxrow_drag_motion(GtkWidget *w, GdkDragContext *ctx,
                                  gint x, gint y, guint time, gpointer data)
{
    (void)x;
    RowLink  *rl = data;
    FxWindow *fw = rl->fw;
    gboolean above = fxrow_drop_above(w, y);
    gint tx, ty;
    gtk_widget_translate_coordinates(w, fw->list_box, 0, 0, &tx, &ty);
    if (!above) {
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        ty += a.height;
    }
    fw->drop_y      = ty;
    fw->drop_active = TRUE;
    gtk_widget_queue_draw(fw->list_box);
    gdk_drag_status(ctx, GDK_ACTION_MOVE, time);
    return TRUE;
}

static void fxrow_drag_leave(GtkWidget *w, GdkDragContext *ctx,
                             guint time, gpointer data)
{
    (void)w; (void)ctx; (void)time;
    RowLink  *rl = data;
    FxWindow *fw = rl->fw;
    if (fw->drop_active) {
        fw->drop_active = FALSE;
        gtk_widget_queue_draw(fw->list_box);
    }
}

static void fxrow_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                                GtkSelectionData *sel, guint info,
                                guint time, gpointer data)
{
    (void)w; (void)ctx; (void)info; (void)time;
    RowLink *rl = data;
    guint idx = rl->index;
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel),
                           8, (const guchar *)&idx, sizeof idx);
}

static void fxrow_drag_data_received(GtkWidget *w, GdkDragContext *ctx,
                                     gint x, gint y, GtkSelectionData *sel,
                                     guint info, guint time, gpointer data)
{
    (void)x; (void)info;
    RowLink *rl = data;
    FxWindow *fw = rl->fw;
    fw->drop_active = FALSE;
    gtk_widget_queue_draw(fw->list_box);

    gboolean ok = (gtk_selection_data_get_length(sel) == (gint)sizeof(guint));
    if (ok) {
        gint  from = (gint)*(const guint *)gtk_selection_data_get_data(sel);
        gint  tgt  = (gint)rl->index;
        guint n    = jackdaw_track_fx_count(fw->track);
        /* Honour the insertion line: top half lands before, bottom half after. */
        gboolean above = fxrow_drop_above(w, y);
        gint ins   = above ? tgt : tgt + 1;          /* slot in [0, n]   */
        gint final = (from < ins) ? ins - 1 : ins;   /* index after move */
        final = CLAMP(final, 0, (gint)n - 1);
        if (final != from) {
            jackdaw_engine_set_suspended(TRUE);
            jackdaw_track_fx_move(fw->track, (guint)from, (guint)final);
            jackdaw_engine_set_suspended(FALSE);
            fxwin_rebuild_list(fw);
            fxwin_show_gui(fw, (guint)final);
            GtkListBoxRow *r = gtk_list_box_get_row_at_index(
                GTK_LIST_BOX(fw->list_box), final);
            if (r) gtk_list_box_select_row(GTK_LIST_BOX(fw->list_box), r);
        }
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
}

/* Insertion line drawn across the effect list during a reorder drag. */
static gboolean fxlist_draw_after(GtkWidget *w, cairo_t *cr, gpointer data)
{
    FxWindow *fw = data;
    if (!fw->drop_active) return FALSE;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    double yy = fw->drop_y + 0.5;
    cairo_set_source_rgb(cr, 0.20, 0.55, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, 0,       yy);
    cairo_line_to(cr, a.width, yy);
    cairo_stroke(cr);
    cairo_arc(cr, 3,           yy, 3, 0, 2 * G_PI);
    cairo_arc(cr, a.width - 3, yy, 3, 0, 2 * G_PI);
    cairo_fill(cr);
    return FALSE;
}

static void fxrow_enable_toggled(GtkToggleButton *b, gpointer data)
{
    RowLink *rl = data;
    gpointer inst = jackdaw_track_fx_get(rl->fw->track, rl->index);
    if (inst) pluginhost_set_active((PluginInstance *)inst,
                                    gtk_toggle_button_get_active(b));
}

static void fxrow_remove_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    RowLink *rl = data;
    FxWindow *fw = rl->fw;
    /* Free this effect's editor (suil instance + timers) before the DSP instance
     * is (deferred-)freed, so nothing lingers pointing at a dead widget. */
    pluginhost_release_gui(jackdaw_track_fx_get(fw->track, rl->index));
    fw->shown = NULL;
    /* Suspend the RT graph around the chain swap + deferred plugin destroy. */
    jackdaw_engine_set_suspended(TRUE);
    jackdaw_track_fx_remove(fw->track, rl->index);
    jackdaw_engine_set_suspended(FALSE);
    fxwin_rebuild_list(fw);
    guint n = jackdaw_track_fx_count(fw->track);
    if (n) fxwin_show_gui(fw, n - 1);
    else if (fw->mix_row) gtk_widget_hide(fw->mix_row);
}

static void fxrow_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    FxWindow *fw = data;
    if (!row) return;
    fxwin_show_gui(fw, (guint)gtk_list_box_row_get_index(row));
}

static void fxwin_rebuild_list(FxWindow *fw)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(fw->list_box));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
    g_list_free(kids);

    guint n = jackdaw_track_fx_count(fw->track);
    for (guint i = 0; i < n; i++) {
        PluginInstance *inst = jackdaw_track_fx_get(fw->track, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

        GtkWidget *en = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(en),
                                     pluginhost_is_active(inst));
        gtk_widget_set_tooltip_text(en, "Enable / bypass this effect");
        GtkWidget *name = gtk_label_new(pluginhost_name(inst));
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        GtkWidget *rm = gtk_button_new_with_label("\342\234\225");  /* ✕ */

        RowLink *rl = g_new0(RowLink, 1);
        rl->fw = fw; rl->index = i;
        g_object_set_data_full(G_OBJECT(row), "row-link", rl, g_free);
        g_signal_connect(en, "toggled", G_CALLBACK(fxrow_enable_toggled), rl);
        g_signal_connect(rm, "clicked", G_CALLBACK(fxrow_remove_clicked), rl);

        gtk_box_pack_start(GTK_BOX(row), en,   FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), name, TRUE,  TRUE,  0);
        gtk_box_pack_start(GTK_BOX(row), rm,   FALSE, FALSE, 0);

        /* Wrap the row in an event box and make THAT the drag source/target.
         * GtkListBox claims press gestures on its own rows for selection, so a
         * drag source set on the GtkListBoxRow never sees the motion needed to
         * start; an event box has its own input window and does. The check
         * button and remove button keep their own clicks (separate windows);
         * dragging the name (which has no window) starts on the event box. */
        GtkWidget *ebox = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(ebox), row);
        gtk_drag_source_set(ebox, GDK_BUTTON1_MASK,
                            FX_ROW_DND, 1, GDK_ACTION_MOVE);
        /* No DEFAULT_HIGHLIGHT — we draw our own insertion line instead. */
        gtk_drag_dest_set(ebox, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
                          FX_ROW_DND, 1, GDK_ACTION_MOVE);
        g_signal_connect(ebox, "drag-begin",
                         G_CALLBACK(fxrow_drag_begin), rl);
        g_signal_connect(ebox, "drag-motion",
                         G_CALLBACK(fxrow_drag_motion), rl);
        g_signal_connect(ebox, "drag-leave",
                         G_CALLBACK(fxrow_drag_leave), rl);
        g_signal_connect(ebox, "drag-data-get",
                         G_CALLBACK(fxrow_drag_data_get), rl);
        g_signal_connect(ebox, "drag-data-received",
                         G_CALLBACK(fxrow_drag_data_received), rl);

        gtk_list_box_insert(GTK_LIST_BOX(fw->list_box), ebox, -1);
    }
    gtk_widget_show_all(fw->list_box);
}

static void fxwin_add_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    FxWindow *fw = data;
    PluginInfo *pi = fx_browse_for_plugin(GTK_WINDOW(fw->window));
    if (!pi) return;
    /* Hold the RT graph off the plugins while we dlopen / setupProcessing /
     * allocate buffers (not RT-safe) and swap in the new chain — otherwise the
     * live audio thread xruns. Same pattern as project load. */
    jackdaw_engine_set_suspended(TRUE);
    PluginInstance *inst = pluginhost_instantiate(pi);
    if (!inst) {
        jackdaw_engine_set_suspended(FALSE);
        GtkWidget *e = gtk_message_dialog_new(GTK_WINDOW(fw->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not load plugin: %s", pi->name);
        gtk_dialog_run(GTK_DIALOG(e));
        gtk_widget_destroy(e);
        return;
    }
    jackdaw_track_fx_add(fw->track, inst);
    jackdaw_engine_set_suspended(FALSE);
    fxwin_rebuild_list(fw);
    fxwin_show_gui(fw, jackdaw_track_fx_count(fw->track) - 1);
}

static gboolean fxwin_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e;
    FxWindow *fw = data;
    if (fw->fit_id) { g_source_remove(fw->fit_id); fw->fit_id = 0; }
    /* Free all editors (suil instances + timers) BEFORE destroying the window. */
    fxwin_release_all(fw);
    /* Store index+1 so 0 means "never saved" and index 0 is distinguishable. */
    g_object_set_data(G_OBJECT(fw->track), "fx-last-index",
                      GUINT_TO_POINTER(fw->sel_index + 1));
    g_object_set_data(G_OBJECT(fw->track), "fx-window", NULL);
    gtk_widget_destroy(fw->window);
    g_free(fw);
    return TRUE;
}

void jackdaw_fx_window_open(JackDawTrack *track, JackDawProject *project)
{
    g_return_if_fail(JACKDAW_IS_TRACK(track));

    FxWindow *existing = g_object_get_data(G_OBJECT(track), "fx-window");
    if (existing) { gtk_window_present(GTK_WINDOW(existing->window)); return; }

    FxWindow *fw = g_new0(FxWindow, 1);
    fw->track   = track;
    fw->project = project;

    fw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gchar *title = g_strdup_printf("FX: %s", jackdaw_track_get_name(track));
    gtk_window_set_title(GTK_WINDOW(fw->window), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(fw->window), 720, 420);
    g_signal_connect(fw->window, "delete-event", G_CALLBACK(fxwin_delete), fw);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(fw->window), paned);

    /* Left side panel */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(left), 4);
    gtk_widget_set_size_request(left, 220, -1);

    GtkWidget *add   = gtk_button_new_with_label("Add Effect\342\200\246");
    g_signal_connect(add, "clicked", G_CALLBACK(fxwin_add_clicked), fw);
    gtk_box_pack_start(GTK_BOX(left), add, FALSE, FALSE, 0);

    GtkWidget *lscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(lscroll, TRUE);
    fw->list_box = gtk_list_box_new();
    g_signal_connect(fw->list_box, "row-selected",
                     G_CALLBACK(fxrow_selected), fw);
    /* Drawn after children so the reorder insertion line sits on top. */
    g_signal_connect_after(fw->list_box, "draw",
                           G_CALLBACK(fxlist_draw_after), fw);
    gtk_container_add(GTK_CONTAINER(lscroll), fw->list_box);
    gtk_box_pack_start(GTK_BOX(left), lscroll, TRUE, TRUE, 0);

    /* Right panel: wet/dry header above the selected effect's GUI. No forced
     * width — the panel fits the shown plugin's natural size (see fxwin_fit_*). */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(right, 300, -1);

    fw->mix_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(fw->mix_row), 4);
    GtkWidget *mix_lbl = gtk_label_new("Dry / Wet");
    fw->mix_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                             0.0, 1.0, 0.01);
    gtk_scale_set_value_pos(GTK_SCALE(fw->mix_scale), GTK_POS_RIGHT);
    gtk_range_set_value(GTK_RANGE(fw->mix_scale), 1.0);
    gtk_widget_set_hexpand(fw->mix_scale, TRUE);
    g_signal_connect(fw->mix_scale, "value-changed",
                     G_CALLBACK(fxwin_mix_changed), fw);
    gtk_box_pack_start(GTK_BOX(fw->mix_row), mix_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fw->mix_row), fw->mix_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right), fw->mix_row, FALSE, FALSE, 0);

    /* A stack so each effect's editor is added once and shown by switching the
     * visible child — never reparented (which would blank a native X11 UI). */
    fw->gui_holder = gtk_stack_new();
    /* Size to the VISIBLE child, not the largest — otherwise a small plugin gets
     * stretched to the biggest one ever loaded and the window can't shrink. */
    gtk_stack_set_hhomogeneous(GTK_STACK(fw->gui_holder), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(fw->gui_holder), FALSE);
    gtk_widget_set_hexpand(fw->gui_holder, TRUE);
    gtk_widget_set_vexpand(fw->gui_holder, TRUE);
    gtk_box_pack_start(GTK_BOX(right), fw->gui_holder, TRUE, TRUE, 0);

    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, TRUE);

    g_object_set_data(G_OBJECT(track), "fx-window", fw);

    fxwin_rebuild_list(fw);

    /* Select the last-used effect (or the first on initial open). */
    guint n = jackdaw_track_fx_count(track);
    if (n > 0) {
        guint stored = GPOINTER_TO_UINT(
            g_object_get_data(G_OBJECT(track), "fx-last-index"));
        guint sel = (stored > 0) ? MIN(stored - 1, n - 1) : 0;
        GtkListBoxRow *row =
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(fw->list_box), (gint)sel);
        if (row)
            gtk_list_box_select_row(GTK_LIST_BOX(fw->list_box), row);
    }

    gtk_widget_show_all(fw->window);
}
