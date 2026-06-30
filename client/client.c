/* client.c - NodeSignal Messenger GTK4 client application.

Creates and manages the GTK user interface, connects to the server over
TCP, sends and receives protocol packets, and updates the UI in response
to chat events. A dedicated background thread (ns_client_receive_loop)
reads incoming packets and queues GTK-safe idle callbacks so all widget
updates run on the GTK main thread. The connection_lock mutex protects
socket_fd, joined, user_id, and the receiver thread handle.

The sidebar labels sidebar_server_label, sidebar_user_label, and
sidebar_status_dot are supplementary display widgets added in the
redesigned two-panel layout.  They are looked up non-fatally so the
application still runs correctly if the UI file predates the redesign.
*/

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

/* NsClientApp - Complete runtime state of the GTK client.

    All GTK widget pointers are valid only after ns_client_on_activate fires.
    Fields protected by connection_lock: socket_fd, transport_connected,
    joined, user_id, receiver_thread, receiver_thread_active.

    sidebar_server_label, sidebar_user_label, and sidebar_status_dot are
    supplementary widgets from the redesigned two panel layout.  They may
    be NULL if the UI file does not define them; all writes to them are
    guarded by NULL checks so the application degrades gracefully.
*/
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
    GtkTextTag *username_tag;
    GtkLabel *sidebar_server_label;
    GtkLabel *sidebar_user_label;
    GtkLabel *sidebar_status_dot;
    char connected_host[256];
    char connected_port[64];
    char connected_username[NS_USERNAME_MAX + 1];
    GMutex connection_lock;
    GThread *receiver_thread;
    gboolean receiver_thread_active;
    ns_socket_t socket_fd;
    gboolean transport_connected;
    gboolean joined;
    uint32_t user_id;
    char *asset_dir;
};

/* ns_client_set_status - Update the status label text.

    Args:
        app:  Client application whose status_label will be updated.
        text: Text to display; NULL is treated as an empty string.
*/
static void ns_client_set_status(NsClientApp *app, const char *text) {
    gtk_label_set_text(app->status_label, text != NULL ? text : "");
}

/* ns_client_update_sidebar - Populate or clear the sidebar connection labels.

    Called on the GTK main thread. Writes host:port and username into the
    sidebar metadata rows introduced by the redesigned two panel layout.
    All three label pointers are optional; if any is NULL the write is skipped
    so the function is safe to call regardless of which UI version is loaded.

    Args:
        app:      Client application whose sidebar labels will be updated.
        host:     Server hostname string, or NULL to reset to "-".
        port:     Server port string, or NULL to reset to "-".
        username: Active username string, or NULL to reset to "-".
*/
static void ns_client_update_sidebar(NsClientApp *app, const char *host, const char *port, const char *username) {
    if(app->sidebar_server_label != NULL) {
        if(host != NULL && port != NULL) {
            char addr_buf[320];
            snprintf(addr_buf, sizeof(addr_buf), "%s:%s", host, port);
            gtk_label_set_text(app->sidebar_server_label, addr_buf);
        } else {
            gtk_label_set_text(app->sidebar_server_label, "-");
        }
    }

    if(app->sidebar_user_label != NULL) {
        gtk_label_set_text(app->sidebar_user_label, username != NULL ? username : "-");
    }

    if(app->sidebar_status_dot != NULL) {
        if(host != NULL) {
            gtk_widget_remove_css_class(GTK_WIDGET(app->sidebar_status_dot), "ns-offline-dot");
            gtk_widget_add_css_class(GTK_WIDGET(app->sidebar_status_dot), "ns-online-dot");
        } else {
            gtk_widget_remove_css_class(GTK_WIDGET(app->sidebar_status_dot), "ns-online-dot");
            gtk_widget_add_css_class(GTK_WIDGET(app->sidebar_status_dot), "ns-offline-dot");
        }
    }
}

