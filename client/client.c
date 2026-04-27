/* ===================================================================================
client.c -- Implements the NodeSignal client application
    -- Creates & manages the GTK user interface
    -- Connects to the server over TCP
    -- Sends & receives protocol packets
    -- Updates the UI based on connection & chat events
    -- Runs a background receive loop for incoming messages
=================================================================================== */

#include "client.h"

#include "comm.h"

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* typedef enum NsUiEventType -- Represents the different types of UI events used by the client

    -- Defines the event types queued from background networking logic
    -- Used so UI updates can safely run on the GTK main thread

    -- NS_UI_APPEND_LINE: Appends a new line of text to the chat transcript
    -- NS_UI_CONNECTED: Updates the UI after a successful connection & join
    -- NS_UI_DISCONNECTED: Updates the UI after the client disconnects
    -- NS_UI_STATUS: Updates the status label text in the client UI
    */
typedef enum NsUiEventType {
    NS_UI_APPEND_LINE = 1,
    NS_UI_CONNECTED = 2,
    NS_UI_DISCONNECTED = 3,
    NS_UI_STATUS = 4
} NsUiEventType;

/* typedef struct NsClientApp -- Forward declaration of the main client application structure

    -- Allows the program to reference NsClientApp before its full structure definition appears later in the file
    */
typedef struct NsClientApp NsClientApp;

/* typedef struct NsUiEvent -- Represents a UI event queued from background logic to the GTK main thread

    -- NsClientApp *app: The client application instance associated with the event
    -- NsUiEventType type: The type of UI event being queued
    -- char *text: The text payload associated with the event
    -- uint32_t user_id: The user ID associated with the event when needed
    */
typedef struct NsUiEvent {
    NsClientApp *app;
    NsUiEventType type;
    char *text;
    uint32_t user_id;
} NsUiEvent;

/* struct NsClientApp -- Represents the overall state of the client application

    -- GtkApplication *application: The GTK application instance for the client
    -- GtkWindow *window: The main application window
    -- GtkStack *stack: The GTK stack used to switch between the login & chat pages
    
    -- GtkWidget *login_page: The login page shown before connecting
    -- GtkWidget *chat_page: The chat page shown after connecting
    -- GtkEntry *server_entry: The text entry for the server address
    -- GtkEntry *port_entry: The text entry for the server port
    -- GtkEntry *username_entry: The text entry for the username
    -- GtkEntry *message_entry: The text entry for composing chat messages
    -- GtkButton *connect_button: The button used to connect to the server
    -- GtkButton *send_button: The button used to send a chat message
    
    -- GtkLabel *status_label: The label used to display connection or status messages
    -- GtkTextView *transcript_view: The text view used to display the chat transcript
    -- GtkTextBuffer *transcript_buffer: The text buffer backing the transcript view
    
    -- GMutex connection_lock: The mutex used to protect shared connection state
    -- GThread *receiver_thread: The background thread used to receive incoming packets
    -- ns_socket_t socket_fd: The socket connected to the server
    -- gboolean transport_connected: Whether the TCP transport connection is active
    -- gboolean joined: Whether the client has successfully joined the chat
    -- uint32_t user_id: The user ID assigned by the server after joining
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
    GMutex connection_lock;
    GThread *receiver_thread;
    ns_socket_t socket_fd;
    gboolean transport_connected;
    gboolean joined;
    uint32_t user_id;
    char *asset_dir;
};

/* static void ns_client_set_status -- Updates the client's status label text

    -- Acts as a helper function for displaying status messages in the UI
    -- Used when the client needs to show connection, error, or general state messages

    -- NsClientApp *app: The client application whose status label will be updated
    -- const char *text: The status text to display

    -- Calls gtk_label_set_text() to update the status label
    -- If text is NULL, displays an empty string instead
    */
static void ns_client_set_status(NsClientApp *app, const char *text) {
    gtk_label_set_text(app->status_label, text != NULL ? text : "");
}

/* static char *ns_client_build_asset_path -- Builds the absolute path to a runtime asset

    -- Acts as a helper function for locating packaged client files relative to the executable
    -- Used when the client needs to load client.ui or style.css from the installed assets directory

    -- const NsClientApp *app: The client application containing the resolved asset directory
    -- const char *filename: The asset file name to append to the asset directory

    -- Calls g_build_filename() to construct and return the full asset path
    */
