/*
 * QEMU Guest Agent
 *
 * Copyright IBM Corp. 2011
 *
 * Authors:
 *  Adam Litke        <aglitke@linux.vnet.ibm.com>
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#include <getopt.h>
#include <termios.h>
#include <syslog.h>
#include "qemu_socket.h"
#include "json-streamer.h"
#include "json-parser.h"
#include "qint.h"
#include "qjson.h"
#include "qga/guest-agent-core.h"
#include "module.h"
#include "signal.h"
#include "qerror.h"
#include "error_int.h"
#include "qapi/qmp-core.h"

#define QGA_VIRTIO_PATH_DEFAULT "/dev/virtio-ports/org.qemu.guest_agent.0"
#define QGA_PIDFILE_DEFAULT "/var/run/qemu-ga.pid"
#define QGA_STATEFILE_DEFAULT "/var/run/qemu-ga.state"
#define QGA_BAUDRATE_DEFAULT B38400 /* for isa-serial channels */
#define QGA_TIMEOUT_DEFAULT 30*1000 /* ms */

typedef struct GAPersistantState {
#define GA_FLAG_FROZEN 1
    uint32_t flags;
    uint32_t filehandle_base;
} GAPersistantState;

struct GAState {
    JSONMessageParser parser;
    GMainLoop *main_loop;
    GIOChannel *conn_channel;
    GIOChannel *listen_channel;
    const char *path;
    const char *method;
    bool virtio; /* fastpath to check for virtio to deal with poll() quirks */
    GACommandState *command_state;
    GLogLevelFlags log_level;
    FILE *log_file;
    const char *log_filepath;
    const char *pid_filepath;
    const char *state_filepath;
    int state_file;
    bool logging_enabled;
    GList *blacklist;
    GAPersistantState persistant_state;
};

/* commands that are safe to issue while filesystems are frozen */
static const char *ga_freeze_whitelist[] = {
    "guest-ping",
    "guest-info",
    "guest-sync",
    "guest-file-read",
    "guest-file-seek",
    "guest-fsfreeze-status",
    "guest-fsfreeze-thaw",
    NULL
};

static struct GAState *ga_state;

static void quit_handler(int sig)
{
    /* if we're frozen, don't exit unless we're absolutely forced to,
     * because we'll most likely hang anyway on closing FDs, and the admin
     * almost certainly doesn't actually want us to die in this situation
     */
    if (ga_is_frozen(ga_state)) {
        return;
    }
    g_debug("received signal num %d, quitting", sig);

    if (g_main_loop_is_running(ga_state->main_loop)) {
        g_main_loop_quit(ga_state->main_loop);
    }
}

static void alarm_handler(int sig)
{
}

