/*
 ** GUI code
 ** (c) 2009-2013 by Robert Manea et al.
*/

#include "gui.h"

#include "uzbl-core.h"
#include "callbacks.h"
#include "events.h"
#include "type.h"
#include "variables.h" /* FIXME: This is for get_geometry which   *
                        *        should probably be in this file. */

#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include <glib.h>

static void
uzbl_status_bar_init (void);
static void
uzbl_web_view_init (void);
static void
uzbl_vbox_init (void);
static void
uzbl_window_init (void);
static void
uzbl_plug_init (void);

void
uzbl_gui_init (gboolean plugmode)
{
    uzbl_status_bar_init ();
    uzbl_web_view_init ();
    uzbl_vbox_init ();

    if (plugmode) {
        uzbl_plug_init ();
    } else {
        uzbl_window_init ();
    }
}

static gboolean
key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer data);
static gboolean
key_release_cb (GtkWidget *widget, GdkEventKey *event, gpointer data);

void
uzbl_status_bar_init (void)
{
    uzbl.gui.status_bar = uzbl_status_bar_new ();

    g_object_connect (G_OBJECT (uzbl.gui.status_bar),
        "signal::key-press-event",   G_CALLBACK (key_press_cb), NULL,
        "signal::key-release-event", G_CALLBACK (key_press_cb), NULL,
        NULL);

    /*
    g_object_connect (G_OBJECT (UZBL_STATUS_BAR (uzbl.gui.status_bar)->label_left),
        "signal::key-press-event",   G_CALLBACK (key_press_cb),   NULL,
        "signal::key-release-event", G_CALLBACK (key_release_cb), NULL,
        NULL);

    g_object_connect (G_OBJECT (UZBL_STATUS_BAR (uzbl.gui.status_bar)->label_right),
        "signal::key-press-event",   G_CALLBACK (key_press_cb),   NULL,
        "signal::key-release-event", G_CALLBACK (key_release_cb), NULL,
        NULL);
      */
}

/* Mouse events */
static gboolean
button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data);
static gboolean
button_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer data);
static void
link_hover_cb (WebKitWebView *view, const gchar *title, const gchar *link, gpointer data);

void
uzbl_web_view_init (void)
{
    uzbl.gui.web_view = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    uzbl.gui.scrolled_win = gtk_scrolled_window_new (NULL, NULL);

    gtk_container_add (
        GTK_CONTAINER (uzbl.gui.scrolled_win),
        GTK_WIDGET (uzbl.gui.web_view)
    );

    g_object_connect (G_OBJECT (uzbl.gui.web_view),
        /* Keyboard events */
        "signal::key-press-event",                      G_CALLBACK (key_press_cb),             NULL,
        "signal::key-release-event",                    G_CALLBACK (key_release_cb),           NULL,
        /* Mouse events */
        "signal::button-press-event",                   G_CALLBACK (button_press_cb),          NULL,
        "signal::button-release-event",                 G_CALLBACK (button_release_cb),        NULL,
        "signal::hovering-over-link",                   G_CALLBACK (link_hover_cb),            NULL,
        /* Page metadata events */
        "signal::notify::title",                        G_CALLBACK (title_change_cb),          NULL,
        "signal::notify::progress",                     G_CALLBACK (progress_change_cb),       NULL,
        "signal::notify::load-status",                  G_CALLBACK (load_status_change_cb),    NULL,
        "signal::notify::uri",                          G_CALLBACK (uri_change_cb),            NULL,
        "signal::load-error",                           G_CALLBACK (load_error_cb),            NULL,
        "signal::window-object-cleared",                G_CALLBACK (window_object_cleared_cb), NULL,
        /* Navigation events */
        "signal::navigation-policy-decision-requested", G_CALLBACK (navigation_decision_cb),   NULL,
        "signal::mime-type-policy-decision-requested",  G_CALLBACK (mime_policy_cb),           NULL,
        "signal::download-requested",                   G_CALLBACK (download_cb),              NULL,
        "signal::resource-request-starting",            G_CALLBACK (request_starting_cb),      NULL,
        /* UI events */
        "signal::create-web-view",                      G_CALLBACK (create_web_view_cb),       NULL,
        "signal::close-web-view",                       G_CALLBACK (close_web_view_cb),        NULL,
        "signal::focus-in-event",                       G_CALLBACK (focus_cb),                 NULL,
        "signal::focus-out-event",                      G_CALLBACK (focus_cb),                 NULL,
#if WEBKIT_CHECK_VERSION (1, 9, 0)
        "signal::context-menu",                         G_CALLBACK (context_menu_cb),          NULL,
#else
        "signal::populate-popup",                       G_CALLBACK (populate_popup_cb),        NULL,
#endif
        NULL);

    uzbl.gui.bar_h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (uzbl.gui.scrolled_win));
    uzbl.gui.bar_v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (uzbl.gui.scrolled_win));

    g_object_connect (G_OBJECT (uzbl.gui.bar_v),
        "signal::value-changed", G_CALLBACK (scroll_vert_cb), NULL,
        "signal::changed",       G_CALLBACK (scroll_vert_cb), NULL,
        NULL);
    g_object_connect (G_OBJECT (uzbl.gui.bar_h),
        "signal::value-changed", G_CALLBACK (scroll_horiz_cb), NULL,
        "signal::changed",       G_CALLBACK (scroll_horiz_cb), NULL,
        NULL);

}