static char *ns_client_build_asset_path(const NsClientApp *app, const char *filename) {
    return g_build_filename(app->asset_dir, filename, NULL);
}

/* static gboolean ns_client_scroll_to_bottom -- Scrolls the chat transcript view to the most recent line

    -- Acts as a helper function for ensuring the latest appended text is visible in the transcript
    -- Used as an idle callback so scrolling occurs after GTK has completed layout and rendering

    -- gpointer data: Pointer to the NsClientApp structure for the running client

    -- Casts data to NsClientApp *app
    -- Retrieves the insert mark from the transcript buffer, which represents the current cursor position
    -- Calls gtk_text_view_scroll_to_mark() to scroll the transcript view so the insert mark is visible
        -- Uses vertical alignment of 1.0 to position the view at the bottom of the buffer

    -- Returns G_SOURCE_REMOVE so the idle callback runs only once
    */
static gboolean ns_client_scroll_to_bottom(gpointer data) {
    NsClientApp *app = (NsClientApp *)data;

    GtkTextMark *mark =
        gtk_text_buffer_get_insert(app->transcript_buffer);

    gtk_text_view_scroll_to_mark(app->transcript_view, mark, 0.0, TRUE, 0.0, 1.0);

    return G_SOURCE_REMOVE;
}

/* static void ns_client_append_line -- Appends a line of text to the chat transcript view

    -- Acts as a helper function for adding new chat or status lines to the transcript
    -- Used when the client needs to display incoming messages or local status updates

    -- NsClientApp *app: The client application whose transcript will be added
    -- const char *line: The line of text to append to the transcript

    -- Declares GtkTextIter end to track the end position of the transcript buffer

    -- If line is NULL or an empty string:
        -- Returns immediately
    
    -- Calls gtk_text_buffer_get_end_iter() to move end to the end of the transcript
    -- Calls gtk_text_buffer_insert() to append the given line
    -- Calls gtk_text_buffer_insert() again to append a newline
    -- Updates end to the new end of the transcript
    -- Calls gtk_text_buffer_place_cursor() to move the text cursor to the end of transcript
    -- Calls gtk_text_view_scroll_mark_onscreen() to keep the latest appended text visible
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

/* static void ns_client_set_login_sensitive -- Enables or disables the login controls in the client UI

    -- Acts as a helper function for changing whether the login related widgets can be edited or clicked
    -- Used when the client connects or disconnects so the login form reflects the current state 

    -- NsClientApp *app: The client application whose login widgets will be updated
    -- gboolean sensitive: Whether the login widgets should be enabled or disabled

    -- Calls gtk_widget_set_sensitive() on:
        -- The server, port, and username entries
        -- The connect button
    */
static void ns_client_set_login_sensitive(NsClientApp *app, gboolean sensitive) {
    gtk_widget_set_sensitive(GTK_WIDGET(app->server_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->port_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->username_entry), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), sensitive);
}

/* static void ns_client_join_receiver_thread -- Joins the background receiver thread if one is active

    -- Acts as a helper function for cleaning up the client's receive thread
    -- Used when the client disconnects or shuts down & must wait for the background thread to finish

    -- NsClientApp *app: The client application whose receiver thread will be joind

    -- Declares GThread *thread & stores app->receiver_thread in it 
    -- If thread is NULL:
        -- Returns immediately
    
    -- Sets app->receiver_thread to NULL
    -- calls g_thread_join() to wait for the receiver thread to finish
    */
static void ns_client_join_receiver_thread(NsClientApp *app) {
    GThread *thread = app->receiver_thread;

    if(thread == NULL) {
        return;
    }

    app->receiver_thread = NULL;
    g_thread_join(thread);
}

