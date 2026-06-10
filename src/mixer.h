#ifndef MIXER_H_INCLUDED
#define MIXER_H_INCLUDED

#include <gtk/gtk.h>
#include "project.h"

G_BEGIN_DECLS

/* ========================================================================
 * JackDawMixer — bottom-docked mixer panel.
 *   [Master | Track 1 | Track 2 | …]   (master pinned far left)
 * Each strip: name, L/R VU meter, pan knob, vertical fader, mute/solo.
 * ======================================================================== */

#define JACKDAW_TYPE_MIXER (jackdaw_mixer_get_type())
#define JACKDAW_MIXER(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_MIXER, JackDawMixer))
#define JACKDAW_IS_MIXER(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_MIXER))

typedef struct _JackDawMixer      JackDawMixer;
typedef struct _JackDawMixerClass JackDawMixerClass;

struct _JackDawMixer {
    GtkBox          parent_instance;
    JackDawProject *project;     /* strong ref */
    GtkWidget      *strips_box;  /* horizontal box of channel strips */
    GHashTable     *strips;      /* JackDawTrack* → strip GtkWidget* */
    gpointer        master;      /* MixerStrip* for the master channel */
    guint           vu_timer;
};

struct _JackDawMixerClass {
    GtkBoxClass parent_class;
};

GType      jackdaw_mixer_get_type(void);
GtkWidget *jackdaw_mixer_new(JackDawProject *project);

G_END_DECLS

#endif /* MIXER_H_INCLUDED */
