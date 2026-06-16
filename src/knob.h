#ifndef KNOB_H_INCLUDED
#define KNOB_H_INCLUDED

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Cairo rotary knob with a numeric value readout and double-click reset.
 * Used by the track strips and the mixer.
 *
 *   KNOB_PLAIN — show the raw value
 *   KNOB_DB    — value is in decibels; shown as "+3.0 dB" / "-inf"
 *   KNOB_PAN   — value in [-1,1]; shown as "L37" / "C" / "R37"
 */
typedef enum {
    KNOB_PLAIN,
    KNOB_DB,
    KNOB_PAN
} KnobKind;

GtkWidget *knob_new(double min, double max, double value, double default_val,
                    KnobKind kind,
                    void (*on_change)(double val, gpointer user_data),
                    gpointer user_data);

/* Set the displayed value WITHOUT invoking the on_change callback. */
void   knob_set_value(GtkWidget *knob, double value);
double knob_get_value(GtkWidget *knob);

/* Format the knob's current value as it appears in the read-out
 * (e.g. "+3.0 dB" / "-inf" / "L37" / "C"), into the caller's buffer.
 * Lets a host place a permanent value label next to/under the knob. */
void   knob_format_value(GtkWidget *knob, char *buf, gsize n);

/* Compact value text for an always-visible label (no unit/percent suffix),
 * keeping the read-out column narrow: "+10.2" / "-inf" / "L100" / "C". */
void   knob_format_compact(GtkWidget *knob, char *buf, gsize n);

/* Draw a 1-character identifier ("V"/"P") in the centre of the dial face. */
void   knob_set_center_label(GtkWidget *knob, const char *label);

G_END_DECLS

#endif /* KNOB_H_INCLUDED */
