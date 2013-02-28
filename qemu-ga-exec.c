#include <glib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "qga/guest-agent-core.h"
#include "qga-qmp-commands.h"
#include "qapi/qmp/qerror.h"
#include "qemu/queue.h"
#include "qemu/host-utils.h"

#if 0
#include <sys/wait.h>
#include <sys/types.h>
GuestExecStatus *qmp_guest_exec_status(int64_t pid, bool has_wait,
                                       bool wait, Error **err)
{
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    int status, ret;
    char **ptr;

    slog("guest-exec-status called");

    gei = guest_exec_info_find(pid);
    if (gei == NULL) {
        error_set(err, QERR_INVALID_PARAMETER, "pid");
        return NULL;
    }

    ret = waitpid(gei->pid, &status, WNOHANG);
    if (ret == -1) {
        error_set(err, QERR_UNDEFINED_ERROR);
        return NULL;
    }

    ges = g_malloc0(sizeof(*ges));
    ges->handle_stdin = gei->gfh_stdin ? gei->gfh_stdin->id : -1;
    ges->handle_stdout = gei->gfh_stdout ? gei->gfh_stdout->id : -1;
    ges->handle_stderr = gei->gfh_stderr ? gei->gfh_stderr->id : -1;
    ges->pid = gei->pid;
    if (ret == 0) {
        ges->exited = false;
    } else {
        ges->exited = true;
        /* reap child info once user has successfully wait()'d */
        QTAILQ_REMOVE(&guest_exec_state.processes, gei, next);
        for (ptr = gei->params; ptr && *ptr != NULL; ptr++) {
            g_free(*ptr);
        }
        g_free(gei->params);
        g_free(gei);
    }
    return ges;
}

static char **extract_param_array(const char *path,
                                  const GuestExecParamList *entry)
{
    const GuestExecParamList *head;
    GuestExecParam *param_container;
    int count = 2; /* reserve 2 for path and NULL terminator */
    int i = 0;
    char **param_array;

    for (head = entry; head != NULL; head = head->next) {
        param_container = head->value;
        printf("value: %s\n", param_container->param);
        count++;
    }

    param_array = g_malloc0((count) * sizeof(*param_array));
    param_array[i++] = strdup(path);

    for (head = entry; head != NULL; head = head->next) {
        param_container = head->value;
        /* NULL-terminated list, so if they passed us a NULL param dup
         * in an emptry string to avoid client-induced memory leak
         */
        if (param_container->param) {
            param_array[i] = strdup(param_container->param);
        } else {
            param_array[i] = strdup("");
        }
        i++;
    }

    return param_array;
}

