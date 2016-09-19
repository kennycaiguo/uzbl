#include "io.h"

#include "commands.h"
#include "events.h"
#include "setup.h"
#include "type.h"
#include "util.h"
#include "variables.h"
#include "uzbl-core.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "extio.h"

struct _UzblIO {
    /* Sockets to connect to as event managers. */
    GPtrArray *connect_sockets;
    /* Sockets to connect to as clients. */
    GPtrArray *client_sockets;

    /* The event buffer. */
    GMutex     event_buffer_lock;
    GPtrArray *event_buffer;

    /* Path to the main FIFO for client communication. */
    gchar *fifo_path;
    /* Path to the main socket for client communication. */
    gchar *socket_path;

    /* The command queue for sending I/O commands from sockets/FIFO/etc. to be
     * run in the main thread. */
    GAsyncQueue  *cmd_q;
    /* The I/O thread variables. */
    GMainContext *io_ctx;
    GMainLoop    *io_loop;
    GThread      *io_thread;

    /* Web extension communication */
    GIOStream *extstream;
    int extfdinfo[2];
};

/* =========================== PUBLIC API =========================== */

static gboolean
flush_event_buffer (gpointer data);
static void
free_cmd_req (gpointer data);
static void
run_command_async (GTask *cmdt, GAsyncReadyCallback callback, gpointer data);
static void
run_command_finish (GTask *cmdt, GAsyncResult *result, GError **error);
static gpointer
run_io (gpointer data);
static void
start_command_loop ();

void
uzbl_io_init ()
{
    uzbl.io = g_malloc (sizeof (UzblIO));

    uzbl.io->connect_sockets = g_ptr_array_new ();
    uzbl.io->client_sockets = g_ptr_array_new ();

    g_mutex_init (&uzbl.io->event_buffer_lock);
    uzbl.io->event_buffer = g_ptr_array_new_with_free_func (g_free);
    g_timeout_add_seconds (10, flush_event_buffer, NULL);

    uzbl.io->fifo_path = NULL;
    uzbl.io->socket_path = NULL;

    uzbl.io->cmd_q = g_async_queue_new_full (g_object_unref);

    start_command_loop ();

    uzbl.io->io_thread = g_thread_new ("uzbl-io", run_io, NULL);
}

void
uzbl_io_free ()
{
    g_ptr_array_unref (uzbl.io->connect_sockets);
    g_ptr_array_unref (uzbl.io->client_sockets);

    if (uzbl.io->event_buffer) {
        g_mutex_lock (&uzbl.io->event_buffer_lock);
        g_ptr_array_unref (uzbl.io->event_buffer);
        uzbl.io->event_buffer = NULL;
        g_mutex_unlock (&uzbl.io->event_buffer_lock);
    }
    g_mutex_clear (&uzbl.io->event_buffer_lock);

    if (uzbl.io->fifo_path) {
        unlink (uzbl.io->fifo_path);
    }
    if (uzbl.io->socket_path) {
        unlink (uzbl.io->socket_path);
    }

    g_free (uzbl.io->fifo_path);
    g_free (uzbl.io->socket_path);

    // TODO: Closing can fail if there is a blocking thread
    // g_async_queue_unref (uzbl.io->cmd_q);
    g_thread_unref (uzbl.io->io_thread);

    g_free (uzbl.io);
    uzbl.io = NULL;
}

typedef gboolean (*UzblIODataCallback)(GIOStream *stream, const gchar* line, gpointer data);
typedef void (*UzblIODataErrorCallback)(GIOStream *stream, gpointer data);
static void
add_buffered_cmd_source (GIOStream *stream, const gchar *name, UzblIODataCallback callback,
                         UzblIODataErrorCallback error_callback, gpointer data);
static gboolean
control_command_stream (GIOStream *stream, const gchar *input, gpointer data);

void
uzbl_io_init_stdin ()
{
    GInputStream *input = g_unix_input_stream_new (STDIN_FILENO, TRUE);
    GOutputStream *output = g_unix_output_stream_new (STDOUT_FILENO, TRUE);
    GIOStream *stream = g_simple_io_stream_new (input, output);
    add_buffered_cmd_source (stream, "Uzbl stdin watcher",
                             control_command_stream, NULL, NULL);
}

