#ifndef SETTINGS_H_INCLUDED
#define SETTINGS_H_INCLUDED

#include <glib.h>

void    settings_init    (void);
void    settings_save    (void);
void    settings_quit    (void);

guint32 settings_get_uint32(const gchar *key, guint32 def);
void    settings_set_uint32(const gchar *key, guint32 val);

/* Caller frees the returned string. */
gchar  *settings_get_string(const gchar *key, const gchar *def);
void    settings_set_string(const gchar *key, const gchar *val);

#endif /* SETTINGS_H_INCLUDED */