/* static ns_socket_t ns_client_take_socket -- Removes the active socket from the client state & resets connection fields

    -- Acts as a helper function for safely taking ownership of the current socket while clearing the shared client state
    -- Used when the client disconnects or shuts down & needs to detach the socket from the application state

    -- NsClientApp *app: The client application whose socket & connection state will be updated
    -- gboolean *was_joined: Optional output parameter that stores whether the client had joined the chat
    -- uint32_t *user_id: Optional output parameter that stores the client's current user ID

    -- Declares ns_socket_t socket_fd = NS_INVALID_SOCKET to store the socket taken from the client state

    -- Locks app->connection_lock with g_mutex_lock()
    -- Copies app->socket_fd into socket_fd
    -- Sets app->socket_fd to NS_INVALID_SOCKET

    -- If was_joined is not NULL:
        -- Stores app->joined in *was_joined
    
    -- If user_id is not NULL:
        -- Stores app->user_id in *user_id

    -- Returns the socket that was removed from the client state
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

/* static gboolean ns_client_dispatch_ui_event -- Applies a queued UI event on the GTK main thread

    -- Acts as a helper function for processing UI updates that were queued from the background client logic
    -- Used by g_idle_add() so that UI changes happen safely on the GTK main thread

    -- gpointer data: Pointer to an NSUiEvent structure describing the UI action to perform

    -- Casts data to NSUiEvent *event

    -- Checks event->type & performs the matching GUI action:
        -- NS_UI_APPEND_LINE:
            -- Appends event->text to the transcript
        -- NS_UI_CONNECTED:
            -- Switches to the chat page
            -- Enables the send button
            -- Disables the login controls
            -- Updates the status label
            -- Appends the connection message to the transcript
            -- Moves keyboard focus to the message entry
        -- NS_UI_DISCONNECTED:
            -- Joins the receiver thread
            -- Disables the send button
            -- Re-enables the login controls
            -- Switches back to the login controls
            -- Updates the status label
        -- NS_UI_STATUS:
            -- Updates the status label only
        -- default:
            -- Performs no action
    
    -- Frees event->text & the NsUiEvent structure
    -- Returns G_SOURCE_REMOVE so that the idle callback only runs once
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

/* static void ns_client_queue_ui_event -- Queues a UI event to be processed later on the GTK main thread

    -- Acts as a helper function for safely requesting UI updates from background logic, such as the receive thread
    -- Used when the client needs to update GTK widgets without touching them directly from another thread

    -- NsClientApp *app: The client application associated with the queued event
    -- NsUiEventType type: The type of UI event to queue
    -- const char *text: The text payload associated with the event
    -- uint32_t user_id: The user ID associated with the event when needed

    -- Allocates a new NsUiEvent with g_new0()
    -- Stores app, type, and user_id in the event
    -- Duplicates text with g_strdup(), using an empty string if text is NULL
    -- Calls g_idle_add() to queue ns_client_dispatch_ui_event() on the GTK main thread
    */
static void ns_client_queue_ui_event(NsClientApp *app, NsUiEventType type, const char *text, uint32_t user_id) {
    NsUiEvent *event = g_new0(NsUiEvent, 1);

    event->app = app;
    event->type = type;
    event->text = g_strdup(text != NULL ? text : "");
    event->user_id = user_id;
    g_idle_add(ns_client_dispatch_ui_event, event);
}

/* static void ns_client_send_leave_if_needed -- Sends a LEAVE packet if the client was joined

    -- Acts as a helper function for notifying the server that the client is leaving
    -- Used when the client disconnects and notify_server is requested

    -- ns_socket_t socket_fd: The active socket connected to the server
    -- gboolean notify_server: Whether the client should send a LEAVE packet
    -- gboolean was_joined: Whether the client had successfully joined the chat
    -- uint32_t user_id: The client’s assigned user ID

    -- Declares NsPacket leave_packet
    -- If notify_server is false or the client had not joined, returns immediately
    -- Calls ns_packet_set() to build a LEAVE packet
    -- If packet creation succeeds, calls ns_send_packet() to send the packet
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

/* static void ns_client_close_socket -- Safely shuts down and closes a socket

    -- Acts as a helper function for cleaning up a network socket
    -- Used by ns_client_disconnect() and other connection teardown code

    -- ns_socket_t socket_fd: The socket to close

    -- Checks if the socket is valid using ns_socket_is_valid()
        -- If invalid, returns immediately
    -- Calls ns_socket_shutdown() to shut down the socket
    -- Calls ns_socket_close() to release the socket resources
    */
static void ns_client_close_socket(ns_socket_t socket_fd) {
    if(!ns_socket_is_valid(socket_fd)) {
        return;
    }

    ns_socket_shutdown(socket_fd);
    ns_socket_close(socket_fd);
}