void
uzbl_vbox_init (void)
{
#if GTK_CHECK_VERSION (3, 0, 0)
    uzbl.gui.vbox = gtk_box_new (FALSE, 0);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (uzbl.gui.vbox), GTK_ORIENTATION_VERTICAL);
#else
    uzbl.gui.vbox = gtk_vbox_new (FALSE, 0);
#endif

    gtk_box_pack_start (GTK_BOX (uzbl.gui.vbox), uzbl.gui.scrolled_win, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (uzbl.gui.vbox), uzbl.gui.status_bar, FALSE, TRUE, 0);
}

static void
destroy_cb (GtkWidget *widget, gpointer data);
static gboolean
configure_event_cb (GtkWidget *widget, GdkEventConfigure *event, gpointer data);

void
uzbl_window_init (void)
{
    uzbl.gui.main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

    gtk_window_set_default_size (GTK_WINDOW (uzbl.gui.main_window), 800, 600);
    gtk_window_set_title (GTK_WINDOW (uzbl.gui.main_window), "Uzbl");
    gtk_widget_set_name (GTK_WIDGET (uzbl.gui.main_window), "Uzbl");

#if GTK_CHECK_VERSION (3, 0, 0)
    gtk_window_set_has_resize_grip (GTK_WINDOW (uzbl.gui.main_window), FALSE);
#endif

    /* Fill in the main window */
    gtk_container_add (GTK_CONTAINER (uzbl.gui.main_window), uzbl.gui.vbox);

    g_object_connect (G_OBJECT (uzbl.gui.main_window),
        "signal::destroy",         G_CALLBACK (destroy_cb),         NULL,
        "signal::configure-event", G_CALLBACK (configure_event_cb), NULL,
        NULL);
}

void
uzbl_plug_init (void)
{
    uzbl.gui.plug = GTK_PLUG (gtk_plug_new (uzbl.state.socket_id));

    gtk_widget_set_name (GTK_WIDGET (uzbl.gui.plug), "Uzbl");

    gtk_container_add (GTK_CONTAINER (uzbl.gui.plug), uzbl.gui.vbox);

    g_object_connect (G_OBJECT (uzbl.gui.plug),
        "signal::destroy",           G_CALLBACK (destroy_cb),   NULL,
        "signal::key-press-event",   G_CALLBACK (key_press_cb), NULL,
        "signal::key-release-event", G_CALLBACK (key_press_cb), NULL,
        NULL);
}

/* Status bar callbacks */

gboolean
key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (data);

    if (event->type == GDK_KEY_PRESS) {
        key_to_event (event->keyval, event->state, event->is_modifier, GDK_KEY_PRESS);
    }

    return (uzbl.behave.forward_keys ? FALSE : TRUE);
}

gboolean
key_release_cb (GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (data);

    if (event->type == GDK_KEY_RELEASE) {
        key_to_event (event->keyval, event->state, event->is_modifier, GDK_KEY_RELEASE);
    }

    return (uzbl.behave.forward_keys ? FALSE : TRUE);
}

/* Web view callbacks */

/* Mouse events */

static gint
get_click_context (void);

