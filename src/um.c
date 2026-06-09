#include <config.h>
#include <gtk/gtk.h>
#include "um.h"

void user_error(const gchar *msg)
{
    g_warning("%s", msg);
    GtkWidget *dlg = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}
