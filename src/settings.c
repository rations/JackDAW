#include <config.h>
#include <glib.h>
#include "settings.h"

static GKeyFile *kf   = NULL;
static gchar    *path = NULL;

void settings_init(void)
{
    const gchar *home = g_get_home_dir();
    gchar *dir = g_build_filename(home, ".jackdaw", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    path = g_build_filename(home, ".jackdaw", "config", NULL);
    kf   = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
}

void settings_save(void)
{
    if (!kf || !path) return;
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, NULL);
    if (data) {
        g_file_set_contents(path, data, (gssize)len, NULL);
        g_free(data);
    }
}

void settings_quit(void)
{
    settings_save();
    if (kf)   { g_key_file_free(kf); kf = NULL; }
    if (path) { g_free(path); path = NULL; }
}

guint32 settings_get_uint32(const gchar *key, guint32 def)
{
    if (!kf) return def;
    GError *err = NULL;
    gint64 v = g_key_file_get_int64(kf, "jackdaw", key, &err);
    if (err) { g_error_free(err); return def; }
    return (guint32)v;
}

void settings_set_uint32(const gchar *key, guint32 val)
{
    if (!kf) return;
    g_key_file_set_int64(kf, "jackdaw", key, (gint64)val);
}
