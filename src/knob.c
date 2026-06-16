#define _GNU_SOURCE
#include <config.h>
#include <math.h>
#include <stdio.h>

#include "knob.h"

#define KNOB_DIAM       28          /* px: circle diameter */
#define KNOB_TEXT_H     12          /* px: readout strip below the circle */
#define KNOB_START_ANG  (-M_PI * 5.0 / 4.0)   /* min value: ~7:30 */
#define KNOB_END_ANG    (M_PI / 4.0)          /* max value: ~4:30 */

typedef struct {
    double     value;
    double     min;
    double     max;
    double     default_val;
    KnobKind   kind;
    double     drag_start_y;
    double     drag_start_val;
    gboolean   dragging;
    GtkWidget *popup;       /* floating value read-out, shown while adjusting */
    GtkWidget *popup_lbl;
    guint      hide_id;     /* auto-hide timeout source */
    char       center[4];   /* optional 1-char id drawn in the dial ("V"/"P") */
    void     (*on_change)(double val, gpointer user_data);
    gpointer   user_data;
} KnobData;

static double knob_angle(KnobData *kd)
{
    double t = (kd->max > kd->min)
               ? (kd->value - kd->min) / (kd->max - kd->min) : 0.0;
    t = CLAMP(t, 0.0, 1.0);
    return KNOB_START_ANG + t * (KNOB_END_ANG - KNOB_START_ANG);
}

/* Value text for the floating read-out: dB with unit, pan as L/R percent. */
static void knob_format_popup(KnobData *kd, char *buf, size_t n);

/* Public wrapper: format the current value using the knob's kind. */
void knob_format_value(GtkWidget *knob, char *buf, gsize n)
{
    KnobData *kd = g_object_get_data(G_OBJECT(knob), "knob-data");
    if (!kd || n == 0) { if (n) buf[0] = '\0'; return; }
    knob_format_popup(kd, buf, n);
}

static void knob_format_popup(KnobData *kd, char *buf, size_t n)
{
    switch (kd->kind) {
    case KNOB_DB:
        if (kd->value <= kd->min + 0.01)
            g_snprintf(buf, n, "-inf");
        else
            g_snprintf(buf, n, "%+.1f dB", kd->value);
        break;
    case KNOB_PAN: {
        int p = (int)lround(fabs(kd->value) * 100.0);
        if (p == 0)             g_snprintf(buf, n, "C");
        else if (kd->value < 0) g_snprintf(buf, n, "L%d%%", p);
        else                    g_snprintf(buf, n, "R%d%%", p);
        break;
    }
    default:
        g_snprintf(buf, n, "%.2f", kd->value);
        break;
    }
}

/* Compact value text for an always-visible strip label (no unit suffix, no
 * percent sign) so the read-out column stays narrow: "+10.2" / "-inf" /
 * "L100" / "C" / "R37". */
void knob_format_compact(GtkWidget *knob, char *buf, gsize n)
{
    KnobData *kd = g_object_get_data(G_OBJECT(knob), "knob-data");
    if (!kd || n == 0) { if (n) buf[0] = '\0'; return; }
    switch (kd->kind) {
    case KNOB_DB:
        if (kd->value <= kd->min + 0.01) g_snprintf(buf, n, "-inf");
        else                             g_snprintf(buf, n, "%+.1f", kd->value);
        break;
    case KNOB_PAN: {
        int p = (int)lround(fabs(kd->value) * 100.0);
        if (p == 0)             g_snprintf(buf, n, "C");
        else if (kd->value < 0) g_snprintf(buf, n, "L%d", p);
        else                    g_snprintf(buf, n, "R%d", p);
        break;
    }
    default:
        g_snprintf(buf, n, "%.2f", kd->value);
        break;
    }
}

/* Set a 1-character identifier drawn in the centre of the dial face. */
void knob_set_center_label(GtkWidget *knob, const char *label)
{
    KnobData *kd = g_object_get_data(G_OBJECT(knob), "knob-data");
    if (!kd) return;
    if (label) g_strlcpy(kd->center, label, sizeof kd->center);
    else       kd->center[0] = '\0';
    gtk_widget_queue_draw(knob);
}