/* ns_client_build_asset_path - Construct an absolute path to a runtime asset file.

    Uses g_build_filename so the result is platform correct. The caller owns
    the returned string and must free it with g_free().

    Args:
        app:      Client application containing the resolved asset directory.
        filename: Asset filename to append (e.g., "client.ui" or "style.css").

    Returns:
        char*: Heap-allocated absolute path string.
*/
static char *ns_client_build_asset_path(const NsClientApp *app, const char *filename) {
    return g_build_filename(app->asset_dir, filename, NULL);
}

/* ns_client_scroll_to_bottom - Scroll the transcript to the most recent line.

    Registered as a one-shot g_idle_add callback so scrolling happens after
    GTK has completed layout and rendering for the newly appended text.

    Args:
        data: Pointer to NsClientApp.

    Returns:
        gboolean: G_SOURCE_REMOVE so the callback runs exactly once.
*/
static gboolean ns_client_scroll_to_bottom(gpointer data) {
    NsClientApp *app = (NsClientApp *)data;

    GtkTextMark *mark =
        gtk_text_buffer_get_insert(app->transcript_buffer);

    gtk_text_view_scroll_to_mark(app->transcript_view, mark, 0.0, TRUE, 0.0, 1.0);

    return G_SOURCE_REMOVE;
}

/* ns_client_append_line - Append one line of text to the chat transcript view.

    If the line contains a colon that is not the first character, the text
    before the colon is rendered with the bold username_tag so sender names
    stand out visually.

    At each step:
        1. Returns immediately if line is NULL or empty.
        2. Searches for a colon separator to identify a "username: message" format.
        3. Inserts the username portion with the bold tag (if applicable).
        4. Inserts the remainder of the line followed by a newline character.
        5. Moves the buffer cursor to the end and queues a scroll-to-bottom idle call.

    Args:
        app:  Client application whose transcript_buffer receives the line.
        line: Null-terminated text to append.
*/
static void ns_client_append_line(NsClientApp *app, const char *line) {
    GtkTextIter end;

    if(line == NULL || line[0] == '\0') {
        return;
    }

    gtk_text_buffer_get_end_iter(app->transcript_buffer, &end);
    const char *colon = strchr(line, ':');

    if(colon != NULL && colon != line) {
        size_t name_len = (size_t)(colon - line);

        if(app->username_tag != NULL && name_len > 0) {
            gtk_text_buffer_insert_with_tags(app->transcript_buffer, &end, line, (gssize) name_len, app->username_tag, NULL);
        } else {
            gtk_text_buffer_insert(app->transcript_buffer, &end, line, (gssize) name_len);
        }
        gtk_text_buffer_insert(app->transcript_buffer, &end, colon, -1);

    } else {
        gtk_text_buffer_insert(app->transcript_buffer, &end, line, -1);
    }

    gtk_text_buffer_insert(app->transcript_buffer, &end, "\n", 1);
    gtk_text_buffer_get_end_iter(app->transcript_buffer, &end);
    gtk_text_buffer_place_cursor(app->transcript_buffer, &end);
    g_idle_add(ns_client_scroll_to_bottom, app);
}

/* ns_client_set_login_sensitive - Enable or disable the login form widgets.

    Called when connecting (disable) or disconnecting (enable) so the user
    cannot change fields while a connection attempt is in progress.

    Args:
        app:       Client application whose login widgets will be updated.
        sensitive: TRUE to enable the widgets, FALSE to disable them.
*/
static void ns_client_set_login_sensitive(NsClientApp *app, gboolean sensitive) {
    gtk_widget_set_sensitive(GTK_WIDGET(app->server_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->port_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->username_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), sensitive);
}

/* ns_client_join_receiver_thread - Join the background receiver thread if one is running.

    Takes receiver_thread and clears receiver_thread_active under the lock,
    then calls g_thread_join() outside the lock so the main thread can never
    join itself. Safe to call if no thread is active (returns immediately).

    Args:
        app: Client application whose receiver thread will be joined.
*/
static void ns_client_join_receiver_thread(NsClientApp *app) {
    GThread *thread = NULL;

    g_mutex_lock(&app->connection_lock);
    if(!app->receiver_thread_active) {
        g_mutex_unlock(&app->connection_lock);
        return;
    }
    thread = app->receiver_thread;
    app->receiver_thread = NULL;
    app->receiver_thread_active = FALSE;
    g_mutex_unlock(&app->connection_lock);

    if(thread != NULL) {
        g_thread_join(thread);
    }
}

