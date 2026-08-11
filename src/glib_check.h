#ifndef GLIB_CHECK_H_INCLUDED
#define GLIB_CHECK_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Startup sanity check for the glib2 runtime the engine depends on
 * (GObject instantiation + atomic ops). Returns 0 on success, nonzero if the
 * runtime is unusable; prints a diagnostic either way. Called once from
 * main() before any engine or UI object is created. */
int jackdaw_glib_check(void);

#ifdef __cplusplus
}
#endif

#endif /* GLIB_CHECK_H_INCLUDED */