static gboolean knob_popup_hide_cb(gpointer data)
{
    KnobData *kd = data;
    kd->hide_id = 0;
    if (kd->popup) gtk_widget_hide(kd->popup);
    return G_SOURCE_REMOVE;
}

/* Show (or refresh) the floating value read-out, centered above the knob, and
 * arm an auto-hide as a safety net (covers scroll, which has no release). */
static void knob_popup_show(GtkWidget *widget, KnobData *kd)
{
    if (!kd->popup) {
        kd->popup = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(kd->popup),
                                 GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_window_set_resizable(GTK_WINDOW(kd->popup), FALSE);
        kd->popup_lbl = gtk_label_new("");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(kd->popup_lbl), "mix-db-pop");
        gtk_container_add(GTK_CONTAINER(kd->popup), kd->popup_lbl);
        gtk_widget_show(kd->popup_lbl);
    }

    char buf[32];
    knob_format_popup(kd, buf, sizeof buf);
    gtk_label_set_text(GTK_LABEL(kd->popup_lbl), buf);

    GtkWidget *top = gtk_widget_get_toplevel(widget);
    GdkWindow *tw  = top ? gtk_widget_get_window(top) : NULL;
    if (tw) {
        gint ox, oy, wx = 0, wy = 0;
        gdk_window_get_origin(tw, &ox, &oy);
        gtk_widget_translate_coordinates(widget, top, 0, 0, &wx, &wy);
        GtkAllocation a; gtk_widget_get_allocation(widget, &a);
        GtkRequisition req;
        gtk_widget_get_preferred_size(kd->popup, NULL, &req);
        gtk_window_move(GTK_WINDOW(kd->popup),
                        ox + wx + a.width / 2 - req.width / 2,
                        oy + wy - req.height - 2);
    }
    gtk_widget_show(kd->popup);

    if (kd->hide_id) g_source_remove(kd->hide_id);
    kd->hide_id = g_timeout_add(900, knob_popup_hide_cb, kd);
}

static void knob_popup_hide(KnobData *kd)
{
    if (kd->hide_id) { g_source_remove(kd->hide_id); kd->hide_id = 0; }
    if (kd->popup) gtk_widget_hide(kd->popup);
}

static gboolean knob_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    KnobData *kd = data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    double cx  = alloc.width / 2.0;
    double cy  = KNOB_DIAM / 2.0 + 1.0;
    double r   = KNOB_DIAM / 2.0 - 2.5;
    double ang = knob_angle(kd);

    /* Background arc track */
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, cx, cy, r - 1.5, KNOB_START_ANG, KNOB_END_ANG);
    cairo_stroke(cr);

    /* Value arc */
    cairo_set_source_rgb(cr, 0.2, 0.55, 0.9);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, cx, cy, r - 1.5, KNOB_START_ANG, ang);
    cairo_stroke(cr);

    /* Pointer dot */
    double px = cx + (r - 3.5) * cos(ang);
    double py = cy + (r - 3.5) * sin(ang);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_arc(cr, px, py, 2.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    /* Optional identifier letter ("V"/"P") centered in the dial face, so the
     * host doesn't need a separate label widget eating horizontal space. */
    if (kd->center[0]) {
        cairo_text_extents_t te;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.0);
        cairo_text_extents(cr, kd->center, &te);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_move_to(cr, cx - te.width / 2.0 - te.x_bearing,
                          cy - te.height / 2.0 - te.y_bearing);
        cairo_show_text(cr, kd->center);
    }

    /* The numeric value is shown in a floating popup while the knob is being
     * adjusted (see knob_popup_show) rather than drawn inline — an inline
     * readout was unreadable against the dark theme. */
    return FALSE;
}