/* ns_client_take_socket - Remove and return the active socket from shared state.

    Atomically clears socket_fd, transport_connected, joined, and user_id
    under connection_lock so the caller gets exclusive ownership of the
    socket before performing shutdown/close operations.

    Args:
        app:        Client application whose socket will be taken.
        was_joined: If non-NULL, receives the prior joined state.
        user_id:    If non-NULL, receives the prior user_id.

    Returns:
        ns_socket_t: The socket that was removed (may be NS_INVALID_SOCKET).
*/
static ns_socket_t ns_client_take_socket(NsClientApp *app, gboolean *was_joined, uint32_t *user_id) {
    ns_socket_t socket_fd = NS_INVALID_SOCKET;

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    app->socket_fd = NS_INVALID_SOCKET;

    if(was_joined != NULL) {
        *was_joined = app->joined;
    }
    if(user_id != NULL) {
        *user_id = app->user_id;
    }

    app->transport_connected = FALSE;
    app->joined = FALSE;
    app->user_id = 0U;
    g_mutex_unlock(&app->connection_lock);

    return socket_fd;
}

/* ns_client_dispatch_ui_event - Apply a queued UI event on the GTK main thread.

    Called by GLib's main loop via g_idle_add(). Dispatches the event type
    to the appropriate GTK operations, then frees the event and its text.

    At each step:
        1. Switches on event->type to choose the correct UI action.
        2. NS_UI_APPEND_LINE: appends event->text to the transcript.
        3. NS_UI_CONNECTED: switches to the chat page, enables the send button,
           disables login controls, updates the status label, appends the
           connection message, and focuses the message entry.
        4. NS_UI_DISCONNECTED: joins the receiver thread, disables the send
           button, re-enables login controls, switches back to the login page,
           and updates the status label.
        5. NS_UI_STATUS: updates the status label only.
        6. Frees event->text and the NsUiEvent allocation.

    Args:
        data: Pointer to an NsUiEvent describing the action.

    Returns:
        gboolean: G_SOURCE_REMOVE so the callback runs exactly once.
*/
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
            ns_client_update_sidebar(event->app,
                                     event->app->connected_host,
                                     event->app->connected_port,
                                     event->app->connected_username);
            gtk_widget_grab_focus(GTK_WIDGET(event->app->message_entry));
            break;

        case NS_UI_DISCONNECTED:
            ns_client_join_receiver_thread(event->app);
            gtk_widget_set_sensitive(GTK_WIDGET(event->app->send_button), FALSE);
            ns_client_set_login_sensitive(event->app, TRUE);
            gtk_stack_set_visible_child(event->app->stack, event->app->login_page);
            ns_client_set_status(event->app, event->text);
            ns_client_update_sidebar(event->app, NULL, NULL, NULL);
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

/* ns_client_queue_ui_event - Queue a UI event for dispatch on the GTK main thread.

    Allocates an NsUiEvent and schedules ns_client_dispatch_ui_event via
    g_idle_add() so widget updates are always performed on the correct thread,
    regardless of which thread calls this function.

    Args:
        app:     Client application associated with the event.
        type:    NsUiEventType describing which UI action to perform.
        text:    Text payload; duplicated with g_strdup (NULL becomes "").
        user_id: User ID associated with the event when relevant.
*/
static void ns_client_queue_ui_event(NsClientApp *app, NsUiEventType type, const char *text, uint32_t user_id) {
    NsUiEvent *event = g_new0(NsUiEvent, 1);

    event->app = app;
    event->type = type;
    event->text = g_strdup(text != NULL ? text : "");
    event->user_id = user_id;
    g_idle_add(ns_client_dispatch_ui_event, event);
}