static void register_signal_handlers(void)
{
    struct sigaction sigact;
    struct sigaction sigact_alarm;
    int ret;

    memset(&sigact, 0, sizeof(struct sigaction));
    sigact.sa_handler = quit_handler;

    ret = sigaction(SIGINT, &sigact, NULL);
    if (ret == -1) {
        g_error("error configuring signal handler: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    ret = sigaction(SIGTERM, &sigact, NULL);
    if (ret == -1) {
        g_error("error configuring signal handler: %s", strerror(errno));
    }
    memset(&sigact_alarm, 0, sizeof(struct sigaction));
    sigact_alarm.sa_handler = alarm_handler;
    ret = sigaction(SIGALRM, &sigact_alarm, NULL);
    if (ret == -1) {
        g_error("error configuring signal handler: %s", strerror(errno));
    }
}

static void usage(const char *cmd)
{
    printf(
"Usage: %s -c <channel_opts>\n"
"QEMU Guest Agent %s\n"
"\n"
"  -m, --method      transport method: one of unix-listen, virtio-serial, or\n"
"                    isa-serial (virtio-serial is the default)\n"
"  -p, --path        device/socket path (%s is the default for virtio-serial)\n"
"  -l, --logfile     set logfile path, logs to stderr by default\n"
"  -f, --pidfile     specify pid file (default is %s)\n"
"  -s, --statefile   specify state file (absolute paths only, default is %s)\n"
"  -v, --verbose     log extra debugging information\n"
"  -V, --version     print version information and exit\n"
"  -d, --daemonize   become a daemon\n"
"  -b, --blacklist   comma-separated list of RPCs to disable (no spaces, \"?\""
"                    to list available RPCs)\n"
"  -h, --help        display this help and exit\n"
"\n"
"Report bugs to <mdroth@linux.vnet.ibm.com>\n"
    , cmd, QGA_VERSION, QGA_VIRTIO_PATH_DEFAULT, QGA_PIDFILE_DEFAULT,
    QGA_STATEFILE_DEFAULT);
}

static void conn_channel_close(GAState *s);

static const char *ga_log_level_str(GLogLevelFlags level)
{
    switch (level & G_LOG_LEVEL_MASK) {
        case G_LOG_LEVEL_ERROR:
            return "error";
        case G_LOG_LEVEL_CRITICAL:
            return "critical";
        case G_LOG_LEVEL_WARNING:
            return "warning";
        case G_LOG_LEVEL_MESSAGE:
            return "message";
        case G_LOG_LEVEL_INFO:
            return "info";
        case G_LOG_LEVEL_DEBUG:
            return "debug";
        default:
            return "user";
    }
}

bool ga_logging_enabled(GAState *s)
{
    return s->logging_enabled;
}

void ga_disable_logging(GAState *s)
{
    s->logging_enabled = false;
}

void ga_enable_logging(GAState *s)
{
    s->logging_enabled = true;
}

static void ga_store_persistant_state(GAState *s)
{
    size_t ret;
    GAPersistantState persistant_state = s->persistant_state;

    if (!s->state_file) {
        return;
    }

    cpu_to_le32s(&persistant_state.flags);
    cpu_to_le32s(&persistant_state.filehandle_base);

    if (lseek(s->state_file, 0, SEEK_SET)) {
        g_warning("failed to rewind state file: %s", strerror(errno));
        return;
    }
    ret = write(s->state_file, &persistant_state, sizeof(persistant_state));
    if (ret == -1) {
        g_warning("failed to write to state file: %s", strerror(errno));
        return;
    }
    fsync(s->state_file);
}

static void ga_load_persistant_state(GAState *s)
{
    size_t ret;
    GAPersistantState persistant_state = {0};

    if (!s->state_file) {
        return;
    }

    if (lseek(s->state_file, 0, SEEK_SET) == -1) {
        g_warning("failed to rewind state file: %s", strerror(errno));
        return;
    }
g_debug("m0");
    /* unless noatime is specified for the filesystem hosting our state file,
     * we can block on a read() due to an attempt to record a new accessed
     * timestamp. fortunately only the first attempt to read() will cause
     * this, so retry till successful.
     */
    do {
g_debug("m1a");
        alarm(1);
        ret = read(s->state_file, &persistant_state, sizeof(persistant_state));
g_debug("m1");
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        g_warning("failed to read state file: %s", strerror(errno));
        return;
    }

    s->persistant_state.flags = le32_to_cpu(persistant_state.flags);
    s->persistant_state.filehandle_base =
        le32_to_cpu(persistant_state.filehandle_base);
}

static void ga_log(const gchar *domain, GLogLevelFlags level,
                   const gchar *msg, gpointer opaque)
{
    GAState *s = opaque;
    GTimeVal time;
    const char *level_str = ga_log_level_str(level);

    if (!ga_logging_enabled(s)) {
        return;
    }

    level &= G_LOG_LEVEL_MASK;
    if (domain && strcmp(domain, "syslog") == 0) {
        syslog(LOG_INFO, "%s: %s", level_str, msg);
    } else if (level & s->log_level) {
        g_get_current_time(&time);
        fprintf(s->log_file,
                "%lu.%lu: %s: %s\n", time.tv_sec, time.tv_usec, level_str, msg);
        fflush(s->log_file);
    }
}

static bool ga_open_pidfile(const char *pidfile)
{
    int pidfd;
    char pidstr[32];

    pidfd = open(pidfile, O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR);
    if (pidfd == -1 || lockf(pidfd, F_TLOCK, 0)) {
        g_critical("Cannot lock pid file, %s", strerror(errno));
        return false;
    }

    if (ftruncate(pidfd, 0) || lseek(pidfd, 0, SEEK_SET)) {
        g_critical("Failed to truncate pid file");
        goto fail;
    }
    sprintf(pidstr, "%d", getpid());
    if (write(pidfd, pidstr, strlen(pidstr)) != strlen(pidstr)) {
        g_critical("Failed to write pid file");
        goto fail;
    }

    return true;

fail:
    unlink(pidfile);
    return false;
}

static void become_daemon(const char *pidfile)
{
    pid_t pid, sid;

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (pidfile) {
        if (!ga_open_pidfile(pidfile)) {
            g_critical("failed to create pidfile");
            exit(EXIT_FAILURE);
        }
    }

    umask(0);
    sid = setsid();
    if (sid < 0) {
        goto fail;
    }
    if ((chdir("/")) < 0) {
        goto fail;
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    return;

fail:
    unlink(pidfile);
    g_critical("failed to daemonize");
    exit(EXIT_FAILURE);
}

static gint ga_strcmp(gconstpointer str1, gconstpointer str2)
{
    return strcmp(str1, str2);
}

/* disable commands that aren't safe for fsfreeze */
static void ga_disable_non_whitelisted(void)
{
    char **list_head, **list;
    bool whitelisted;
    int i;

    list_head = list = qmp_get_command_list();
    while (*list != NULL) {
        whitelisted = false;
        i = 0;
        while (ga_freeze_whitelist[i] != NULL) {
            if (strcmp(*list, ga_freeze_whitelist[i]) == 0) {
                whitelisted = true;
            }
            i++;
        }
        if (!whitelisted) {
            g_debug("disabling command: %s", *list);
            qmp_disable_command(*list);
        }
        g_free(*list);
        list++;
    }
    g_free(list_head);
}

/* [re-]enable all commands, except those explictly blacklisted by user */
static void ga_enable_non_whitelisted(GList *blacklist)
{
    char **list_head, **list;

    list_head = list = qmp_get_command_list();
    while (*list != NULL) {
        if (g_list_find_custom(blacklist, *list, ga_strcmp) == NULL &&
            !qmp_command_is_enabled(*list)) {
            g_debug("enabling command: %s", *list);
            qmp_enable_command(*list);
        }
        g_free(*list);
        list++;
    }
    g_free(list_head);
}

bool ga_is_frozen(GAState *s)
{
    return !!(s->persistant_state.flags & GA_FLAG_FROZEN);
}

void ga_set_frozen(GAState *s)
{
    if (ga_is_frozen(s)) {
        return;
    }
    /* disable all non-whitelisted (for frozen state) commands */
    ga_disable_non_whitelisted();
    g_warning("disabling logging due to filesystem freeze");
    ga_disable_logging(s);
    s->persistant_state.flags |= GA_FLAG_FROZEN;
    ga_store_persistant_state(s);
}

void ga_unset_frozen(GAState *s)
{
    if (!ga_is_frozen(s)) {
        return;
    }
    /* if we delayed creation/opening of pid/log files due to being
     * in a frozen state at start up, do it now
     */
    if (s->log_filepath) {
        s->log_file = fopen(s->log_filepath, "a");
        if (!s->log_file) {
            s->log_file = stderr;
        }
        s->log_filepath = NULL;
    } else if (!s->log_file) {
        s->log_file = stderr;
    }
    ga_enable_logging(s);
    g_warning("logging re-enabled");
    if (s->pid_filepath) {
        if (!ga_open_pidfile(s->pid_filepath)) {
            g_warning("failed to create/open pid file");
        }
        s->pid_filepath = NULL;
    }

    /* enable all disabled, non-blacklisted commands */
    ga_enable_non_whitelisted(s->blacklist);

    s->persistant_state.flags &= ~GA_FLAG_FROZEN;
    ga_store_persistant_state(s);
}

int ga_get_filehandle(GAState *s)
{
    s->persistant_state.filehandle_base++;
    ga_store_persistant_state(s);
    return s->persistant_state.filehandle_base;
}

static int conn_channel_send_buf(GIOChannel *channel, const char *buf,
                                 gsize count)
{
    GError *err = NULL;
    gsize written = 0;
    GIOStatus status;

    while (count) {
        status = g_io_channel_write_chars(channel, buf, count, &written, &err);
        g_debug("sending data, count: %d", (int)count);
        if (err != NULL) {
            g_warning("error sending newline: %s", err->message);
            return err->code;
        }
        if (status == G_IO_STATUS_ERROR || status == G_IO_STATUS_EOF) {
            return -EPIPE;
        }

        if (status == G_IO_STATUS_NORMAL) {
            count -= written;
        }
    }

    return 0;
}

static int conn_channel_send_payload(GIOChannel *channel, QObject *payload)
{
    int ret = 0;
    const char *buf;
    QString *payload_qstr;
    GError *err = NULL;

    g_assert(payload && channel);

    payload_qstr = qobject_to_json(payload);
    if (!payload_qstr) {
        return -EINVAL;
    }

    qstring_append_chr(payload_qstr, '\n');
    buf = qstring_get_str(payload_qstr);
    ret = conn_channel_send_buf(channel, buf, strlen(buf));
    if (ret) {
        goto out_free;
    }

    g_io_channel_flush(channel, &err);
    if (err != NULL) {
        g_warning("error flushing payload: %s", err->message);
        ret = err->code;
        goto out_free;
    }

out_free:
    QDECREF(payload_qstr);
    if (err) {
        g_error_free(err);
    }
    return ret;
}

static void process_command(GAState *s, QDict *req)
{
    QObject *rsp = NULL;
    int ret;

    g_assert(req);
    g_debug("processing command");
    rsp = qmp_dispatch(QOBJECT(req));
    if (rsp) {
        ret = conn_channel_send_payload(s->conn_channel, rsp);
        if (ret) {
            g_warning("error sending payload: %s", strerror(ret));
        }
        qobject_decref(rsp);
    } else {
        g_warning("error getting response");
    }
}

/* handle requests/control events coming in over the channel */
static void process_event(JSONMessageParser *parser, QList *tokens)
{
    GAState *s = container_of(parser, GAState, parser);
    QObject *obj;
    QDict *qdict;
    Error *err = NULL;
    int ret;

    g_assert(s && parser);

    g_debug("process_event: called");
    obj = json_parser_parse_err(tokens, NULL, &err);
    if (err || !obj || qobject_type(obj) != QTYPE_QDICT) {
        qobject_decref(obj);
        qdict = qdict_new();
        if (!err) {
            g_warning("failed to parse event: unknown error");
            error_set(&err, QERR_JSON_PARSING);
        } else {
            g_warning("failed to parse event: %s", error_get_pretty(err));
        }
        qdict_put_obj(qdict, "error", error_get_qobject(err));
        error_free(err);
    } else {
        qdict = qobject_to_qdict(obj);
    }

    g_assert(qdict);

    /* handle host->guest commands */
    if (qdict_haskey(qdict, "execute")) {
        process_command(s, qdict);
    } else {
        if (!qdict_haskey(qdict, "error")) {
            QDECREF(qdict);
            qdict = qdict_new();
            g_warning("unrecognized payload format");
            error_set(&err, QERR_UNSUPPORTED);
            qdict_put_obj(qdict, "error", error_get_qobject(err));
            error_free(err);
        }
        ret = conn_channel_send_payload(s->conn_channel, QOBJECT(qdict));
        if (ret) {
            g_warning("error sending payload: %s", strerror(ret));
        }
    }

    QDECREF(qdict);
}

static gboolean conn_channel_read(GIOChannel *channel, GIOCondition condition,
                                  gpointer data)
{
    GAState *s = data;
    gchar buf[1024];
    gsize count;
    GError *err = NULL;
    memset(buf, 0, 1024);
    GIOStatus status = g_io_channel_read_chars(channel, buf, 1024,
                                               &count, &err);
    if (err != NULL) {
        g_warning("error reading channel: %s", err->message);
        conn_channel_close(s);
        g_error_free(err);
        return false;
    }
    switch (status) {
    case G_IO_STATUS_ERROR:
        g_warning("problem");
        return false;
    case G_IO_STATUS_NORMAL:
        g_debug("read data, count: %d, data: %s", (int)count, buf);
        json_message_parser_feed(&s->parser, (char *)buf, (int)count);
    case G_IO_STATUS_AGAIN:
        /* virtio causes us to spin here when no process is attached to
         * host-side chardev. sleep a bit to mitigate this
         */
        if (s->virtio) {
            usleep(100*1000);
        }
        return true;
    case G_IO_STATUS_EOF:
        g_debug("received EOF");
        conn_channel_close(s);
        if (s->virtio) {
            return true;
        }
        return false;
    default:
        g_warning("unknown channel read status, closing");
        conn_channel_close(s);
        return false;
    }
    return true;
}

static int conn_channel_add(GAState *s, int fd)
{
    GIOChannel *conn_channel;
    GError *err = NULL;

    g_assert(s && !s->conn_channel);
    conn_channel = g_io_channel_unix_new(fd);
    g_assert(conn_channel);
    g_io_channel_set_encoding(conn_channel, NULL, &err);
    if (err != NULL) {
        g_warning("error setting channel encoding to binary");
        g_error_free(err);
        return -1;
    }
    g_io_add_watch(conn_channel, G_IO_IN | G_IO_HUP,
                   conn_channel_read, s);
    s->conn_channel = conn_channel;
    return 0;
}

static gboolean listen_channel_accept(GIOChannel *channel,
                                      GIOCondition condition, gpointer data)
{
    GAState *s = data;
    g_assert(channel != NULL);
    int ret, conn_fd;
    bool accepted = false;
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);

    conn_fd = qemu_accept(g_io_channel_unix_get_fd(s->listen_channel),
                             (struct sockaddr *)&addr, &addrlen);
    if (conn_fd == -1) {
        g_warning("error converting fd to gsocket: %s", strerror(errno));
        goto out;
    }
    fcntl(conn_fd, F_SETFL, O_NONBLOCK);
    ret = conn_channel_add(s, conn_fd);
    if (ret) {
        g_warning("error setting up connection");
        goto out;
    }
    accepted = true;

out:
    /* only accept 1 connection at a time */
    return !accepted;
}

/* start polling for readable events on listen fd, new==true
 * indicates we should use the existing s->listen_channel
 */
static int listen_channel_add(GAState *s, int listen_fd, bool new)
{
    if (new) {
        s->listen_channel = g_io_channel_unix_new(listen_fd);
    }
    g_io_add_watch(s->listen_channel, G_IO_IN,
                   listen_channel_accept, s);
    return 0;
}

/* cleanup state for closed connection/session, start accepting new
 * connections if we're in listening mode
 */
static void conn_channel_close(GAState *s)
{
    if (strcmp(s->method, "unix-listen") == 0) {
        g_io_channel_shutdown(s->conn_channel, true, NULL);
        listen_channel_add(s, 0, false);
    } else if (strcmp(s->method, "virtio-serial") == 0) {
        /* we spin on EOF for virtio-serial, so back off a bit. also,
         * dont close the connection in this case, it'll resume normal
         * operation when another process connects to host chardev
         */
        usleep(100*1000);
        goto out_noclose;
    }
    g_io_channel_unref(s->conn_channel);
    s->conn_channel = NULL;
out_noclose:
    return;
}

static void init_guest_agent(GAState *s)
{
    struct termios tio;
    int ret, fd;

    if (s->method == NULL) {
        /* try virtio-serial as our default */
        s->method = "virtio-serial";
    }

    if (s->path == NULL) {
        if (strcmp(s->method, "virtio-serial") != 0) {
            g_critical("must specify a path for this channel");
            exit(EXIT_FAILURE);
        }
        /* try the default path for the virtio-serial port */
        s->path = QGA_VIRTIO_PATH_DEFAULT;
    }

    if (strcmp(s->method, "virtio-serial") == 0) {
        s->virtio = true;
        fd = qemu_open(s->path, O_RDWR | O_NONBLOCK | O_ASYNC);
        if (fd == -1) {
            g_critical("error opening channel: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        ret = conn_channel_add(s, fd);
        if (ret) {
            g_critical("error adding channel to main loop");
            exit(EXIT_FAILURE);
        }
    } else if (strcmp(s->method, "isa-serial") == 0) {
        fd = qemu_open(s->path, O_RDWR | O_NOCTTY);
        if (fd == -1) {
            g_critical("error opening channel: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        tcgetattr(fd, &tio);
        /* set up serial port for non-canonical, dumb byte streaming */
        tio.c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK | ISTRIP |
                         INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY |
                         IMAXBEL);
        tio.c_oflag = 0;
        tio.c_lflag = 0;
        tio.c_cflag |= QGA_BAUDRATE_DEFAULT;
        /* 1 available byte min or reads will block (we'll set non-blocking
         * elsewhere, else we have to deal with read()=0 instead)
         */
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        /* flush everything waiting for read/xmit, it's garbage at this point */
        tcflush(fd, TCIFLUSH);
        tcsetattr(fd, TCSANOW, &tio);
        ret = conn_channel_add(s, fd);
        if (ret) {
            g_error("error adding channel to main loop");
        }
    } else if (strcmp(s->method, "unix-listen") == 0) {
        fd = unix_listen(s->path, NULL, strlen(s->path));
        if (fd == -1) {
            g_critical("error opening path: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        ret = listen_channel_add(s, fd, true);
        if (ret) {
            g_critical("error binding/listening to specified socket");
            exit(EXIT_FAILURE);
        }
    } else {
        g_critical("unsupported channel method/type: %s", s->method);
        exit(EXIT_FAILURE);
    }

    json_message_parser_init(&s->parser, process_event);
    s->main_loop = g_main_loop_new(NULL, false);
}

int main(int argc, char **argv)
{
    const char *sopt = "hVvdm:p:l:f:b:s:";
    const char *method = NULL, *path = NULL;
    const char *pid_filepath = QGA_PIDFILE_DEFAULT;
    const char *state_filepath = QGA_STATEFILE_DEFAULT;
    const struct option lopt[] = {
        { "help", 0, NULL, 'h' },
        { "version", 0, NULL, 'V' },
        { "logfile", 0, NULL, 'l' },
        { "pidfile", 0, NULL, 'f' },
        { "statefile", 0, NULL, 's' },
        { "verbose", 0, NULL, 'v' },
        { "method", 0, NULL, 'm' },
        { "path", 0, NULL, 'p' },
        { "daemonize", 0, NULL, 'd' },
        { "blacklist", 0, NULL, 'b' },
        { NULL, 0, NULL, 0 }
    };
    int opt_ind = 0, ch, daemonize = 0, i, j, len;
    GLogLevelFlags log_level = G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL;
    char *log_filepath = NULL;
    GAState *s;
    GList *blacklist = NULL;

    module_call_init(MODULE_INIT_QAPI);

    while ((ch = getopt_long(argc, argv, sopt, lopt, &opt_ind)) != -1) {
        switch (ch) {
        case 'm':
            method = optarg;
            break;
        case 'p':
            path = optarg;
            break;
        case 'l':
            log_filepath = optarg;
            break;
        case 'f':
            pid_filepath = optarg;
            break;
        case 's':
            state_filepath = optarg;
            break;
        case 'v':
            /* enable all log levels */
            log_level = G_LOG_LEVEL_MASK;
            break;
        case 'V':
            printf("QEMU Guest Agent %s\n", QGA_VERSION);
            return 0;
        case 'd':
            daemonize = 1;
            break;
        case 'b': {
            char **list_head, **list;
            if (*optarg == '?') {
                list_head = list = qmp_get_command_list();
                while (*list != NULL) {
                    printf("%s\n", *list);
                    g_free(*list);
                    list++;
                }
                g_free(list_head);
                return 0;
            }
            for (j = 0, i = 0, len = strlen(optarg); i < len; i++) {
                if (optarg[i] == ',') {
                    optarg[i] = 0;
                    blacklist = g_list_append(blacklist, &optarg[j]);
                    j = i + 1;
                }
            }
            if (j < i) {
                blacklist = g_list_append(blacklist, &optarg[j]);
            }
            break;
        }
        case 'h':
            usage(argv[0]);
            return 0;
        case '?':
            g_print("Unknown option, try '%s --help' for more information.\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }

    s = g_malloc0(sizeof(GAState));
g_debug("marker 0");
    s->state_file = open(state_filepath, O_CREAT|O_RDONLY, S_IWUSR|S_IRUSR);
g_debug("marker 1");
    if (s->state_file != -1) {
g_debug("marker 2");
        ga_load_persistant_state(s);
        close(s->state_file);
        s->state_file = open(state_filepath, O_CREAT|O_RDWR, S_IWUSR|S_IRUSR);
    }
g_debug("marker 3");

    g_log_set_default_handler(ga_log, s);
    g_log_set_fatal_mask(NULL, G_LOG_LEVEL_ERROR);

    if (ga_is_frozen(s)) {
        if (daemonize) {
            /* delay opening/locking of pidfile till filesystem are unfrozen */
            s->pid_filepath = pid_filepath;
            become_daemon(NULL);
        }
        if (log_filepath) {
            /* delay opening the log file till filesystems are unfrozen */
            s->log_filepath = log_filepath;
        }
        ga_disable_logging(s);
        ga_disable_non_whitelisted();
    } else {
        if (daemonize) {
            become_daemon(pid_filepath);
        }
        if (log_filepath) {
            s->log_file = fopen(log_filepath, "a");
            if (!s->log_file) {
                g_critical("unable to open specified log file: %s",
                           strerror(errno));
                goto fail;
            }
        } else {
            s->log_file = stderr;
        }
        ga_enable_logging(s);
    }

    s->conn_channel = NULL;
    s->path = path;
    s->method = method;
    s->log_level = log_level;
    if (blacklist) {
        s->blacklist = blacklist;
        do {
            g_debug("disabling command: %s", (char *)blacklist->data);
            qmp_disable_command(blacklist->data);
            blacklist = g_list_next(blacklist);
        } while (blacklist);
    }
    s->command_state = ga_command_state_new();
    ga_command_state_init(s, s->command_state);
    ga_command_state_init_all(s->command_state);
    ga_state = s;

    init_guest_agent(ga_state);
    register_signal_handlers();

    g_main_loop_run(ga_state->main_loop);

    ga_command_state_cleanup_all(ga_state->command_state);
    unlink(pid_filepath);

    return 0;
fail:
    unlink(pid_filepath);
    return EXIT_FAILURE;
}
