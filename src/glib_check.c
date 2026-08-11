#include <glib-object.h>
#include <stdio.h>

#include "glib_check.h"

int jackdaw_glib_check(void)
{
    /* Header/runtime version skew: glib_check_version returns NULL when the
     * runtime is compatible with the headers we compiled against. */
    const char *mismatch = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, 0);
    if (mismatch != NULL) {
        fprintf(stderr, "jackdaw: glib runtime incompatible: %s\n", mismatch);
        return 1;
    }

    /* GObject type system: create and destroy a plain GObject. */
    GObject *obj = g_object_new(G_TYPE_OBJECT, NULL);
    if (obj == NULL || !G_IS_OBJECT(obj)) {
        fprintf(stderr, "jackdaw: GObject instantiation failed\n");
        return 1;
    }
    g_object_unref(obj);

    /* Atomics (the engine's main<->RT flag mechanism). */
    gint v = 0;
    g_atomic_int_set(&v, 41);
    g_atomic_int_inc(&v);
    if (g_atomic_int_get(&v) != 42) {
        fprintf(stderr, "jackdaw: g_atomic_int is broken\n");
        return 1;
    }

    printf("jackdaw: glib %u.%u.%u OK (GObject + atomics)\n", glib_major_version,
           glib_minor_version, glib_micro_version);
    return 0;
}