static void
close_client_socket (GIOStream *stream, gpointer data);
static void
replay_event_buffer (GIOStream *stream);

gboolean
uzbl_io_init_connect_socket (const gchar *socket_path)
{
    GError *error = NULL;
    GSocketAddress *addr = g_unix_socket_address_new (socket_path);
    GSocketClient *client = g_socket_client_new ();
    GSocketConnection *con;

    con = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (addr),
                                   NULL, &error);

    if (!con) {
        g_warning ("Error connecting to socket %s: %s\n", socket_path, error->message);
        g_error_free (error);
        g_object_unref (client);
        return FALSE;
    }

    add_buffered_cmd_source (G_IO_STREAM (con), "Uzbl connect socket",
                             control_command_stream,
                             close_client_socket,
                             uzbl.io->connect_sockets);
    g_ptr_array_add (uzbl.io->connect_sockets, G_IO_STREAM (con));
    replay_event_buffer (G_IO_STREAM (con));

    g_object_unref (client);

    return TRUE;
}

static void
buffer_event (const gchar *message);
static void
send_event_sockets (GPtrArray *sockets, const gchar *message);

void
uzbl_io_send (const gchar *message, gboolean connect_only)
{
    if (!message) {
        return;
    }

    if (!strchr (message, '\n')) {
        return;
    }

    buffer_event (message);

    if (uzbl_variables_get_int ("print_events")) {
        fprintf (stdout, "%s", message);
        fflush (stdout);
    }

    /* Write to all --connect-socket sockets. */
    send_event_sockets (uzbl.io->connect_sockets, message);

    if (!connect_only) {
        /* Write to all client sockets. */
        send_event_sockets (uzbl.io->client_sockets, message);
    }
}

typedef struct {
    gchar *cmd;
    const UzblCommand *info;
    GArray *argv;
} UzblCommandData;

void
uzbl_io_schedule_command (const UzblCommand *cmd, GArray *argv, GAsyncReadyCallback callback, gpointer data)
{
    if (!cmd || !argv) {
        uzbl_debug ("Invalid command scheduled");
        return;
    }

    UzblCommandData *cmd_data = g_new (UzblCommandData, 1);
    cmd_data->cmd = NULL;
    cmd_data->info = cmd;
    cmd_data->argv = argv;

    GTask *task = g_task_new (NULL, NULL, callback, data);
    g_task_set_task_data (task, cmd_data, free_cmd_req);
    g_async_queue_push (uzbl.io->cmd_q, task);
}

GString *
uzbl_io_command_finish (GObject *source, GAsyncResult *result, GError **error)
{
    UZBL_UNUSED (source);
    GTask *task = G_TASK (result);
    return (GString*) g_task_propagate_pointer (task, error);
}

void
uzbl_io_send_ext_message (ExtIOMessageType type, ...)
{
    va_list vargs;
    va_start (vargs, type);
    GOutputStream *os = g_io_stream_get_output_stream (uzbl.io->extstream);
    uzbl_extio_send_new_messagev (os, type, &vargs);
    va_end (vargs);
}

typedef enum {
    UZBL_COMM_FIFO,
    UZBL_COMM_SOCKET
} UzblCommType;

static gchar *
build_stream_name (UzblCommType type, const gchar *dir);
static int
create_dir (const gchar *dir);

static gboolean
attach_fifo (const gchar *path);

gboolean
uzbl_io_init_fifo (const gchar *dir)
{
    if (create_dir (dir)) {
        return FALSE;
    }

    if (uzbl.io->fifo_path) {
        /* We're changing the fifo path, get rid of the old fifo if one exists. */
        if (unlink (uzbl.io->fifo_path)) {
            g_warning ("Fifo: Can't unlink old fifo at %s\n", uzbl.io->fifo_path);
            return FALSE;
        }
        g_free (uzbl.io->fifo_path);
        uzbl.io->fifo_path = NULL;
    }

    gchar *path = build_stream_name (UZBL_COMM_FIFO, dir);

    /* If something exists at that path, try to delete it. */
    if (file_exists (path) && unlink (path)) {
        g_warning ("Fifo: Can't unlink old fifo at %s\n", path);
    }

    gboolean ret = FALSE;

    if (!mkfifo (path, 0600)) {
      if (attach_fifo (path)) {
         ret = TRUE;
      } else {
         g_warning ("init_fifo: can't attach to %s: %s\n", path, strerror (errno));
      }
    } else {
        g_warning ("init_fifo: can't create %s: %s\n", path, strerror (errno));
    }

    g_free (path);
    return ret;
}