/* ns_client_send_leave_if_needed - Send a LEAVE packet to the server if appropriate.

    Only sends if notify_server is TRUE and the client had previously joined.

    Args:
        socket_fd:     Active socket connected to the server.
        notify_server: Whether the client should notify the server.
        was_joined:    Whether the client had successfully joined the chat.
        user_id:       The client's assigned user ID to embed in the packet.
*/
static void ns_client_send_leave_if_needed(ns_socket_t socket_fd, gboolean notify_server, gboolean was_joined, uint32_t user_id) {
    NsPacket leave_packet;

    if(!notify_server || !was_joined) {
        return;
    }

    if(ns_packet_set(&leave_packet, NS_PACKET_LEAVE, user_id, ns_unix_time_now(), "") == 0) {
        (void) ns_send_packet(socket_fd, &leave_packet);
    }
}

/* ns_client_close_socket - Shut down and close socket_fd if it is valid. */
static void ns_client_close_socket(ns_socket_t socket_fd) {
    if(!ns_socket_is_valid(socket_fd)) {
        return;
    }

    ns_socket_shutdown(socket_fd);
    ns_socket_close(socket_fd);
}

/* ns_client_disconnect - Safely disconnect the client and reset all connection state.

    At each step:
        1. Calls ns_client_take_socket() to atomically claim the socket and
           clear joined, transport_connected, and user_id.
        2. Optionally sends a LEAVE packet via ns_client_send_leave_if_needed().
        3. Shuts down and closes the socket.
        4. Joins the background receiver thread.

    Args:
        app:           Client application to disconnect.
        notify_server: TRUE to send a LEAVE packet before closing.
*/
static void ns_client_disconnect(NsClientApp *app, gboolean notify_server) {
    gboolean was_joined = FALSE;
    uint32_t user_id = 0U;

    ns_socket_t socket_fd = ns_client_take_socket(app, &was_joined, &user_id);

    ns_client_send_leave_if_needed(socket_fd, notify_server, was_joined, user_id);

    ns_client_close_socket(socket_fd);

    ns_client_join_receiver_thread(app);
}

/* ns_client_handle_packet - Process one received packet and queue the appropriate UI event.

    At each step:
        1. Switches on packet->header.type.
        2. NS_PACKET_ACK: under connection_lock, marks app->joined and stores user_id,
           then queues NS_UI_CONNECTED with the ACK body as the status text.
        3. NS_PACKET_JOIN/TEXT/LEAVE: queues NS_UI_APPEND_LINE with packet->body.
        4. NS_PACKET_ERROR: queues NS_UI_APPEND_LINE with a formatted error prefix,
           then queues NS_UI_STATUS to update the status label separately.
        5. default: queues NS_UI_APPEND_LINE with an "unknown packet" notice.

    Args:
        app:    Client application receiving the packet.
        packet: Packet received from the server.
*/
static void ns_client_handle_packet(NsClientApp *app, NsPacket *packet) {
    char line_buffer[NS_PACKET_BODY_MAX + 64U];

    switch(packet->header.type) {
        case NS_PACKET_ACK:
            g_mutex_lock(&app->connection_lock);
            app->joined = TRUE;
            app->user_id = packet->header.sender_id;
            g_mutex_unlock(&app->connection_lock);

            ns_client_queue_ui_event(app, NS_UI_CONNECTED, packet->body, packet->header.sender_id);
            break;

        case NS_PACKET_JOIN:
        case NS_PACKET_TEXT:
        case NS_PACKET_LEAVE:
            ns_client_queue_ui_event(app, NS_UI_APPEND_LINE, packet->body, packet->header.sender_id);
            break;

        case NS_PACKET_ERROR:
            snprintf(line_buffer, sizeof(line_buffer), "Server error: %s", packet->body);

            ns_client_queue_ui_event(app, NS_UI_APPEND_LINE, line_buffer, 0U);
            ns_client_queue_ui_event(app, NS_UI_STATUS, packet->body, 0U);
            break;

        default:
            ns_client_queue_ui_event(app, NS_UI_APPEND_LINE, "Received an unknown packet from the server.", 0U);
            break;
    }
}

/* ns_client_get_socket_copy - Safely read the current socket under connection_lock.

    Args:
        app: Client application containing the shared socket.

    Returns:
        ns_socket_t: A snapshot of app->socket_fd at the time of the call.
*/
static ns_socket_t ns_client_get_socket_copy(NsClientApp *app) {
    ns_socket_t socket_fd;

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    g_mutex_unlock(&app->connection_lock);

    return socket_fd;
}