GuestExecStatus *qmp_guest_exec(const char *path,
                                bool has_params,
                                GuestExecParamList *params,
                                bool has_detach, bool detach,
                                Error **err)
{
    int ret, fd;
    Error *local_err = NULL;
    GuestExecStatus *ges;
    char **param_array;

    slog("guest-exec called");

    if (!has_detach) {
        detach = false;
    }

    param_array = extract_param_array(path, has_params ? params : NULL);

    ret = fork();
    if (ret < 0) {
        error_set(err, QERR_UNDEFINED_ERROR);
        return NULL;
    } else if (ret == 0) {
        slog("guest-exec child spawned");
        /* exec the command */
        setsid();
        if (has_handle_stdin) {
            if (gfh_stdin->is_pipe) {
                fclose(gfh_stdin->stream.pipe.in); /* parent writes to this */
                fd = fileno(gfh_stdin->stream.pipe.out);
            } else {
                fd = fileno(gfh_stdin->stream.fh);
            }
            dup2(fd, STDIN_FILENO);
            /* processes don't seem to like O_NONBLOCK std in/out/err */
            toggle_flags(fd, O_NONBLOCK, false, err);
            if (error_is_set(err)) {
                return NULL;
            }
        } else {
            fclose(stdin);
        }
        if (has_handle_stdout) {
            if (gfh_stdout->is_pipe) {
                fclose(gfh_stdout->stream.pipe.out); /* parent reads this end */
                fd = fileno(gfh_stdout->stream.pipe.in);
            } else {
                fd = fileno(gfh_stdout->stream.fh);
            }
            dup2(fd, STDOUT_FILENO);
            toggle_flags(fd, O_NONBLOCK, false, err);
            if (error_is_set(err)) {
                return NULL;
            }
        } else {
            fclose(stdout);
        }
        if (has_handle_stderr) {
            if (gfh_stderr->is_pipe) {
                fclose(gfh_stderr->stream.pipe.out); /* parent reads this end */
                fd = fileno(gfh_stderr->stream.pipe.in);
            } else {
                fd = fileno(gfh_stderr->stream.fh);
            }
            dup2(fd, STDERR_FILENO);
            toggle_flags(fd, O_NONBLOCK, false, err);
            if (error_is_set(err)) {
                return NULL;
            }
        } else {
            fclose(stderr);
        }

        ret = execvp(path, (char * const*)param_array);
        if (ret) {
            slog("guest-exec child failed: %s", strerror(errno));
        }
        exit(!!ret);
    }

    guest_exec_info_add(ret, param_array, gfh_stdin, gfh_stdout, gfh_stderr,
                        &local_err);
    if (local_err) {
        error_propagate(err, local_err);
        return NULL;
    }

    /* return initial status, or wait for completion if !detach */
    if (!detach) {
        ges = qmp_guest_exec_status(ret, true, true, err);
    } else {
        ges = qmp_guest_exec_status(ret, true, false, err);
    }
    return ges;
}

#endif

static struct {
    QTAILQ_HEAD(, GuestExecInfo) processes;
} guest_exec_state;

typedef struct GuestExecInfo {
    GPid gpid;
    char *cmdline;
    QTAILQ_ENTRY(GuestExecInfo) next;
} GuestExecInfo;

static void guest_exec_info_add(GPid gpid)
{
    GuestExecInfo *gei;

    gei = g_new(GuestExecInfo, 1);
    gei->gpid = gpid;
    QTAILQ_INSERT_TAIL(&guest_exec_state.processes, gei, next);
}

static GuestExecInfo *guest_exec_info_find(GPid gpid)
{
    GuestExecInfo *gei;

    QTAILQ_FOREACH(gei, &guest_exec_state.processes, next)
    {
        if (gei->gpid == gpid) {
            return gei;
        }
    }

    return NULL;
}