static gboolean
attach_socket (const gchar *path);

gboolean
uzbl_io_init_socket (const gchar *dir)
{
    if (create_dir (dir)) {
        return FALSE;
    }

    if (uzbl.io->socket_path) {
        /* Remove an existing socket should one exist. */
        if (unlink (uzbl.io->socket_path)) {
            g_warning ("init_socket: couldn't unlink socket at %s\n", uzbl.io->socket_path);
            return FALSE;
        }
        g_free (uzbl.io->socket_path);
        uzbl.io->socket_path = NULL;
    }

    if (*dir == ' ') {
        return FALSE;
    }

    gchar *path = build_stream_name (UZBL_COMM_SOCKET, dir);

    /* If something exists at that path, try to delete it. */
    if (file_exists (path) && unlink (path)) {
        g_warning ("Socket: Can't unlink old socket at %s\n", path);
    }

    gboolean ret = FALSE;

    if (attach_socket (path)) {
        ret = TRUE;
    } else {
        g_warning ("init_socket: can't attach to %s: %s\n", path, strerror (errno));
    }

    g_free (path);
    return ret;
}

void
read_message_cb (GObject *stream,
                 GAsyncResult *res,
                 gpointer user_data);

gboolean
uzbl_io_init_extpipe ()
{
    int readfd[2];
    int writefd[2];

    pipe (readfd);
    pipe (writefd);

    GInputStream *input = g_unix_input_stream_new (readfd[0], TRUE);
    GOutputStream *output = g_unix_output_stream_new (writefd[1], TRUE);

    GIOStream *stream = g_simple_io_stream_new (input, output);

    uzbl.io->extstream = stream;
    uzbl.io->extfdinfo[0] = writefd[0];
    uzbl.io->extfdinfo[1] = readfd[1];

    uzbl_extio_read_message_async (input, read_message_cb, NULL);

    return TRUE;
}

void
uzbl_io_extfds (int *input, int *output)
{
    *input = uzbl.io->extfdinfo[0];
    *output = uzbl.io->extfdinfo[1];
}

void
uzbl_io_flush_buffer ()
{
    flush_event_buffer (NULL);
}

void
uzbl_io_quit ()
{
    g_main_loop_quit (uzbl.io->io_loop);
}

/* ===================== HELPER IMPLEMENTATIONS ===================== */

static void
read_command_async (GAsyncQueue         *queue,
                    GAsyncReadyCallback  callback,
                    gpointer             data);

static GTask*
read_command_finish (GAsyncQueue  *queue,
                     GAsyncResult  *result,
                     GError       **error);

static void
wait_for_command (GTask         *task,
                  gpointer       source_object,
                  gpointer       task_data,
                  GCancellable  *cancellable);

static void
read_command_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data);

static void
run_command_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      data);

void
start_command_loop ()
{
    GAsyncQueue *queue = uzbl.io->cmd_q;
    read_command_async (queue, read_command_cb, queue);
}

void
read_command_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
    GAsyncQueue *queue = (GAsyncQueue*) source;
    GError *err;
    GTask *task = read_command_finish (queue, result, &err);
    run_command_async (task, run_command_cb, data);
}

void
run_command_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      data)
{
    GTask *task = G_TASK (source);
    GAsyncQueue *queue = (GAsyncQueue*) data;
    GError *err;
    run_command_finish (task, result, &err);
    read_command_async (queue, read_command_cb, queue);
}

void
read_command_async (GAsyncQueue         *queue,
                    GAsyncReadyCallback  callback,
                    gpointer             data)
{
    GTask *task = g_task_new (NULL, NULL, callback, data);
    g_task_set_task_data (task, queue, NULL);
    g_task_run_in_thread (task, wait_for_command);
    g_object_unref (task);
}