/* ns_client_receive_loop - Background thread that reads packets from the server.

    Runs until the socket closes or ns_recv_packet returns an error. After
    the loop exits, takes and closes the socket (if the main thread has not
    already done so) and queues NS_UI_DISCONNECTED to restore the login UI.

    At each step:
        1. Reads a snapshot of the current socket under connection_lock.
        2. Loops calling ns_recv_packet(); breaks on receive_status <= 0.
        3. On each successful packet, calls ns_client_handle_packet().
        4. After the loop, calls ns_client_take_socket() to claim any remaining socket.
        5. Shuts down and closes the socket if it is still valid.
        6. Queues NS_UI_DISCONNECTED with a "Disconnected from server." message.

    Args:
        data: Pointer to NsClientApp.

    Returns:
        gpointer: NULL when the thread finishes.
*/
static gpointer ns_client_receive_loop(gpointer data) {
    NsClientApp *app = (NsClientApp *) data;
    ns_socket_t socket_fd = ns_client_get_socket_copy(app);

    while(ns_socket_is_valid(socket_fd)) {
        NsPacket packet;
        int receive_status = ns_recv_packet(socket_fd, &packet);

        if(receive_status <= 0) {
            break;
        }

        ns_client_handle_packet(app, &packet);
    }

    socket_fd = ns_client_take_socket(app, NULL, NULL);
    if(ns_socket_is_valid(socket_fd)) {
        ns_socket_shutdown(socket_fd);
        ns_socket_close(socket_fd);
    }

    ns_client_queue_ui_event(app, NS_UI_DISCONNECTED, "Disconnected from server.", 0U);

    return NULL;
}