static gboolean knob_press(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    KnobData *kd = data;
    if (ev->button != 1) return FALSE;

    /* Double-click resets to the default value (e.g. 0 dB / centre). */
    if (ev->type == GDK_2BUTTON_PRESS) {
        kd->value    = CLAMP(kd->default_val, kd->min, kd->max);
        kd->dragging = FALSE;
        gtk_widget_queue_draw(widget);
        if (kd->on_change) kd->on_change(kd->value, kd->user_data);
        knob_popup_show(widget, kd);
        return TRUE;
    }

    kd->dragging       = TRUE;
    kd->drag_start_y   = ev->y_root;
    kd->drag_start_val = kd->value;
    gtk_widget_grab_focus(widget);
    knob_popup_show(widget, kd);
    return TRUE;
}

static gboolean knob_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    KnobData *kd = data;
    if (!kd->dragging) return FALSE;
    double range = kd->max - kd->min;
    double delta = (kd->drag_start_y - ev->y_root) * range / 150.0;
    kd->value = CLAMP(kd->drag_start_val + delta, kd->min, kd->max);
    gtk_widget_queue_draw(widget);
    if (kd->on_change) kd->on_change(kd->value, kd->user_data);
    knob_popup_show(widget, kd);
    return TRUE;
}

static gboolean knob_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    (void)widget; (void)ev;
    KnobData *kd = data;
    kd->dragging = FALSE;
    knob_popup_hide(kd);
    return TRUE;
}

static gboolean knob_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer data)
{
    KnobData *kd = data;
    double step = (kd->max - kd->min) / 200.0;
    if (ev->direction == GDK_SCROLL_UP   || ev->direction == GDK_SCROLL_RIGHT)
        kd->value = CLAMP(kd->value + step, kd->min, kd->max);
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_LEFT)
        kd->value = CLAMP(kd->value - step, kd->min, kd->max);
    else
        return FALSE;
    gtk_widget_queue_draw(widget);
    if (kd->on_change) kd->on_change(kd->value, kd->user_data);
    knob_popup_show(widget, kd);
    return TRUE;
}

static void knob_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    KnobData *kd = data;
    if (kd->hide_id) { g_source_remove(kd->hide_id); kd->hide_id = 0; }
    if (kd->popup) { gtk_widget_destroy(kd->popup); kd->popup = NULL; }
}

GtkWidget *knob_new(double min, double max, double value, double default_val,
                    KnobKind kind,
                    void (*on_change)(double, gpointer), gpointer user_data)
{
    KnobData *kd     = g_new0(KnobData, 1);
    kd->min          = min;
    kd->max          = max;
    kd->value        = CLAMP(value, min, max);
    kd->default_val  = default_val;
    kd->kind         = kind;
    kd->on_change    = on_change;
    kd->user_data    = user_data;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, KNOB_DIAM, KNOB_DIAM + KNOB_TEXT_H);
    gtk_widget_add_events(da,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_widget_set_can_focus(da, TRUE);

    g_object_set_data_full(G_OBJECT(da), "knob-data", kd, g_free);
    g_signal_connect(da, "draw",                 G_CALLBACK(knob_draw_cb), kd);
    g_signal_connect(da, "button-press-event",   G_CALLBACK(knob_press),   kd);
    g_signal_connect(da, "motion-notify-event",  G_CALLBACK(knob_motion),  kd);
    g_signal_connect(da, "button-release-event", G_CALLBACK(knob_release), kd);
    g_signal_connect(da, "scroll-event",         G_CALLBACK(knob_scroll),  kd);
    g_signal_connect(da, "destroy",              G_CALLBACK(knob_destroy), kd);

    return da;
}

void knob_set_value(GtkWidget *knob, double value)
{
    KnobData *kd = g_object_get_data(G_OBJECT(knob), "knob-data");
    if (!kd) return;
    kd->value = CLAMP(value, kd->min, kd->max);
    gtk_widget_queue_draw(knob);
}

double knob_get_value(GtkWidget *knob)
{
    KnobData *kd = g_object_get_data(G_OBJECT(knob), "knob-data");
    return kd ? kd->value : 0.0;
}