GTask*
read_command_finish (GAsyncQueue  *queue,
                     GAsyncResult  *result,
                     GError       **error)
{
    UZBL_UNUSED (queue);
    GTask *task = G_TASK (result);
    return g_task_propagate_pointer (task, error);
}

void
wait_for_command (GTask         *task,
                  gpointer       source_object,
                  gpointer       task_data,
                  GCancellable  *cancellable)
{
    UZBL_UNUSED (source_object);
    UZBL_UNUSED (cancellable);
    GAsyncQueue *queue = (GAsyncQueue*) task_data;
    gpointer data = g_async_queue_pop (queue);
    g_task_return_pointer (task, data, NULL);
}

static void
send_buffered_event (gpointer event, gpointer data);

gboolean
flush_event_buffer (gpointer data)
{
    UZBL_UNUSED (data);

    if (!uzbl.io->event_buffer) {
        return FALSE;
    }

    g_mutex_lock (&uzbl.io->event_buffer_lock);
    g_ptr_array_foreach (uzbl.io->event_buffer, send_buffered_event, NULL);
    g_ptr_array_free (uzbl.io->event_buffer, TRUE);
    uzbl.io->event_buffer = NULL;
    g_mutex_unlock (&uzbl.io->event_buffer_lock);

    return FALSE;
}

void
free_cmd_req (gpointer data)
{
    UzblCommandData *cmd = (UzblCommandData *)data;

    uzbl_commands_args_free (cmd->argv);
    g_free (cmd->cmd);
    g_free (cmd);
}

void
commands_run_cb (GObject *source, GAsyncResult *res, gpointer data)
{
    GTask *task = G_TASK (data);
    GTask *cmdt = G_TASK (g_task_get_source_object (task));
    GError *err;
    GString *result = uzbl_commands_run_finish (source, res, &err);

    g_task_return_pointer (cmdt, result, free_gstring);
    g_task_return_pointer (task, NULL, NULL);
    g_object_unref (task);
    g_object_unref (cmdt);
}

void
run_command_async (GTask *cmdt, GAsyncReadyCallback callback, gpointer data)
{
    UZBL_UNUSED (data);
    GTask *task = g_task_new (cmdt, NULL, callback, data);
    UzblCommandData *cmd = (UzblCommandData*) g_task_get_task_data (cmdt);

    if (cmd->cmd) {
        uzbl_commands_run_string_async (cmd->cmd, TRUE, commands_run_cb, task);
    } else {
        uzbl_commands_run_async (cmd->info, cmd->argv, TRUE, commands_run_cb, task);
    }
}

void
run_command_finish (GTask *cmdt, GAsyncResult *result, GError **error)
{
    UZBL_UNUSED (cmdt);
    GTask *task = G_TASK (result);
    g_task_propagate_pointer (task, error);
}

gpointer
run_io (gpointer data)
{
    UZBL_UNUSED (data);

    uzbl.io->io_ctx = g_main_context_new ();

    uzbl.io->io_loop = g_main_loop_new (uzbl.io->io_ctx, FALSE);
    g_main_loop_run (uzbl.io->io_loop);
    g_main_loop_unref (uzbl.io->io_loop);
    uzbl.io->io_loop = NULL;

    g_main_context_unref (uzbl.io->io_ctx);
    uzbl.io->io_ctx = NULL;

    return NULL;
}

typedef struct {
    UzblIODataCallback callback;
    UzblIODataErrorCallback error_callback;
    GIOStream *stream;
    gpointer data;
} UzblIOBufferData;

static void
read_line_cb (GObject *source, GAsyncResult *res, gpointer data);

void
add_buffered_cmd_source (GIOStream *stream, const gchar *name,
                         UzblIODataCallback callback,
                         UzblIODataErrorCallback error_callback,
                         gpointer data)
{
    UZBL_UNUSED (name);

    GDataInputStream *ds = g_data_input_stream_new (
        g_io_stream_get_input_stream (stream));

    UzblIOBufferData *io_data = g_malloc (sizeof (UzblIOBufferData));
    io_data->callback = callback;
    io_data->error_callback = error_callback;
    io_data->stream = stream;
    io_data->data = data;

    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL,
                                         read_line_cb, (gpointer) io_data);
}

