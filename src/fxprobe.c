#define _GNU_SOURCE
#include <config.h>
#include <stdio.h>
#include <math.h>
#include <gtk/gtk.h>
#include "pluginhost.h"

static void spin(int n){ for(int i=0;i<n;i++) g_main_context_iteration(NULL,FALSE); }

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    const char *want = argc > 1 ? argv[1] : "Fuzz";
    pluginhost_init(48000.0, 256);
    pluginhost_load_paths_from_settings();
    const GList *cat = pluginhost_catalog();

    PluginInfo *a = NULL;
    for (const GList *l = cat; l; l = l->next) {
        PluginInfo *info = l->data;
        if (info->format == PH_LV2 && strstr(info->name, want)) { a = info; break; }
    }
    if (!a) { g_printerr("not found: %s\n", want); return 1; }
    g_printerr("plugin: %s\n", a->name);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 400);
    GtkWidget *stack = gtk_stack_new();
    gtk_container_add(GTK_CONTAINER(win), stack);
    gtk_widget_show_all(win);
    spin(40);

    PluginInstance *pi = pluginhost_instantiate(a);
    GtkWidget *gui = pluginhost_make_gui(pi);
    g_printerr("gui type=%s\n", G_OBJECT_TYPE_NAME(gui));
    gtk_container_add(GTK_CONTAINER(stack), gui);
    gtk_widget_show_all(gui);
    gtk_stack_set_visible_child(GTK_STACK(stack), gui);
    spin(200);   /* let the UI paint + idle run */

    /* screenshot the window */
    GdkWindow *gw = gtk_widget_get_window(win);
    int w = gdk_window_get_width(gw), h = gdk_window_get_height(gw);
    GdkPixbuf *pb = gdk_pixbuf_get_from_window(gw, 0, 0, w, h);
    if (pb) {
        gdk_pixbuf_save(pb, "/tmp/fxshot.png", "png", NULL, NULL);
        /* measure colour variation to detect a blank UI */
        int rs = gdk_pixbuf_get_rowstride(pb), nch = gdk_pixbuf_get_n_channels(pb);
        guchar *px = gdk_pixbuf_get_pixels(pb);
        long mn=255, mx=0; long distinct=0; guchar first=px[0];
        for (int y=0;y<h;y+=4) for (int x=0;x<w;x+=4){
            guchar v = px[y*rs + x*nch];
            if (v<mn)mn=v; if(v>mx)mx=v; if(v!=first)distinct++;
        }
        g_printerr("shot %dx%d saved; luma min=%ld max=%ld spread=%ld distinct=%ld\n",
                   w,h,mn,mx,mx-mn,distinct);
    } else g_printerr("no pixbuf\n");

    return 0;
}