/* static void ns_client_disconnect -- Safely disconnects the client and resets connection state

    -- Handles both voluntary disconnects and unexpected connection loss
    -- Ensures the socket, receiver thread, and UI state are cleaned up properly

    -- Acts as a helper function for shutting down the current server connection
    -- Used when the client disconnects voluntarily, loses the connection, or shuts down

    -- NsClientApp *app: The client application whose connection should be closed
    -- gboolean notify_server: Whether the client should send a LEAVE packet before disconnecting

    -- Declares gboolean was_joined = FALSE to track whether the client had joined the chat
    -- Declares uint32_t user_id = 0U to store the current user ID
    -- Calls ns_client_take_socket() to remove the socket from the client state & reset connection fields

    -- Calls ns_client_send_leave_if_needed() to optionally notify the server
    -- Calls ns_client_close_socket() to safely shut down and close the socket
    -- Calls ns_client_join_receiver_thread() to wait for the background receive thread to finish
    */
static void ns_client_disconnect(NsClientApp *app, gboolean notify_server) {
    gboolean was_joined = FALSE;
    uint32_t user_id = 0U;

    ns_socket_t socket_fd = ns_client_take_socket(app, &was_joined, &user_id);

    ns_client_send_leave_if_needed(socket_fd, notify_server, was_joined, user_id);

    ns_client_close_socket(socket_fd);

    ns_client_join_receiver_thread(app);
}

/* static void ns_client_handle_packet -- Processes a single received packet

    -- Acts as a helper function for handling incoming packets from the server
    -- Used inside the background receive loop to dispatch UI updates safely

    -- NsClientApp *app: The client application receiving the packet
    -- NsPacket *packet: The packet received from the server

    -- Declares line_buffer[NS_PACKET_BODY_MAX + 64] to format readable messages for the transcript

    -- Switches on packet->header.type to determine handling:
        -- NS_PACKET_ACK:
            -- Locks app->connection_lock
            -- Marks the client as joined and stores the server-assigned user ID
            -- Unlocks the mutex
            -- Queues an NS_UI_CONNECTED event with packet->body and sender ID
        -- NS_PACKET_JOIN, NS_PACKET_TEXT, NS_PACKET_LEAVE:
            -- Queues an NS_UI_APPEND_LINE event with packet->body and sender ID
        -- NS_PACKET_ERROR:
            -- Builds a formatted string "Server error: ..." in line_buffer
            -- Queues NS_UI_APPEND_LINE with line_buffer
            -- Queues NS_UI_STATUS with packet->body
        -- default:
            -- Queues NS_UI_APPEND_LINE indicating an unknown packet was received
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

/* static ns_socket_t ns_client_get_socket_copy -- Safely copies the current client socket

    -- Acts as a helper function for safely accessing the socket from another thread
    -- Used by the receive loop to avoid directly using the shared socket while holding the mutex

    -- NsClientApp *app: The client application containing the socket

    -- Locks app->connection_lock
    -- Copies app->socket_fd into a local variable
    -- Unlocks the mutex
    -- Returns the copied socket descriptor
    */
static ns_socket_t ns_client_get_socket_copy(NsClientApp *app) {
    ns_socket_t socket_fd;

    g_mutex_lock(&app->connection_lock);
    socket_fd = app->socket_fd;
    g_mutex_unlock(&app->connection_lock);

    return socket_fd;
}