static void
read_line_cb (GObject *source, GAsyncResult *res, gpointer data)
{
    UzblIOBufferData *io_data = (UzblIOBufferData *)data;
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    GError *error = NULL;
    gsize length;

    gchar *line = g_data_input_stream_read_line_finish (ds, res, &length, &error);
    if (error) {
        g_warning ("Error reading: %s", error->message);
        g_clear_error (&error);

        if (io_data->error_callback) {
            io_data->error_callback (io_data->stream, io_data->data);
            return;
        }
    }

    if (!line) {
        if (io_data->error_callback) {
            io_data->error_callback (io_data->stream, io_data->data);
            return;
        }
    }

    io_data->callback (io_data->stream, line, data);
    g_free (line);

    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL,
                                         read_line_cb, data);
}

static void
schedule_io_input (gchar *line, GAsyncReadyCallback callback, gpointer data);

static void
write_result_to_stream (GObject      *source,
                        GAsyncResult *res,
                        gpointer      data);

gboolean
control_command_stream (GIOStream *stream, const gchar *input, gpointer data)
{
    UZBL_UNUSED (data);

    g_object_ref (G_OBJECT (stream));
    gchar *ctl_line = g_strdup (input);
    schedule_io_input (ctl_line, write_result_to_stream, stream);

    return TRUE;
}

void
close_client_socket (GIOStream *stream, gpointer data)
{
    GError *error = NULL;
    gboolean ok = g_io_stream_close (stream, NULL, &error);
    GPtrArray *socket_array = (GPtrArray *)data;

    if (socket_array) {
        g_ptr_array_remove_fast (socket_array, stream);
    }

    if (!ok) {
        g_warning ("Error shutting down client socket: %s", error->message);
        g_clear_error (&error);
    }
}

static void
send_buffered_event_to_socket (gpointer event, gpointer data);

void
replay_event_buffer (GIOStream *stream)
{
    if (!uzbl.io->event_buffer) {
        return;
    }

    g_mutex_lock (&uzbl.io->event_buffer_lock);
    g_ptr_array_foreach (uzbl.io->event_buffer, send_buffered_event_to_socket, stream);
    g_mutex_unlock (&uzbl.io->event_buffer_lock);
}

void
buffer_event (const gchar *message)
{
    if (!uzbl.io->event_buffer) {
        return;
    }

    g_mutex_lock (&uzbl.io->event_buffer_lock);
    g_ptr_array_add (uzbl.io->event_buffer, g_strdup (message));
    g_mutex_unlock (&uzbl.io->event_buffer_lock);
}

static void
write_to_stream (GIOStream *stream, const gchar *message);

void
send_event_sockets (GPtrArray *sockets, const gchar *message)
{
    g_ptr_array_foreach (sockets, (GFunc)write_to_stream, (gpointer)message);
}

gchar *
build_stream_name (UzblCommType type, const gchar *dir)
{
    gchar *str = NULL;
    gchar *type_str;

    switch (type) {
    case UZBL_COMM_FIFO:
        type_str = "fifo";
        break;
    case UZBL_COMM_SOCKET:
        type_str = "socket";
        break;
    default:
        g_error ("Unknown communication type: %d\n", type);
        return NULL;
    }

    str = g_strdup_printf ("%s/uzbl_%s_%s", dir, type_str, uzbl.state.instance_name);

    return str;
}

int
create_dir (const gchar *dir)
{
    gchar *work_path = g_strdup (dir);
    size_t len = strlen (work_path);
    gchar *p;

    if (!len) {
        return EXIT_FAILURE;
    }

    if (work_path[len - 1] == '/') {
        work_path[len - 1] = '\0';
    }

#define check_mkdir(dir, mode)                              \
    if (mkdir (work_path, 0700)) {                          \
        switch (errno) {                                    \
        case EEXIST:                                        \
            break;                                          \
        default:                                            \
            perror ("Failed to create socket or fifo dir"); \
            return EXIT_FAILURE;                            \
        }                                                   \
    }

    /* Start making the parent directories from the bottom. */
    for (p = work_path + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            check_mkdir (work_path, 0700);
            *p = '/';
        }
    }

    check_mkdir (work_path, 0700);
    g_free (work_path);

    return EXIT_SUCCESS;
}

