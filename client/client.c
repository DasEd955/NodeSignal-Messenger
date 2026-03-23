#include "client.h"

#include "comm.h"

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum NsUiEventType {
    NS_UI_APPEND_LINE = 1,
    NS_UI_CONNECTED = 2,
    NS_UI_DISCONNECTED = 3,
    NS_UI_STATUS = 4
} NsUiEventType;

typedef struct NsClientApp NsClientApp;

typedef struct NsUiEvent {
    NsClientApp *app;
    NsUiEventType type;
    char *text;
    uint32_t user_id;
} NsUiEvent;

struct NsClientApp {
    GtkApplication *application;
    GtkWindow *window;
    GtkStack *stack;
    GtkWidget *login_page;
    GtkWidget *chat_page;
    GtkEntry *server_entry;
    GtkEntry *port_entry;
    GtkEntry *username_entry;
    GtkEntry *message_entry;
    GtkButton *connect_button;
    GtkButton *send_button;
    GtkLabel *status_label;
    GtkTextView *transcript_view;
    GtkTextBuffer *transcript_buffer;
    GMutex connection_lock;
    GThread *receiver_thread;
    ns_socket_t socket_fd;
    gboolean transport_connected;
    gboolean joined;
    uint32_t user_id;
};

static void ns_client_set_status(NsClientApp *app, const char *text) {
    gtk_label_set_text(app->status_label, text != NULL ? text : "");
}

static void ns_client_append_line(NsClientApp *app, const char *line) {
    GtkTextIter end;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    gtk_text_buffer_get_end_iter(app->transcript_buffer, &end);
    gtk_text_buffer_insert(app->transcript_buffer, &end, line, -1);
    gtk_text_buffer_insert(app->transcript_buffer, &end, "\n", 1);
    gtk_text_buffer_get_end_iter(app->transcript_buffer, &end);
    gtk_text_buffer_place_cursor(app->transcript_buffer, &end);
    gtk_text_view_scroll_mark_onscreen(app->transcript_view,
                                       gtk_text_buffer_get_insert(app->transcript_buffer));
}

static void ns_client_set_login_sensitive(NsClientApp *app, gboolean sensitive) {
    gtk_widget_set_sensitive(GTK_WIDGET(app->server_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->port_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->username_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), sensitive);
}

static void ns_client_join_receiver_thread(NsClientApp *app) {
    GThread *thread = app->receiver_thread;

    if (thread == NULL) {
        return;
    }

    app->receiver_thread = NULL;
    g_thread_join(thread);
}

static ns_socket_t ns_client_take_socket(NsClientApp *app,
                                         gboolean *was_joined,
                                         uint32_t *user_id) {
    ns_socket_t socket_fd = NS_INVALID_SOCKET;

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    app->socket_fd = NS_INVALID_SOCKET;

    if (was_joined != NULL) {
        *was_joined = app->joined;
    }
    if (user_id != NULL) {
        *user_id = app->user_id;
    }

    app->transport_connected = FALSE;
    app->joined = FALSE;
    app->user_id = 0U;
    g_mutex_unlock(&app->connection_lock);

    return socket_fd;
}

static gboolean ns_client_dispatch_ui_event(gpointer data) {
    NsUiEvent *event = (NsUiEvent *) data;

    switch (event->type) {
        case NS_UI_APPEND_LINE:
            ns_client_append_line(event->app, event->text);
            break;
        case NS_UI_CONNECTED:
            gtk_stack_set_visible_child(event->app->stack, event->app->chat_page);
            gtk_widget_set_sensitive(GTK_WIDGET(event->app->send_button), TRUE);
            ns_client_set_login_sensitive(event->app, FALSE);
            ns_client_set_status(event->app, event->text);
            ns_client_append_line(event->app, event->text);
            gtk_widget_grab_focus(GTK_WIDGET(event->app->message_entry));
            break;
        case NS_UI_DISCONNECTED:
            ns_client_join_receiver_thread(event->app);
            gtk_widget_set_sensitive(GTK_WIDGET(event->app->send_button), FALSE);
            ns_client_set_login_sensitive(event->app, TRUE);
            gtk_stack_set_visible_child(event->app->stack, event->app->login_page);
            ns_client_set_status(event->app, event->text);
            break;
        case NS_UI_STATUS:
            ns_client_set_status(event->app, event->text);
            break;
        default:
            break;
    }

    g_free(event->text);
    g_free(event);
    return G_SOURCE_REMOVE;
}

static void ns_client_queue_ui_event(NsClientApp *app,
                                     NsUiEventType type,
                                     const char *text,
                                     uint32_t user_id) {
    NsUiEvent *event = g_new0(NsUiEvent, 1);

    event->app = app;
    event->type = type;
    event->text = g_strdup(text != NULL ? text : "");
    event->user_id = user_id;
    g_idle_add(ns_client_dispatch_ui_event, event);
}