/* static gpointer ns_client_receive_loop -- Runs the background loop that receives packets

    -- Acts as a helper function for continuously reading packets from the server
    -- Used in a dedicated thread so incoming messages can be processed without blocking the GTK UI

    -- gpointer data: Pointer to the NsClientApp structure for the running client

    -- Casts data to NsClientApp *app
    -- Calls ns_client_get_socket_copy() to safely fetch the current socket

    -- Loops while the socket remains valid:
        -- Declares NsPacket packet
        -- Calls ns_recv_packet() to receive the next packet
        -- If receive fails or returns 0, breaks the loop
        -- Calls ns_client_handle_packet() to process the received packet

    -- After loop exit:
        -- Calls ns_client_take_socket() to remove the socket from the client state
        -- If socket is valid:
            -- Shuts down and closes the socket
        -- Queues an NS_UI_DISCONNECTED event with "Disconnected from server."

    -- Returns NULL when the background thread finishes
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

/* static void ns_client_on_send_clicked -- Handles the [Send] button click & transmits a chat message

    -- Acts as a GTK callback function for sending a message entered by the user
    -- Used when the [Send] button is clicked on the chat page

    -- GtkButton *button: The GTK button that triggered the callback
    -- gpointer user_data: Pointer to the NsClientApp structure for the running client

    -- Casts user_data to NsClientApp *app
    -- Reads the current message text from app->message_entry
    -- Declares NsPacket packet to store the outgoing TEXT packet
    -- Declares ns_socket_t socket_fd = NS_INVALID_SOCKET to store the current socket
    -- Declares gboolean joined = FALSE to track whether the client has joined the chat
    -- Declares uint32_t user_id = 0U to store the current user ID

    -- Casts button to void since it is otherwise not used

    -- If message is NULL or empty, returns immediately
    -- Locks app->connection_lock
    -- Copies app->socket_fd, app->joined, and app->user_id into local variables
    -- Unlocks app->connection_lock

    -- If the socket is invalid or the client has not joined:
        -- Updates the status label with an error
        -- Returns immediately

    -- Calls ns_packet_set() to build a TEXT packet
        -- If packet creation fails:
            -- Updates the status label with a message-length error
            -- Returns immediately

    -- Calls ns_send_packet() to send the packet
        -- If sending fails:
            -- Updates the status label with a send failure message
            -- Calls ns_client_disconnect() without notifying the server
            -- Disables the send button
            -- Re-enables the login controls
            -- Switches the UI back to the login page
            -- Returns immediately

    -- Clears the message entry after a successful send
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

/* static void ns_client_on_message_entry_activate -- Sends the current message when the message entry is activated

    -- Acts as a GTK callback function for the message entry's activate signal
    -- Used when the user presses [Enter] in the message entry field
    
    -- GtkEntry *entry: The GTK entry widget that triggered the callback
    -- gpointer user_data: Pointer to the NsClientApp structure for the running client

    -- Calls ns_client_on_send_clicked() to reuse the normal send-message logic
    -- Casts entry to void since it is not otherwise used
    */
static void ns_client_on_message_entry_activate(GtkEntry *entry, gpointer user_data) {
    ns_client_on_send_clicked(NULL, user_data);
    (void) entry;
}

/* static gboolean ns_client_validate_login -- Validates host, port, and username

    -- NsClientApp *app: The client application
    -- const char *host, *port, *username: User-entered login inputs

    -- Returns TRUE if all inputs are valid and username is <= NS_USERNAME_MAX
    -- Returns FALSE and sets a status message if any input is invalid
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

/* static int ns_client_build_join_packet -- Constructs a JOIN packet

    -- NsPacket *packet: The packet to populate
    -- const char *username: Username for the join
    -- ns_socket_t socket_fd: Socket to close if packet creation fails

    -- Returns 0 on success, -1 on failure
*/
static int ns_client_build_join_packet(NsPacket *packet, const char *username, ns_socket_t socket_fd) {
    if(ns_packet_set(packet, NS_PACKET_JOIN, 0U, ns_unix_time_now(), username) != 0) {
        ns_socket_close(socket_fd);
        return -1;
    }
    return 0;
}

/* static void ns_client_store_connection -- Safely stores connection state

    -- NsClientApp *app: The client application
    -- ns_socket_t socket_fd: Connected socket

    -- Locks app->connection_lock
    -- Updates app->socket_fd, app->transport_connected, app->joined, app->user_id
    -- Unlocks app->connection_lock
*/
static void ns_client_store_connection(NsClientApp *app, ns_socket_t socket_fd) {
    g_mutex_lock(&app->connection_lock);
    app->socket_fd = socket_fd;
    app->transport_connected = TRUE;
    app->joined = FALSE;
    app->user_id = 0U;
    g_mutex_unlock(&app->connection_lock);
}