gboolean
attach_fifo (const gchar *path)
{
    GError *error = NULL;
    GFile *file = g_file_new_for_path (path);

    /* We don't really need to write to the file, but if we open the file as
     * 'r' we will block here, waiting for a writer to open the file. */
    GFileIOStream *stream = g_file_open_readwrite (file, NULL, &error);
    if (!stream) {
        g_warning ("attach_fifo: can't open: %s\n", error->message);
        g_error_free (error);
        return FALSE;
    }

    GOutputStream *os = g_io_stream_get_output_stream (G_IO_STREAM (stream));
    if (!g_output_stream_close (os, NULL, &error)) {
        g_warning ("Failed to close write end of fifo: %s\n", error->message);
        g_error_free (error);
        return FALSE;
    }

    add_buffered_cmd_source (G_IO_STREAM (stream), "Uzbl main fifo",
                             control_command_stream, NULL, NULL);
    uzbl.io->fifo_path = g_strdup (path);
    uzbl_events_send (FIFO_SET, NULL,
                      TYPE_STR, uzbl.io->fifo_path,
                      NULL);
    /* TODO: Collect all environment settings into one place. */
    g_setenv ("UZBL_FIFO", uzbl.io->fifo_path, TRUE);
    return TRUE;
}

static void
accept_socket_cb (GObject *source, GAsyncResult *res, gpointer data);

gboolean
attach_socket (const gchar *path)
{
    GError *error = NULL;
    GSocketAddress *addr = g_unix_socket_address_new (path);
    GSocket *sock = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                  G_SOCKET_TYPE_STREAM, 0, &error);

    if (!sock) {
        g_warning ("attach_socket: failed to create socket %s\n", error->message);
        g_error_free (error);
        return FALSE;
    }

    if (!g_socket_bind (sock, addr, FALSE, &error)) {
        g_warning ("attach_socket: could not bind to %s: %s\n", path, error->message);
        g_error_free (error);
        return FALSE;
    }

    if (chmod (path, 0600)) {
        g_warning ("unable to change permissions for %s socket: %s\n", path, strerror (errno));
        g_socket_close (sock, NULL);
        return FALSE;
    }

    if (!g_socket_listen (sock, &error)) {
        g_warning ("attach_socket: could not listen on %s: %s\n", path, error->message);
        g_socket_close (sock, NULL);
        g_error_free (error);
        return FALSE;
    }

    GSocketListener *listener = g_socket_listener_new ();
    if (!g_socket_listener_add_socket (listener, sock, NULL, &error)) {
        g_warning ("unable to add socket to listening set %s\n", path);
        g_socket_close (sock, NULL);
        g_error_free (error);
        return FALSE;
    }

    uzbl.io->socket_path = g_strdup (path);
    uzbl_events_send (SOCKET_SET, NULL,
                      TYPE_STR, uzbl.io->socket_path,
                      NULL);
    /* TODO: Collect all environment settings into one place. */
    g_setenv ("UZBL_SOCKET", uzbl.io->socket_path, TRUE);


    g_socket_listener_accept_async (listener, NULL,
                                    accept_socket_cb, NULL);

    return TRUE;
}