static void ns_client_disconnect(NsClientApp *app, gboolean notify_server) {
    gboolean was_joined = FALSE;
    uint32_t user_id = 0U;
    ns_socket_t socket_fd = ns_client_take_socket(app, &was_joined, &user_id);

    if (ns_socket_is_valid(socket_fd)) {
        if (notify_server && was_joined) {
            NsPacket leave_packet;
            if (ns_packet_set(&leave_packet,
                              NS_PACKET_LEAVE,
                              user_id,
                              ns_unix_time_now(),
                              "") == 0) {
                (void) ns_send_packet(socket_fd, &leave_packet);
            }
        }

        ns_socket_shutdown(socket_fd);
        ns_socket_close(socket_fd);
    }

    ns_client_join_receiver_thread(app);
}

static gpointer ns_client_receive_loop(gpointer data) {
    NsClientApp *app = (NsClientApp *) data;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    char line_buffer[NS_PACKET_BODY_MAX + 64U];

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    g_mutex_unlock(&app->connection_lock);

    while (ns_socket_is_valid(socket_fd)) {
        NsPacket packet;
        int receive_status = ns_recv_packet(socket_fd, &packet);

        if (receive_status <= 0) {
            break;
        }

        switch (packet.header.type) {
            case NS_PACKET_ACK:
                g_mutex_lock(&app->connection_lock);
                app->joined = TRUE;
                app->user_id = packet.header.sender_id;
                g_mutex_unlock(&app->connection_lock);
                ns_client_queue_ui_event(app,
                                         NS_UI_CONNECTED,
                                         packet.body,
                                         packet.header.sender_id);
                break;
            case NS_PACKET_JOIN:
            case NS_PACKET_TEXT:
            case NS_PACKET_LEAVE:
                ns_client_queue_ui_event(app, NS_UI_APPEND_LINE, packet.body, packet.header.sender_id);
                break;
            case NS_PACKET_ERROR:
                snprintf(line_buffer, sizeof(line_buffer), "Server error: %s", packet.body);
                ns_client_queue_ui_event(app, NS_UI_APPEND_LINE, line_buffer, 0U);
                ns_client_queue_ui_event(app, NS_UI_STATUS, packet.body, 0U);
                break;
            default:
                ns_client_queue_ui_event(app,
                                         NS_UI_APPEND_LINE,
                                         "Received an unknown packet from the server.",
                                         0U);
                break;
        }
    }

    socket_fd = ns_client_take_socket(app, NULL, NULL);
    if (ns_socket_is_valid(socket_fd)) {
        ns_socket_shutdown(socket_fd);
        ns_socket_close(socket_fd);
    }

    ns_client_queue_ui_event(app, NS_UI_DISCONNECTED, "Disconnected from server.", 0U);
    return NULL;
}

