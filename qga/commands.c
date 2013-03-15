/*
 * QEMU Guest Agent common/cross-platform command implementations
 *
 * Copyright IBM Corp. 2012
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "qga/guest-agent-core.h"
#include "qga/guest-file-command-state.h"
#include "qga-qmp-commands.h"
#include "qapi/qmp/qerror.h"

/* Note: in some situations, like with the fsfreeze, logging may be
 * temporarilly disabled. if it is necessary that a command be able
 * to log for accounting purposes, check ga_logging_enabled() beforehand,
 * and use the QERR_QGA_LOGGING_DISABLED to generate an error
 */
void slog(const gchar *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_logv("syslog", G_LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

int64_t qmp_guest_sync_delimited(int64_t id, Error **errp)
{
    ga_set_response_delimited(ga_state);
    return id;
}

int64_t qmp_guest_sync(int64_t id, Error **errp)
{
    return id;
}

void qmp_guest_ping(Error **err)
{
    slog("guest-ping called");
}

struct GuestAgentInfo *qmp_guest_info(Error **err)
{
    GuestAgentInfo *info = g_malloc0(sizeof(GuestAgentInfo));
    GuestAgentCommandInfo *cmd_info;
    GuestAgentCommandInfoList *cmd_info_list;
    char **cmd_list_head, **cmd_list;

    info->version = g_strdup(QEMU_VERSION);

    cmd_list_head = cmd_list = qmp_get_command_list();
    if (*cmd_list_head == NULL) {
        goto out;
    }

    while (*cmd_list) {
        cmd_info = g_malloc0(sizeof(GuestAgentCommandInfo));
        cmd_info->name = g_strdup(*cmd_list);
        cmd_info->enabled = qmp_command_is_enabled(cmd_info->name);

        cmd_info_list = g_malloc0(sizeof(GuestAgentCommandInfoList));
        cmd_info_list->value = cmd_info;
        cmd_info_list->next = info->supported_commands;
        info->supported_commands = cmd_info_list;

        g_free(*cmd_list);
        cmd_list++;
    }

out:
    g_free(cmd_list_head);
    return info;
}

static struct {
    QTAILQ_HEAD(, GuestExecInfo) processes;
    int64_t next_handle;
} guest_exec_state;

typedef struct GuestExecInfo {
    GPid gpid;
    const char *cmdline;
    int64_t handle; /* this should be UUID */
    gint fd_in;
    gint fd_out;
    gint fd_err;
    QTAILQ_ENTRY(GuestExecInfo) next;
} GuestExecInfo;

static void guest_exec_init(void)
{
    QTAILQ_INIT(&guest_exec_state.processes);
    guest_exec_state.next_handle = 0;
}

static GuestExecInfo *guest_exec_info_new(GPid gpid, const char *cmdline, gint fd_in, gint fd_out,
                                          gint fd_err)
{
    GuestExecInfo *gei;

    gei = g_new(GuestExecInfo, 1);
    gei->gpid = gpid;
    gei->cmdline = cmdline;
    gei->fd_in = fd_in;
    gei->fd_out = fd_out;
    gei->fd_err = fd_err;
    gei->handle = -1;

    return gei;
}

/* TODO: USE UUIDs! */
static int64_t guest_exec_info_register(GuestExecInfo *gei)
{
    gei->handle = guest_exec_state.next_handle++;
    QTAILQ_INSERT_TAIL(&guest_exec_state.processes, gei, next);

    return gei->handle;
}

static GuestExecInfo *guest_exec_info_find(int64_t handle)
{
    GuestExecInfo *gei;

    QTAILQ_FOREACH(gei, &guest_exec_state.processes, next)
    {
        if (gei->handle == handle) {
            return gei;
        }
    }

    return NULL;
}

#ifndef WIN32
GuestExecStatus *qmp_guest_exec_status(int64_t handle, bool has_wait, bool wait,
                                       Error **errp)
{
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    int status;
    int ret;

    gei = guest_exec_info_find(handle);
    if (!gei) {
        error_setg(errp, "process not found for handle %"PRId64, handle);
        return NULL;
    }

    ret = waitpid(gei->gpid, &status, (has_wait && wait) ? 0 : WNOHANG);
    if (ret == -1) {
        error_setg_errno(errp, errno, "waitpid error, pid: %d", gei->gpid);
        return NULL;
    }

    ges = g_new0(GuestExecStatus, 1);
    ges->handle = gei->handle;
    if (WIFEXITED(status)) {
        ges->exited = true;
        ges->exit_code = WEXITSTATUS(status);
    } else {
        ges->exited = false;
        ges->exit_code = 0;
    }

    return ges;
}

#else
GuestExecStatus *qmp_guest_exec_status(int64_t handle, bool has_wait, bool wait,
                                       Error **errp)
{
    DWORD p_exit_code;
    bool ret;
    GuestExecInfo *gei;
    GuestExecStatus *ges;

    gei = guest_exec_info_find(handle);
    if (!gei) {
        error_setg(errp, "process not found for handle %"PRId64, handle);
        return NULL;
    }

    do {
        ret = GetExitCodeProcess(gei->gpid, &p_exit_code);
        if (!ret) {
            error_setg(errp, "failed to obtain process exit code: %d",
                       (int)GetLastError());
            return NULL;
        }

    } while (has_wait && wait && p_exit_code == STILL_ACTIVE);

    ges = g_new(GuestExecStatus, 1);
    /* TODO: expose actual handle? only seems useful on linux */
    ges->handle = gei->handle;
    if (p_exit_code == STILL_ACTIVE) {
        ges->exited = false;
        ges->exit_code = 0;
    } else {
        ges->exited = true;
        ges->exit_code = p_exit_code;
    }

    return ges;
}
#endif /* WIN32 */

/*
gboolean            g_spawn_async_with_pipes            (const gchar *working_directory,
                                                         gchar **argv,
                                                         gchar **envp,
                                                         GSpawnFlags flags,
                                                         GSpawnChildSetupFunc child_setup,
                                                         gpointer user_data,
                                                         GPid *child_pid,
                                                         gint *standard_input,
                                                         gint *standard_output,
                                                         gint *standard_error,
                                                         GError **error);
*/

#define QGA_DEBUG

static GuestExecInfo *guest_exec_spawn(const char *cmdline, bool interactive,
                                       Error **errp)
{
    GSpawnFlags default_flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
    gboolean ret;
    GPid gpid;
    gchar **argv;
    gint argc;
    GError *gerr = NULL;
    gint fd_in = -1, fd_out = -1, fd_err = -1;

    ret = g_shell_parse_argv(cmdline, &argc, &argv, &gerr);
    if (!ret || gerr) {
        error_setg(errp, "failed to parse command: %s, %s", cmdline,
                  gerr->message);
        return NULL;
    }
#ifdef QGA_DEBUG
    int i;
    for (i = 0; i < argc; i++) {
        g_print("argv[%d]: %s\n", i, argv[i]); }
#endif

    ret = g_spawn_async_with_pipes(NULL, argv, NULL,
                                   default_flags, NULL, NULL, &gpid,
                                   interactive ? &fd_in : NULL, &fd_out, &fd_err, &gerr);
    if (gerr) {
        error_setg(errp, "failed to execute command: %s, %s", cmdline,
                  gerr->message);
        return NULL;
    }

#ifdef QGA_DEBUG
#ifndef _WIN32
    g_print("gpid: %d\n", gpid);
#endif
    g_print("return: %d\n", ret);
    /*
    g_print("fd_in: %d, fd_out: %d, fd_err: %d\n",
            *fd_in, *fd_out, *fd_err);
            */
#endif

    return guest_exec_info_new(gpid, cmdline, fd_in, fd_out, fd_err);
}

GuestExecAsyncResponse *qmp_guest_exec_async(const char *cmdline,
                                             bool has_interactive,
                                             bool interactive,
                                             Error **errp)
{
    GuestExecAsyncResponse *ger;
    GuestExecInfo *gei;
    int32_t handle;
    //FILE *fh;
    GIOChannel *chan;

    gei = guest_exec_spawn(cmdline, has_interactive && interactive, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    ger = g_new0(GuestExecAsyncResponse, 1);

    /* TODO: clean this up it looks like crap */
    if (has_interactive && interactive) {
#if 0
        fh = fdopen(gei->fd_in, "a");
        if (!fh) {
            error_setg_errno(errp, errno,
                             "error creating file handle for stdin");
            return NULL;
        }
#endif
        chan = g_io_channel_unix_new(gei->fd_in);
        ger->has_handle_stdin = true;
        ger->handle_stdin = guest_file_handle_add(chan, errp);
        if (error_is_set(errp)) {
            return NULL;
        }
    }

#if 0
    fh = fdopen(gei->fd_out, "r");
    if (!fh) {
        error_setg_errno(errp, errno, "error creating file handle for stdout");
        return NULL;
    }
#endif
    chan = g_io_channel_unix_new(gei->fd_out);
    ger->has_handle_stdout = true;
    ger->handle_stdout = guest_file_handle_add(chan, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

#if 0
    fh = fdopen(gei->fd_out, "r");
    if (!fh) {
        error_setg_errno(errp, errno, "error creating file handle for stderr");
        return NULL;
    }
#endif
    chan = g_io_channel_unix_new(gei->fd_err);
    ger->has_handle_stderr = true;
    ger->handle_stderr = guest_file_handle_add(chan, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    handle = guest_exec_info_register(gei);
    ger->status = qmp_guest_exec_status(handle, false, false, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    return ger;
}

typedef struct GuestExecPipeData {
    GIOChannel *chan; /* TODO: don't really need this */
    GString *buffer;
    gsize buffer_max;
    Error *err;
    bool done;
    int debug_id;
} GuestExecPipeData;

static GuestExecPipeData *guest_exec_pipe_data_new(GIOChannel *chan,
                                                   int debug_id)
{
    GuestExecPipeData *pd = g_new(GuestExecPipeData, 1);

    pd->chan = chan;
    pd->buffer = g_string_new("");
    pd->buffer_max = QGA_EXEC_BUFFER_MAX;
    pd->err = NULL;
    pd->debug_id = debug_id;
    pd->done = false;

    return pd;
}

static gboolean guest_exec_pipe_cb(GIOChannel *chan, GIOCondition condition,
                                   gpointer user_data)
{
    GuestExecPipeData *pd = user_data;
    gchar buf[1024+1];
    gsize count = 0;
    GError *gerr = NULL;
    GIOStatus gstatus;

g_warning("cb called, debug_id: %d", pd->debug_id);
g_warning("marker 0");
    if (pd->done || pd->err) {
        return false;
    }

g_warning("marker 1");
    if (pd->buffer->len >= pd->buffer_max) {
        error_setg(&pd->err,
                   "error reading pipe, %"PRId64" buffer size limit exceeded",
                   pd->buffer_max);
        goto out;
    }

    gstatus = g_io_channel_read_chars(pd->chan, buf,
                                      MIN(pd->buffer_max - pd->buffer->len, 1024),
                                      &count, &gerr);
    if (gerr) {
        error_setg(&pd->err, "error reading pipe: %s", gerr->message);
        g_error_free(gerr);
        goto out;
    }
g_warning("marker 2");

    switch (gstatus) {
g_warning("marker 3");
    case G_IO_STATUS_ERROR:
g_warning("marker 4");
        error_setg(&pd->err, "error reading pipe");
        /* fall through to channel teardown */
    case G_IO_STATUS_EOF:
g_warning("marker 5");
        break;
    case G_IO_STATUS_NORMAL:
g_warning("marker 6");
        g_string_append_len(pd->buffer, buf, count);
        buf[1024] = 0;
        g_print("current buffer:\n%s\n", buf);
        /* fall through to re-poll later */
    case G_IO_STATUS_AGAIN:
g_warning("marker 7");
        return true;
    }

    pd->done = true;

out:
g_warning("exiting handler for debug_id: %d", pd->debug_id);
    pd->done = true;
    //g_main_loop_quit(pd->main_loop);
    return false;
}

static void guest_exec_buffer_output(int32_t handle, gchar **stdout_buf,
                                     gchar **stderr_buf, Error **errp)
{
    GMainContext *ctx;
    GIOChannel *stdout_chan, *stderr_chan;
    GSource *stdout_source, *stderr_source;
    GuestExecPipeData *stdout_pd, *stderr_pd;
    GuestExecInfo *gei = guest_exec_info_find(handle);

    stdout_chan = g_io_channel_unix_new(gei->fd_out);
    stderr_chan = g_io_channel_unix_new(gei->fd_err);
    ctx = g_main_context_new();
    //loop = g_main_loop_new(ctx, false);

    stdout_pd = guest_exec_pipe_data_new(stdout_chan, 100);
    stderr_pd = guest_exec_pipe_data_new(stderr_chan, 101);
    stdout_source = g_io_create_watch(stdout_chan, G_IO_OUT | G_IO_ERR | G_IO_HUP);
    stderr_source = g_io_create_watch(stderr_chan, G_IO_OUT | G_IO_ERR | G_IO_HUP);
    g_source_attach(stdout_source, ctx);
    g_source_attach(stderr_source, ctx);
    g_source_set_callback(stdout_source, (GSourceFunc)guest_exec_pipe_cb, stdout_pd, NULL);
    g_source_set_callback(stderr_source, (GSourceFunc)guest_exec_pipe_cb, stderr_pd, NULL);

    while (!(stdout_pd->done && stderr_pd->done)) {
        g_main_context_iteration(ctx, true);
    }

g_warning("dmarker 0");

    *stdout_buf = stdout_pd->buffer->str;
    *stderr_buf = stderr_pd->buffer->str;

g_warning("dmarker 1");

    /* TODO: cleanup pipe data */
}

#ifdef QGA_DEBUG
static void print_gei(GuestExecInfo *gei)
{
    g_print("gei->gpid: %d\n", gei->gpid);
    g_print("gei->cmdline: %s\n", gei->cmdline);
    g_print("gei->handle: %d\n", (int)gei->handle);
    g_print("gei->fd_in: %d\n", gei->fd_in);
    g_print("gei->fd_out: %d\n", gei->fd_out);
    g_print("gei->fd_err: %d\n", gei->fd_err);
}
#endif

GuestExecResponse *qmp_guest_exec(const char *cmdline, Error **errp)
{
    int64_t handle;
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    GuestExecResponse *ger;
    gchar *stdout_buf = NULL, *stderr_buf = NULL;

    gei = guest_exec_spawn(cmdline, false, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    handle = guest_exec_info_register(gei);
#ifdef QGA_DEBUG
    print_gei(gei);
#endif

    /* TODO: select on fd in/out and buffer out, handle timeout.
     * call blocking exec_status when eof on both handle, or timeout
     * occurs
     */
    guest_exec_buffer_output(handle, &stdout_buf, &stderr_buf, errp);

    ges = qmp_guest_exec_status(handle, true, true, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    g_print("stdout: %s", stdout_buf);
    g_print("stderr: %s", stderr_buf);
    ger = g_new0(GuestExecResponse, 1);
    ger->status = ges;
    /* TODO: base64 encode the response */
    ger->has_stdout_buffer = true;
    ger->stdout_buffer = stdout_buf;
    ger->has_stderr_buffer = true;
    ger->stderr_buffer = stderr_buf;

    return ger;
}

/* register init/cleanup routines for stateful command groups */
void ga_command_state_init_common(GAState *s, GACommandState *cs)
{
    ga_command_state_add(cs, guest_file_init, NULL);
    ga_command_state_add(cs, guest_exec_init, NULL);
}