gboolean
button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (data);

    gint context;
    gboolean propagate = FALSE;
    gboolean sendev    = FALSE;

    /* Save last button click for use in menu */
    if (uzbl.state.last_button) {
        gdk_event_free ((GdkEvent *)uzbl.state.last_button);
    }
    uzbl.state.last_button = (GdkEventButton *)gdk_event_copy ((GdkEvent *)event);

    /* Grab context from last click */
    context = get_click_context ();

    if(event->type == GDK_BUTTON_PRESS) {
        /* left click */
        if (event->button == 1) {
            if ((context & WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE)) {
                send_event (FORM_ACTIVE, NULL,
                    TYPE_NAME, "button1",
                    NULL);
            } else if ((context & WEBKIT_HIT_TEST_RESULT_CONTEXT_DOCUMENT)) {
                send_event (ROOT_ACTIVE, NULL,
                    TYPE_NAME, "button1",
                    NULL);
            } else {
                sendev    = TRUE;
                propagate = TRUE;
            }
        } else if ((event->button == 2) && !(context & WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE)) {
            sendev    = TRUE;
            propagate = TRUE;
        } else if (event->button > 3) {
            sendev    = TRUE;
            propagate = TRUE;
        }
    }

    if ((event->type == GDK_2BUTTON_PRESS) || (event->type == GDK_3BUTTON_PRESS)) {
        if ((event->button == 1) && !(context & WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE) && (context & WEBKIT_HIT_TEST_RESULT_CONTEXT_DOCUMENT)) {
            sendev    = TRUE;
            propagate = uzbl.state.handle_multi_button;
        } else if ((event->button == 2) && !(context & WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE)) {
            sendev    = TRUE;
            propagate = uzbl.state.handle_multi_button;
        } else if (event->button >= 3) {
            sendev    = TRUE;
            propagate = uzbl.state.handle_multi_button;
        }
    }

    if (sendev) {
        button_to_event (event->button, event->state, event->type);
    }

    return propagate;
}

gboolean
button_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (data);

    gint context;
    gboolean propagate = FALSE;
    gboolean sendev    = FALSE;

    context = get_click_context ();

    if (event->type == GDK_BUTTON_RELEASE) {
        if ((event->button == 2) && !(context & WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE)) {
            sendev    = TRUE;
            propagate = TRUE;
        } else if (event->button > 3) {
            sendev    = TRUE;
            propagate = TRUE;
        }

        if (sendev) {
            button_to_event (event->button, event->state, GDK_BUTTON_RELEASE);
        }
    }

    return propagate;
}

gint
get_click_context ()
{
    WebKitHitTestResult *ht;
    guint context;

    if (!uzbl.state.last_button) {
        return -1;
    }

    ht = webkit_web_view_get_hit_test_result (uzbl.gui.web_view, uzbl.state.last_button);
    g_object_get (ht, "context", &context, NULL);
    g_object_unref (ht);

    return (gint)context;
}

void
link_hover_cb (WebKitWebView *view, const gchar *title, const gchar *link, gpointer data)
{
    UZBL_UNUSED (view);
    UZBL_UNUSED (title);
    UZBL_UNUSED (data);

    if (uzbl.state.last_selected_url) {
        g_free (uzbl.state.last_selected_url);
    }

    if (uzbl.state.selected_url) {
        uzbl.state.last_selected_url = g_strdup (uzbl.state.selected_url);
        g_free (uzbl.state.selected_url);
        uzbl.state.selected_url = NULL;
    } else {
        uzbl.state.last_selected_url = NULL;
    }

    if (uzbl.state.last_selected_url && g_strcmp0 (link, uzbl.state.last_selected_url)) {
        send_event (LINK_UNHOVER, NULL,
            TYPE_STR, uzbl.state.last_selected_url,
            NULL);
    }

    if (link) {
        uzbl.state.selected_url = g_strdup (link);
        send_event (LINK_HOVER, NULL,
            TYPE_STR, uzbl.state.selected_url,
            NULL);
    }

    update_title ();
}

/* Window callbacks */

void
destroy_cb (GtkWidget *widget, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (data);

    gtk_main_quit ();
}

gboolean
configure_event_cb (GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    UZBL_UNUSED (widget);
    UZBL_UNUSED (event);
    UZBL_UNUSED (data);

    gchar *last_geo    = uzbl.gui.geometry;
    gchar *current_geo = get_geometry ();

    if (!last_geo || g_strcmp0 (last_geo, current_geo)) {
        send_event (GEOMETRY_CHANGED, NULL,
            TYPE_STR, current_geo,
            NULL);
    }

    g_free (current_geo);

    return FALSE;
}