/* ns_client_on_send_clicked - GTK callback: send the current message entry text.

    At each step:
        1. Reads message text from app->message_entry; returns if empty.
        2. Reads socket_fd and joined under connection_lock.
        3. Returns with a status error if the socket is invalid or client is not joined.
        4. Calls ns_packet_set() to build a TEXT packet; shows an error if the text is too long.
        5. Calls ns_send_packet(); on failure, calls ns_client_disconnect() and restores the
           login UI (the receiver thread will queue NS_UI_DISCONNECTED asynchronously).
        6. Clears the message entry on success.

    Args:
        button:    The Send button (unused beyond triggering the callback).
        user_data: Pointer to NsClientApp.
*/
static void ns_client_on_send_clicked(GtkButton *button, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;
    const char *message = gtk_editable_get_text(GTK_EDITABLE(app->message_entry));
    NsPacket packet;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    gboolean joined = FALSE;
    uint32_t user_id = 0U;

    (void) button;

    if(message == NULL || message[0] == '\0') {
        return;
    }

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    joined = app->joined;
    user_id = app->user_id;
    g_mutex_unlock(&app->connection_lock);

    if(!ns_socket_is_valid(socket_fd) || !joined) {
        ns_client_set_status(app, "Connect to the server before sending messages.");
        return;
    }

    if(ns_packet_set(&packet, NS_PACKET_TEXT, user_id, ns_unix_time_now(), message) != 0) {
        ns_client_set_status(app, "Messages must be 512 characters or fewer.");
        return;
    }

    if(ns_send_packet(socket_fd, &packet) != 0) {
        ns_client_set_status(app, "The message could not be sent.");
        ns_client_disconnect(app, FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
        ns_client_set_login_sensitive(app, TRUE);
        gtk_stack_set_visible_child(app->stack, app->login_page);
        return;
    }

    gtk_editable_set_text(GTK_EDITABLE(app->message_entry), "");
}

/* ns_client_on_message_entry_activate - GTK callback: send the message when Enter is pressed.

    Delegates to ns_client_on_send_clicked so the behavior is identical to
    clicking the Send button.

    Args:
        entry:     The message GtkEntry (unused beyond triggering the callback).
        user_data: Pointer to NsClientApp.
*/
static void ns_client_on_message_entry_activate(GtkEntry *entry, gpointer user_data) {
    ns_client_on_send_clicked(NULL, user_data);
    (void) entry;
}

/* ns_client_validate_login - Validate host, port, and username before connecting.

    At each step:
        1. Checks that host, port, and username are all non-NULL and non-empty.
        2. Checks that username does not exceed NS_USERNAME_MAX characters.
        3. Calls ns_client_set_status() with a descriptive message on failure.

    Args:
        app:      Client application whose status label receives error messages.
        host:     Server hostname or IP string from the login form.
        port:     Port number string from the login form.
        username: Username string from the login form.

    Returns:
        gboolean: TRUE if all inputs are valid, FALSE otherwise.
*/
static gboolean ns_client_validate_login(NsClientApp *app, const char *host, const char *port, const char *username) {
    if(host == NULL || host[0] == '\0' ||
        port == NULL || port[0] == '\0' ||
        username == NULL || username[0] == '\0') {
        ns_client_set_status(app, "Enter a host, port, and username.");
        return FALSE;
    }

    if(strlen(username) > NS_USERNAME_MAX) {
        ns_client_set_status(app, "Usernames must be 32 characters or fewer.");
        return FALSE;
    }

    return TRUE;
}

/* ns_client_build_join_packet - Construct a JOIN packet from the given username.

    Closes socket_fd and returns -1 if ns_packet_set fails, so the caller does
    not need to handle the partial-connection state separately.

    Args:
        packet:    Packet structure to populate.
        username:  Username to embed as the packet body.
        socket_fd: Socket to close on failure.

    Returns:
        int: 0 on success, -1 if the username is too long for the packet body.
*/
static int ns_client_build_join_packet(NsPacket *packet, const char *username, ns_socket_t socket_fd) {
    if(ns_packet_set(packet, NS_PACKET_JOIN, 0U, ns_unix_time_now(), username) != 0) {
        ns_socket_close(socket_fd);
        return -1;
    }
    return 0;
}

/* ns_client_store_connection - Store a new socket in shared state under connection_lock.

    Sets socket_fd and transport_connected to TRUE; resets joined and user_id
    so the client is in the "connected but not yet joined" state.

    Args:
        app:       Client application to update.
        socket_fd: Newly connected socket to store.
*/
static void ns_client_store_connection(NsClientApp *app, ns_socket_t socket_fd) {
    g_mutex_lock(&app->connection_lock);
    app->socket_fd = socket_fd;
    app->transport_connected = TRUE;
    app->joined = FALSE;
    app->user_id = 0U;
    g_mutex_unlock(&app->connection_lock);
}

/* ns_client_on_connect_clicked - GTK callback: begin the server connection and join flow.

    At each step:
        1. Reads host, port, and username from the login form entries.
        2. Validates inputs with ns_client_validate_login(); returns on failure.
        3. Calls ns_connect_tcp(); shows the error string on failure.
        4. Builds a JOIN packet via ns_client_build_join_packet().
        5. Stores the socket with ns_client_store_connection().
        6. Updates the UI: disables login controls, disables send, shows "Connecting...".
        7. Sends the JOIN packet; on failure disconnects and re-enables the login form.
        8. Starts the background receiver thread with g_thread_new().

    Args:
        button:    The Connect button (unused beyond triggering the callback).
        user_data: Pointer to NsClientApp.
*/
static void ns_client_on_connect_clicked(GtkButton *button, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;
    const char *host = gtk_editable_get_text(GTK_EDITABLE(app->server_entry));
    const char *port = gtk_editable_get_text(GTK_EDITABLE(app->port_entry));
    const char *username = gtk_editable_get_text(GTK_EDITABLE(app->username_entry));

    NsPacket join_packet;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    char error_buffer[256];

    (void) button;

    if(!ns_client_validate_login(app, host, port, username)) {
        return;
    }

    socket_fd = ns_connect_tcp(host, port, error_buffer, sizeof(error_buffer));
    if(!ns_socket_is_valid(socket_fd)) {
        ns_client_set_status(app, error_buffer);
        return;
    }

    if(ns_client_build_join_packet(&join_packet, username, socket_fd) != 0) {
        ns_client_set_status(app, "That username is too long.");
        return;
    }

    ns_client_store_connection(app, socket_fd);

    snprintf(app->connected_host, sizeof(app->connected_host), "%s", host);
    snprintf(app->connected_port, sizeof(app->connected_port), "%s", port);
    snprintf(app->connected_username, sizeof(app->connected_username), "%s", username);

    ns_client_set_login_sensitive(app, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    ns_client_set_status(app, "Connecting...");

    if(ns_send_packet(socket_fd, &join_packet) != 0) {
        ns_client_set_status(app, "Unable to send the join request.");
        ns_client_disconnect(app, FALSE);
        ns_client_set_login_sensitive(app, TRUE);
        return;
    }

    g_mutex_lock(&app->connection_lock);
    app->receiver_thread_active = TRUE;
    app->receiver_thread = g_thread_new("nodesignal-recv", ns_client_receive_loop, app);
    g_mutex_unlock(&app->connection_lock);
}

/* ns_client_on_close_request - GTK callback: disconnect gracefully when the window closes.

    Sends a LEAVE packet before tearing down the connection so the server
    can broadcast a leave notice to remaining clients.

    Args:
        window:    The main GtkWindow (unused beyond triggering the callback).
        user_data: Pointer to NsClientApp.

    Returns:
        gboolean: FALSE so GTK continues with the normal window close behavior.
*/
static gboolean ns_client_on_close_request(GtkWindow *window, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) window;

    ns_client_disconnect(app, TRUE);
    return FALSE;
}

/* ns_client_apply_css - Load and apply the client stylesheet to the GTK display.

    At each step:
        1. Creates a GtkCssProvider and gets the default GdkDisplay.
        2. Returns immediately if the display is NULL or app->asset_dir is NULL.
        3. Builds the stylesheet path with ns_client_build_asset_path().
        4. Loads the CSS file and adds the provider at APPLICATION priority.
        5. Frees the path and releases the provider.

    Args:
        app: Client application containing the resolved asset directory.
*/
static void ns_client_apply_css(NsClientApp *app) {
    char *css_path = NULL;
    GtkCssProvider *provider = gtk_css_provider_new();
    GdkDisplay *display = gdk_display_get_default();

    if(display == NULL) {
        g_object_unref(provider);
        return;
    }

    if(app == NULL || app->asset_dir == NULL) {
        g_object_unref(provider);
        return;
    }

    css_path = ns_client_build_asset_path(app, "style.css");
    gtk_css_provider_load_from_path(provider, css_path);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css_path);
    g_object_unref(provider);
}

