/* lv2ui_bridge.h — host side of out-of-process LV2 UI hosting, used ONLY for
 * toolkits that can't run in JackDAW's GTK3 process (GtkUI/GTK2, Qt). Spawns a
 * per-UI helper, embeds its window in a GtkSocket, and forwards control ports.
 * X11/Gtk3 UIs are hosted in-process (pluginhost_lv2.c) and never come here.
 */
#ifndef LV2UI_BRIDGE_H_INCLUDED
#define LV2UI_BRIDGE_H_INCLUDED

#include <gtk/gtk.h>
#include "pluginhost.h"

G_BEGIN_DECLS

/* Returns a GtkSocket widget hosting `inst`'s editor in a helper process, or
 * NULL if the plugin has no helper-hostable UI / the helper isn't installed. */
GtkWidget *lv2ui_bridge_new(PluginInstance *inst);

G_END_DECLS

#endif /* LV2UI_BRIDGE_H_INCLUDED */