#ifndef WIN32
GuestExecStatus *qmp_guest_exec_status(int64_t pid, bool has_wait, bool wait,
                                       Error **errp)
{
    GPid gpid = pid;
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    int status;
    int ret;

    gei = guest_exec_info_find(gpid);
    if (!gei) {
        error_setg(errp, "process not found");
        return NULL;
    }

    ret = waitpid(gei->gpid, &status, (has_wait && wait) ? 0 : WNOHANG);
    if (ret == -1) {
        error_setg_errno(errp, errno, "waitpid error, pid: %d", gei->gpid);
        return NULL;
    }

    ges = g_new0(GuestExecStatus, 1);
    ges->pid = gei->gpid;
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
GuestExecStatus *qmp_guest_exec_status(int64_t pid, bool has_wait, bool wait,
                                       Error **errp)
{
    GPid gpid = pid;
    HANDLE p_handle = gpid;
    DWORD p_exit_code;
    bool ret;
    GuestExecStatus *ges;

    do {
        ret = GetExitCodeProcess(phandle, &p_exit_code);
        if (!ret) {
            error_setg(errp, "failed to obtain process exit code: %d",
                       GetLastError());
            return NULL;
        }

    } while (has_wait && wait && p_exit_code == STILL_ACTIVE);

    ges = g_new(GuestExecStatus, 1);
    ges->gpid = pid;
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

void guest_exec_init(void)
{
    QTAILQ_INIT(&guest_exec_state.processes);
}

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

static GPid guest_exec_async(const char *cmdline,
                             gint *fd_in, gint *fd_out, gint *fd_err,
                             Error **errp)
{

    //GSpawnFlags default_flags = G_SPAWN_STDOUT_TO_DEV_NULL;
    GSpawnFlags default_flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
    gboolean ret;
    GPid gpid = -1;
    gchar **argv;
    gint argc;
    GError *gerr = NULL;

    ret = g_shell_parse_argv(cmdline, &argc, &argv, &gerr);
    if (!ret || gerr) {
        error_setg(errp, "failed to parse command: %s, %s", cmdline,
                  gerr->message);
        return -1;
    }
#ifdef QGA_DEBUG
    int i;
    for (i = 0; i < argc; i++) {
        g_print("argv[%d]: %s\n", i, argv[i]);
    }
#endif

    ret = g_spawn_async_with_pipes(NULL, argv, NULL,
                                   default_flags, NULL, NULL, &gpid,
                                   fd_in, fd_out, fd_err, &gerr);
    if (gerr) {
        error_setg(errp, "failed to execute command: %s, %s", cmdline,
                  gerr->message);
        return -1;
    }

#ifdef QGA_DEBUG
    g_print("gpid: %d, return: %d\n", gpid, ret);
    /*
    g_print("fd_in: %d, fd_out: %d, fd_err: %d\n",
            *fd_in, *fd_out, *fd_err);
            */
#endif

    return gpid;
}

GuestExecStatus *qmp_guest_exec_async(const char *cmdline,
                                      bool has_stdin, int64_t stdin_gfh,
                                      bool has_stdout, int64_t stdout_gfh,
                                      bool has_stderr, int64_t stderr_gfh,
                                      Error **errp)
{   
    GPid gpid;
    GuestExecStatus *ges;

    gpid = guest_exec_async(cmdline, NULL, NULL, NULL, errp);

    guest_exec_info_add(gpid);

    /* TODO: don't block, add pipes to state tracker */
    ges = qmp_guest_exec_status(gpid, true, true, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    return ges;
}

GuestExecStatus *qmp_guest_exec(const char *cmdline, Error **errp)
{
    int fd_out, fd_err;
    GPid gpid;
    gboolean ret;
    GError *gerr = NULL;
    GuestExecStatus *ges;
    gchar *buf_stdout, *buf_stderr;
    gint status;

    gpid = guest_exec_async(cmdline, NULL, &fd_out, &fd_err, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    ret = g_spawn_command_line_sync(cmdline, &buf_stdout, &buf_stderr,
                                    &status, &gerr);
    if (!ret) {
        error_setg(errp, "error executing command: %s", gerr->message);
        return NULL;
    }

    ges = g_new(GuestExecStatus, 1);
    ges->pid = gpid; //don't return this, use a handle
    ges->exited = WIFEXITED(status);
    ges->exit_code = WEXITSTATUS(status);

    g_print("stdout: %s", buf_stdout);
    g_print("stderr: %s", buf_stderr);

    return ges;
}

static void print_gei(const GuestExecStatus *ges)
{
    g_print("ges->pid: %lu\n", ges->pid);
    g_print("ges->exited: %d\n", ges->exited);
    g_print("ges->exit_code: %lu\n", ges->exit_code);
}

int main(int argc, char **argv)
{
    const char *cmdline = argv[1];
    Error *err = NULL;
    GuestExecStatus *ges;

    guest_exec_init();
    ges = qmp_guest_exec(cmdline, &err);
    if (err) {
        g_error("error: %s", error_get_pretty(err));
    }
    print_gei(ges);

    ges = qmp_guest_exec(cmdline, &err);
    if (err) {
        g_error("error: %s", error_get_pretty(err));
    }
    print_gei(ges);

    return 0;
}