/* ns_client_load_ui - Load the GTK UI from client.ui and initialize widget references.

    At each step:
        1. Builds the UI file path with ns_client_build_asset_path().
        2. Calls gtk_builder_add_from_file(); prints the GError and returns -1 on failure.
        3. Retrieves all required widget pointers from the builder by ID.
        4. Returns -1 if any required widget lookup returns NULL.
        5. Gets the transcript buffer, creates the bold username_tag.
        6. Associates the window with the GTK application.
        7. Shows the login page, disables the send button, sets the initial status.
        8. Connects GTK signals: connect button, send button, message entry activate,
           and window close-request.

    Args:
        app: Client application structure that will receive the widget pointers.

    Returns:
        int: 0 on success, -1 if the UI file cannot be loaded or a widget is missing.
*/
static int ns_client_load_ui(NsClientApp *app) {
    char *ui_path = NULL;
    GError *error = NULL;
    GtkBuilder *builder = gtk_builder_new();

    ui_path = ns_client_build_asset_path(app, "client.ui");
    if(!gtk_builder_add_from_file(builder, ui_path, &error)) {
        if(error != NULL) {
            g_printerr("Failed to load UI file '%s': %s\n", ui_path, error->message);
            g_clear_error(&error);
        }
        g_free(ui_path);
        g_object_unref(builder);
        return -1;
    }
    g_free(ui_path);

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
    app->username_tag = gtk_text_buffer_create_tag(app->transcript_buffer, "username", "weight", PANGO_WEIGHT_BOLD, NULL);

    app->sidebar_server_label = GTK_LABEL(gtk_builder_get_object(builder, "sidebar_server_label"));
    app->sidebar_user_label   = GTK_LABEL(gtk_builder_get_object(builder, "sidebar_user_label"));
    app->sidebar_status_dot   = GTK_LABEL(gtk_builder_get_object(builder, "sidebar_status_dot"));

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

/* ns_client_on_activate - GTK callback: build and show the main window on first activation.

    At each step:
        1. If app->window is already set, calls gtk_window_present() and returns.
        2. Stores the GtkApplication pointer in app->application.
        3. Calls ns_client_apply_css() to apply the stylesheet.
        4. Calls ns_client_load_ui() to build the widget tree; quits on failure.
        5. Calls gtk_window_present() to show the main window.

    Args:
        application: The GtkApplication being activated.
        user_data:   Pointer to NsClientApp.
*/
static void ns_client_on_activate(GtkApplication *application, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    if(app->window != NULL) {
        gtk_window_present(app->window);
        return;
    }

    app->application = application;
    ns_client_apply_css(app);

    if(ns_client_load_ui(app) != 0) {
        g_printerr("Failed to load the GTK user interface.\n");
        g_application_quit(G_APPLICATION(application));
        return;
    }

    gtk_window_present(app->window);
}

/* ns_client_on_shutdown - GTK callback: disconnect gracefully on application shutdown.

    Args:
        application: The GApplication being shut down (unused).
        user_data:   Pointer to NsClientApp.
*/
static void ns_client_on_shutdown(GApplication *application, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) application;
    ns_client_disconnect(app, TRUE);
}

