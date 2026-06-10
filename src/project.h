#ifndef PROJECT_H_INCLUDED
#define PROJECT_H_INCLUDED

#include <glib-object.h>
#include "track.h"

G_BEGIN_DECLS

#define JACKDAW_TYPE_PROJECT (jackdaw_project_get_type())
#define JACKDAW_PROJECT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST(obj, JACKDAW_TYPE_PROJECT, JackDawProject))
#define JACKDAW_IS_PROJECT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE(obj, JACKDAW_TYPE_PROJECT))

typedef struct _JackDawProject      JackDawProject;
typedef struct _JackDawProjectClass JackDawProjectClass;

struct _JackDawProject {
    GObject parent_instance;

    GPtrArray   *tracks;          /* array of JackDawTrack* (strong refs) */
    gchar       *project_file;    /* NULL if unsaved */

    gfloat       master_volume;
    volatile gpointer master_rt_chain;  /* swapped atomically (Phase 5) */

    /* JACK port counts (0 = auto-detect from physical JACK ports at startup).
     * Non-zero values are user overrides saved in the inifile. */
    guint        audio_in_count;
    guint        audio_out_count;
    guint        midi_in_count;
    guint        midi_out_count;
};

struct _JackDawProjectClass {
    GObjectClass parent_class;

    void (*track_added)  (JackDawProject *project, JackDawTrack *track);
    void (*track_removed)(JackDawProject *project, JackDawTrack *track);
    void (*ports_changed)(JackDawProject *project);
};

GType          jackdaw_project_get_type(void);
JackDawProject *jackdaw_project_new(void);

/* Track management */
void          jackdaw_project_add_track   (JackDawProject *p, JackDawTrack *t);
void          jackdaw_project_remove_track(JackDawProject *p, JackDawTrack *t);
guint         jackdaw_project_track_count (JackDawProject *p);
JackDawTrack *jackdaw_project_get_track   (JackDawProject *p, guint idx);

/* Master volume */
void   jackdaw_project_set_master_volume(JackDawProject *p, gfloat vol);
gfloat jackdaw_project_get_master_volume(JackDawProject *p);

/* Project file */
void         jackdaw_project_set_file(JackDawProject *p, const gchar *path);
const gchar *jackdaw_project_get_file(JackDawProject *p);

/* Signal to refresh port selectors after port count change */
void jackdaw_project_emit_ports_changed(JackDawProject *p);

G_END_DECLS

#endif /* PROJECT_H_INCLUDED */