static void ns_client_on_send_clicked(GtkButton *button, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;
    const char *message = gtk_editable_get_text(GTK_EDITABLE(app->message_entry));
    NsPacket packet;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    gboolean joined = FALSE;
    uint32_t user_id = 0U;

    (void) button;

    if (message == NULL || message[0] == '\0') {
        return;
    }

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    joined = app->joined;
    user_id = app->user_id;
    g_mutex_unlock(&app->connection_lock);

    if (!ns_socket_is_valid(socket_fd) || !joined) {
        ns_client_set_status(app, "Connect to the server before sending messages.");
        return;
    }

    if (ns_packet_set(&packet, NS_PACKET_TEXT, user_id, ns_unix_time_now(), message) != 0) {
        ns_client_set_status(app, "Messages must be 512 characters or fewer.");
        return;
    }

    if (ns_send_packet(socket_fd, &packet) != 0) {
        ns_client_set_status(app, "The message could not be sent.");
        ns_client_disconnect(app, FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
        ns_client_set_login_sensitive(app, TRUE);
        gtk_stack_set_visible_child(app->stack, app->login_page);
        return;
    }

    gtk_editable_set_text(GTK_EDITABLE(app->message_entry), "");
}

static void ns_client_on_message_entry_activate(GtkEntry *entry, gpointer user_data) {
    ns_client_on_send_clicked(NULL, user_data);
    (void) entry;
}

static void ns_client_on_connect_clicked(GtkButton *button, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;
    const char *host = gtk_editable_get_text(GTK_EDITABLE(app->server_entry));
    const char *port = gtk_editable_get_text(GTK_EDITABLE(app->port_entry));
    const char *username = gtk_editable_get_text(GTK_EDITABLE(app->username_entry));
    NsPacket join_packet;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    char error_buffer[256];

    (void) button;

    if (host == NULL || host[0] == '\0' ||
        port == NULL || port[0] == '\0' ||
        username == NULL || username[0] == '\0') {
        ns_client_set_status(app, "Enter a host, port, and username.");
        return;
    }

    if (strlen(username) > NS_USERNAME_MAX) {
        ns_client_set_status(app, "Usernames must be 32 characters or fewer.");
        return;
    }

    socket_fd = ns_connect_tcp(host, port, error_buffer, sizeof(error_buffer));
    if (!ns_socket_is_valid(socket_fd)) {
        ns_client_set_status(app, error_buffer);
        return;
    }

    if (ns_packet_set(&join_packet, NS_PACKET_JOIN, 0U, ns_unix_time_now(), username) != 0) {
        ns_socket_close(socket_fd);
        ns_client_set_status(app, "That username is too long.");
        return;
    }

    g_mutex_lock(&app->connection_lock);
    app->socket_fd = socket_fd;
    app->transport_connected = TRUE;
    app->joined = FALSE;
    app->user_id = 0U;
    g_mutex_unlock(&app->connection_lock);

    ns_client_set_login_sensitive(app, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    ns_client_set_status(app, "Connecting...");

    if (ns_send_packet(socket_fd, &join_packet) != 0) {
        ns_client_set_status(app, "Unable to send the join request.");
        ns_client_disconnect(app, FALSE);
        ns_client_set_login_sensitive(app, TRUE);
        return;
    }

    app->receiver_thread = g_thread_new("nodesignal-recv", ns_client_receive_loop, app);
}

static gboolean ns_client_on_close_request(GtkWindow *window, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) window;

    ns_client_disconnect(app, TRUE);
    return FALSE;
}

static void ns_client_apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    GdkDisplay *display = gdk_display_get_default();

    if (display == NULL) {
        g_object_unref(provider);
        return;
    }

    gtk_css_provider_load_from_path(provider, NS_ASSET_DIR "/style.css");
    gtk_style_context_add_provider_for_display(display,
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static int ns_client_load_ui(NsClientApp *app) {
    GtkBuilder *builder = gtk_builder_new_from_file(NS_ASSET_DIR "/client.ui");

    app->window = GTK_WINDOW(gtk_builder_get_object(builder, "main_window"));
    app->stack = GTK_STACK(gtk_builder_get_object(builder, "main_stack"));
    app->login_page = GTK_WIDGET(gtk_builder_get_object(builder, "login_page"));
    app->chat_page = GTK_WIDGET(gtk_builder_get_object(builder, "chat_page"));
    app->server_entry = GTK_ENTRY(gtk_builder_get_object(builder, "server_entry"));
    app->port_entry = GTK_ENTRY(gtk_builder_get_object(builder, "port_entry"));
    app->username_entry = GTK_ENTRY(gtk_builder_get_object(builder, "username_entry"));
    app->message_entry = GTK_ENTRY(gtk_builder_get_object(builder, "message_entry"));
    app->connect_button = GTK_BUTTON(gtk_builder_get_object(builder, "connect_button"));
    app->send_button = GTK_BUTTON(gtk_builder_get_object(builder, "send_button"));
    app->status_label = GTK_LABEL(gtk_builder_get_object(builder, "status_label"));
    app->transcript_view = GTK_TEXT_VIEW(gtk_builder_get_object(builder, "transcript_view"));

    if (app->window == NULL || app->stack == NULL || app->login_page == NULL ||
        app->chat_page == NULL || app->server_entry == NULL || app->port_entry == NULL ||
        app->username_entry == NULL || app->message_entry == NULL ||
        app->connect_button == NULL || app->send_button == NULL ||
        app->status_label == NULL || app->transcript_view == NULL) {
        g_object_unref(builder);
        return -1;
    }

    app->transcript_buffer = gtk_text_view_get_buffer(app->transcript_view);
    gtk_window_set_application(app->window, app->application);
    gtk_stack_set_visible_child(app->stack, app->login_page);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    ns_client_set_status(app, "Enter a server and username to connect.");

    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(ns_client_on_connect_clicked), app);
    g_signal_connect(app->send_button, "clicked", G_CALLBACK(ns_client_on_send_clicked), app);
    g_signal_connect(app->message_entry, "activate", G_CALLBACK(ns_client_on_message_entry_activate), app);
    g_signal_connect(app->window, "close-request", G_CALLBACK(ns_client_on_close_request), app);

    g_object_unref(builder);
    return 0;
}

static void ns_client_on_activate(GtkApplication *application, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    if (app->window != NULL) {
        gtk_window_present(app->window);
        return;
    }

    app->application = application;
    ns_client_apply_css();

    if (ns_client_load_ui(app) != 0) {
        g_printerr("Failed to load the GTK user interface.\n");
        g_application_quit(G_APPLICATION(application));
        return;
    }

    gtk_window_present(app->window);
}

static void ns_client_on_shutdown(GApplication *application, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) application;
    ns_client_disconnect(app, TRUE);
}

int ns_client_run(int argc, char **argv) {
    GtkApplication *application = NULL;
    NsClientApp app;
    int status = 0;

    (void) argc;
    (void) argv;

    memset(&app, 0, sizeof(app));
    app.socket_fd = NS_INVALID_SOCKET;
    g_mutex_init(&app.connection_lock);

    application = gtk_application_new("com.nodesignal.messenger",
                                      G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(ns_client_on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(ns_client_on_shutdown), &app);

    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_mutex_clear(&app.connection_lock);
    return status;
}

int main(int argc, char **argv) {
    int net_status = ns_net_init();
    int app_status = 0;

    if (net_status != 0) {
        fprintf(stderr, "Network initialization failed.\n");
        return EXIT_FAILURE;
    }

    app_status = ns_client_run(argc, argv);
    ns_net_cleanup();
    return app_status;
}