/* ns_client_run - Allocate client state, create the GTK application, and run the event loop.

    At each step:
        1. Allocates NsClientApp on the heap via g_new0 (avoids stack overflow from
           the mutex and large arrays inside the structure).
        2. Sets socket_fd to NS_INVALID_SOCKET and initializes connection_lock.
        3. Resolves the executable directory and builds the asset_dir path.
        4. Creates a G_APPLICATION_NON_UNIQUE GtkApplication so multiple instances
           can run concurrently for testing.
        5. Connects the activate and shutdown signals.
        6. Calls g_application_run() to enter the GTK event loop.
        7. Frees the application object, asset_dir, clears the mutex, and frees app.

    Args:
        argc: Argument count forwarded from main().
        argv: Argument vector forwarded from main().

    Returns:
        int: Exit status from g_application_run().
*/
int ns_client_run(int argc, char **argv) {
    GtkApplication *application = NULL;
    NsClientApp *app = NULL;
    char executable_dir[1024];
    int status = 0;

    app = g_new0(NsClientApp, 1);
    app->socket_fd = NS_INVALID_SOCKET;
    g_mutex_init(&app->connection_lock);
    if(ns_get_executable_dir(executable_dir, sizeof(executable_dir)) != 0) {
        snprintf(executable_dir, sizeof(executable_dir), ".");
    }
    app->asset_dir = g_build_filename(executable_dir, "assets", NULL);

    application = gtk_application_new("com.nodesignal.messenger", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(ns_client_on_activate), app);
    g_signal_connect(application, "shutdown", G_CALLBACK(ns_client_on_shutdown), app);

    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_free(app->asset_dir);
    g_mutex_clear(&app->connection_lock);
    g_free(app);
    return status;
}

/* main - Initialize networking, run the client, clean up, and return the exit status.

    At each step:
        1. Calls ns_net_init(); prints an error and returns EXIT_FAILURE on failure.
        2. Calls ns_client_run() to enter the GTK event loop.
        3. Calls ns_net_cleanup() to release networking resources.
        4. Returns the status from ns_client_run().

    Args:
        argc: Number of command-line arguments.
        argv: Argument strings.

    Returns:
        int: EXIT_SUCCESS or EXIT_FAILURE.
*/
int main(int argc, char **argv) {
    int net_status = ns_net_init();
    int app_status = 0;

    if(net_status != 0) {
        fprintf(stderr, "Network initialization failed.\n");
        return EXIT_FAILURE;
    }

    app_status = ns_client_run(argc, argv);
    ns_net_cleanup();
    return app_status;
}