/* static void ns_client_on_connect_clicked -- Handles the [Connect] button click & begins the join flow

    -- Acts as a GTK callback for connecting the client to the server
    -- GtkButton *button: The button triggering the callback
    -- gpointer user_data: Pointer to NsClientApp

    -- Step 1: validate login inputs with ns_client_validate_login()
    -- Step 2: connect to the server with ns_connect_tcp()
    -- Step 3: build JOIN packet with ns_client_build_join_packet()
    -- Step 4: store connection state with ns_client_store_connection()
    -- Step 5: update UI: disable login controls, disable send button, status "Connecting..."
    -- Step 6: send JOIN packet; handle send failure by disconnecting and re-enabling login
    -- Step 7: start background receiver thread with g_thread_new()
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

    ns_client_set_login_sensitive(app, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    ns_client_set_status(app, "Connecting...");

    if(ns_send_packet(socket_fd, &join_packet) != 0) {
        ns_client_set_status(app, "Unable to send the join request.");
        ns_client_disconnect(app, FALSE);
        ns_client_set_login_sensitive(app, TRUE);
        return;
    }

    app->receiver_thread = g_thread_new("nodesignal-recv", ns_client_receive_loop, app);
}

/* static gboolean ns_client_on_close_request -- Handles the window close request by disconnecting the client 

    -- Acts as a GTK callback function for the window close-request signal
    -- Used when the user closes the application window

    -- GtkWindow *window: The GTK window that triggered the callback
    -- gpointer user_data: Pointer to the NsClientApp structure for running the client

    -- Casts user_data to NsClientApp *app
    -- Casts window to void since it is not otherwise used
    -- Calls ns_client_disconnect() & requests that the server be notified
    -- Returns FALSE so that GTK continues with the normal window close behavior
    */
static gboolean ns_client_on_close_request(GtkWindow *window, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) window;

    ns_client_disconnect(app, TRUE);
    return FALSE;
}

/* static void ns_client_apply_css -- Loads & applies the client CSS stylesheet

    -- Acts as a helper function for applying custom visual styling to the GTK application
    -- Used during client startup so that the UI uses the project's stylesheet

    -- NsClientApp *app: The client application containing the resolved assets directory
    -- Declares GtkCssProvider *provider to load the CSS file
    -- Declares GdkDisplay *display to access the current display
    -- Declares char *css_path to store the full path to the stylesheet

    -- If display is NULL:
        -- Release the CSS provider & returns immediately

    -- Calls ns_client_build_asset_path() to construct the full stylesheet path
    -- Calls gtk_css_provider_load_from_path() to load the stylesheet from the packaged assets directory
    -- Calls gtk_style_context_add_provider_for_display() to apply the stylesheet to the display
    -- Releases css_path with g_free()
    -- Releases the CSS provider with g_object_unref()
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

/* static int ns_client_load_ui -- Loads the GTK UI from the builder file & initializes widget references

    -- Acts as a helper function for constructing the client interface from client.ui
    -- Used during client startup before the application window is shown

    -- NsClientApp *app: The client application structure that will receive the loaded widget references

    -- Declares GtkBuilder *builder to load the UI definition
    -- Declares char *ui_path to store the full path to client.ui
    -- Calls ns_client_build_asset_path() to build the full UI file path
    -- Calls gtk_builder_add_from_file() to load client.ui from the packaged assets directory

    -- Retrieves and stores pointers to:
        -- The main window
        -- The stack
        -- The login page
        -- The chat page
        -- The server, port, username, and message entries
        -- The connect and send buttons
        -- The status label
        -- The transcript view

    -- If any required widget lookup fails:
        -- Releases the builder
        -- Returns -1
    
    -- Retrieves the transcript buffer from the transcript view
    -- Associates the window with the GTK application
    -- Shows the login page in the stack
    -- Disables the send button initially
    -- Sets the initial status message

    -- Connects GTK signals for:
        -- The connect button click
        -- The send button click
        -- The message entry activate event
        -- The window close-request event
    
    -- Releases the builder
    -- Returns 0 upon success
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
    gtk_window_set_application(app->window, app->application);
    gtk_stack_set_visible_child(app->stack, app->login_page);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    // ns_client_set_status(app, "Enter a server and username to connect.");
    ns_client_set_status(app, "");

    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(ns_client_on_connect_clicked), app);
    g_signal_connect(app->send_button, "clicked", G_CALLBACK(ns_client_on_send_clicked), app);
    g_signal_connect(app->message_entry, "activate", G_CALLBACK(ns_client_on_message_entry_activate), app);
    g_signal_connect(app->window, "close-request", G_CALLBACK(ns_client_on_close_request), app);

    g_object_unref(builder);
    return 0;
}

/* static void ns_client_on_activate -- Handles GTK application activation & shows the main client window 

    -- Acts as a GTK callback function for the application's activate signal
    -- Used when the client application starts or is activated again

    -- GtkApplication *application: The GTK application instance being activated
    -- gpointer user_data: Pointer to the NsClientApp structure for the running client
    
    -- Casts user_data to NsClientApp *app

    -- If app->window is already initialized:
        -- Calls gtk_window_present() to bring the existing window to the front
        -- Returns immediately
    
    -- Stores application in app->application
    -- Calls ns_client_apply_css() to load & apply the client stylesheet

    -- Calls ns_client_load_ui() to load the GTK UI
    -- If UI loading fails:
        -- Prints an error message to stderr
        -- Calls g_application_quit() to shut down the application
        -- Returns immediately
    
    -- Calls gtk_window_present() to show the main window
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

/* static void ns_client_on_shutdown -- Handles GTK application shutdown by disconnecting the client

    -- Acts as a GTK callback function for the application's shutdown signal
    -- Used when the client application is closing & must clean up the server connection

    -- GApplication *application: The GTK application instance being shut down
    -- gpointer user_data: Pointer to the NsClientApp structure for the running client

    -- Casts user_data to NsClientApp *app
    -- Casts application to void since it is not otherwise used
    -- Calls ns_client_disconnect() & requests that the server be notified 
    */