void
read_message_cb (GObject *source,
                 GAsyncResult *res,
                 gpointer user_data)
{
    UZBL_UNUSED (user_data);

    GInputStream *stream = G_INPUT_STREAM (source);
    GError *error = NULL;
    GVariant *message;
    ExtIOMessageType messagetype;

    if (!(message = uzbl_extio_read_message_finish (stream, res, &messagetype, &error))) {
        g_warning ("reading message from extension: %s", error->message);
        g_error_free (error);
        return;
    }

    switch (messagetype) {
    case EXT_HELO:
        {
            gint proto;
            uzbl_extio_get_message_data (EXT_HELO, message, &proto);
            if (proto != EXTIO_PROTOCOL) {
                g_warning ("Extension with incompatible version loaded (expected %d, was %d)", EXTIO_PROTOCOL, proto);
                gtk_main_quit ();
            }
            break;
        }
    case EXT_FOCUS:
        {
            gchar *name;
            uzbl_extio_get_message_data (EXT_FOCUS, message, &name);
            uzbl_events_send (FOCUS_ELEMENT, NULL,
                              TYPE_STR, name,
                              NULL);
            g_free (name);
            break;
        }
    case EXT_BLUR:
        {
            gchar *name;
            uzbl_extio_get_message_data (EXT_BLUR, message, &name);
            uzbl_events_send (BLUR_ELEMENT, NULL,
                              TYPE_STR, name,
                              NULL);
            g_free (name);
            break;
        }
    default:
        {
            gchar *pmsg = g_variant_print (message, TRUE);
            g_debug ("got unrecognised message %s", pmsg);
            g_free (pmsg);
            break;
        }
    }
    g_variant_unref (message);

    uzbl_extio_read_message_async (stream, read_message_cb, NULL);
}

void
send_buffered_event (gpointer event, gpointer data)
{
    UZBL_UNUSED (data);

    const gchar *message = (const gchar *)event;

    send_event_sockets (uzbl.io->connect_sockets, message);
}

void
schedule_io_input (gchar *line, GAsyncReadyCallback callback, gpointer data)
{
    if (!line) {
        return;
    }

    remove_trailing_newline (line);

    if (g_str_has_prefix (line, "REPLY-")) {
        uzbl_requests_set_reply (line);

        g_free (line);
    } else {
        UzblCommandData *cmd_data = g_new (UzblCommandData, 1);
        cmd_data->cmd = line;
        cmd_data->info = NULL;
        cmd_data->argv = NULL;

        GTask *task = g_task_new (NULL, NULL, callback, data);
        g_task_set_task_data (task, cmd_data, free_cmd_req);
        g_async_queue_push (uzbl.io->cmd_q, task);
    }
}

void
write_result_to_stream (GObject      *source,
                        GAsyncResult *res,
                        gpointer      data)
{
    GIOStream *stream = G_IO_STREAM (data);
    GError *err = NULL;
    GString *result = uzbl_io_command_finish (source, res, &err);

    g_string_append_c (result, '\n');
    write_to_stream (stream, result->str);

    g_object_unref (G_OBJECT (stream));
}

void
send_buffered_event_to_socket (gpointer event, gpointer data)
{
    const gchar *message = (const gchar *)event;
    GIOStream *stream = G_IO_STREAM (data);

    write_to_stream (stream, message);
}

void
write_to_stream (GIOStream *stream, const gchar *message)
{
    GError *error = NULL;
    gssize ret;
    GOutputStream *output = g_io_stream_get_output_stream (stream);

    if (!output || g_output_stream_is_closed (output)) {
        return;
    }

    ret = g_output_stream_write (output, message, strlen (message),
                                 NULL, &error);

    if (ret == -1) {
        g_warning ("Error writing: %s", error->message);
        g_clear_error (&error);
    } else {
        if (!g_output_stream_flush (output, NULL, &error)) {
            g_warning ("Error flushing: %s", error->message);
            g_clear_error (&error);
        }
    }
}

void
accept_socket_cb (GObject *source, GAsyncResult *res, gpointer data)
{
    UZBL_UNUSED (data);

    GSocketListener *listener = G_SOCKET_LISTENER (source);
    GError *error = NULL;
    GSocketConnection *con;

    con = g_socket_listener_accept_finish (listener, res, NULL, &error);
    if (!con) {
        g_warning ("Failed to accept client %s", error->message);
        g_error_free (error);
    }

    add_buffered_cmd_source (G_IO_STREAM (con), "Uzbl control socket",
                             control_command_stream, close_client_socket,
                             uzbl.io->client_sockets);
    g_ptr_array_add (uzbl.io->client_sockets, G_IO_STREAM (con));

    g_socket_listener_accept_async (listener, NULL,
                                    accept_socket_cb, NULL);
}