static void ns_client_on_shutdown(GApplication *application, gpointer user_data) {
    NsClientApp *app = (NsClientApp *) user_data;

    (void) application;
    ns_client_disconnect(app, TRUE);
}

/* int ns_client_run -- Initializes & runs the NodeSignal client application

    -- Acts as the main public entry point for starting the GTK client
    -- Used to initialize client state, create the GTK application, connect lifestyle callbacks, and run the event loop

    -- int argc: The number of CLI arguments passed to the program
    -- int **argv: The array of CLI argument strings

    -- Declares GtkApplication *application = NULL to store the GTK application object
    -- Declares NsClientApp app to store the client application's runtime state
    -- Declares int status = 0 to store the final application exit status

    -- Clears app with memset()
    -- Sets app.socket_fd to NS_INVALID_SOCKET
    -- Initializes app.connection_lock with g_mutex_init()

    -- Calls gtk_application_new() to create the GTK application
    -- Uses the application ID "com.nodesignal.messenger"
    -- Uses G_APPLICATION_NON_UNIQUE so that multiple instances may run concurrently

    -- Connects the application's activate signal to ns_client_on_activate()
    -- Connects the application's shutdown singal to ns_client_on_shutdown()

    -- Calls g_application_run() to start the GTK event loop & stores the return value in status
    -- Releases the GTK application with g_object_unref()
    -- Clears app.connection_lock with g_mutex_clear()
    -- Returns status 
    */
int ns_client_run(int argc, char **argv) {
    GtkApplication *application = NULL;
    NsClientApp app;
    char executable_dir[1024];
    int status = 0;

    memset(&app, 0, sizeof(app));
    app.socket_fd = NS_INVALID_SOCKET;
    g_mutex_init(&app.connection_lock);
    if(ns_get_executable_dir(executable_dir, sizeof(executable_dir)) != 0) {
        snprintf(executable_dir, sizeof(executable_dir), ".");
    }
    app.asset_dir = g_build_filename(executable_dir, "assets", NULL);

    application = gtk_application_new("com.nodesignal.messenger", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(ns_client_on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(ns_client_on_shutdown), &app);

    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_free(app.asset_dir);
    g_mutex_clear(&app.connection_lock);
    return status;
}


/* int main -- Entry point of the NodeSignal client program

    -- Acts as the program's starting point
    -- Used to initialize networking, run the client application, clean up networking resources, and return the final exit status

    -- int argc: The number of CLI arguments passed to the program
    -- int **argv: The array of CLI argument strings

    -- Declares int net_status to store the result of network initialization
    -- Declares int app_status = 0 to store the exit status returned by ns_client_run()

    -- Calls ns_net_init() to initialize the networking subsystem
    -- If network initialization fails:
        -- Prints an error message to stderr 
        -- Returns EXIT_FAILURE
    
    -- Calls ns_client_run() to start the client application
    -- Stores the returned exit status in app_status

    -- Calls ns_net_cleanup() to clean up networking resources
    -- Retuns app_status
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
