/*
 * Tests for various QEMU event handlers
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 */

#include <glib.h>
#include <time.h>
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/sockets.h"

#define WAIT_TIMEOUT 5000 /* ms */

/* wait until all currently queued events have been cleared,
 * abort if timeout is exceeded
 *
 * NOTE: we currently don't have a way to reset the main loop to
 * a "pristine" state, so the best we can manage is iterating it
 * until there's no more events left. If for whatever reason
 * an "always-on" event is added to the main loop, this
 * assumption will fail and generate an abort after a certain
 * period. generally this should only happen by way of
 * constructor functions, or a call made specifically by the
 * unit test that results in such events being added.
 */
static void clear_events(int timeout_ms)
{
    int ret;
    int duration_ms = 0;

    do {
        g_assert_cmpint(duration_ms, <, timeout_ms);
        ret = main_loop_wait(true);
        usleep(10*1000);
        duration_ms += 10;
    } while (ret > 0);
}

typedef struct MainLoop {
    QemuThread thread;
    QemuCond joined_cond;
    QemuMutex mutex;
    bool run;
    bool blocking;
    bool joined;
    bool global_mutex;
} MainLoop;

static MainLoop *qemu_main_loop;

static void main_loop_lock(MainLoop *l)
{
    /* BQL via libqemuutil is a noop, so we synchronize around our
     * own mutex instead. Functionally the only difference is that
     * we'll hold the mutex while polling, which may obscure some
     * timing issues but should otherwise be similar to normal
     * main loop usage within QEMU
     */
    qemu_mutex_lock(&l->mutex);
}

static void main_loop_unlock(MainLoop *l)
{
    qemu_mutex_unlock(&l->mutex);
}

void qemu_mutex_lock_iothread(void)
{
    if (qemu_main_loop) {
        main_loop_lock(qemu_main_loop);
    }
}

void qemu_mutex_unlock_iothread(void)
{
    if (qemu_main_loop) {
        main_loop_unlock(qemu_main_loop);
    }
}

static void *main_loop_threadfn(void *opaque)
{
    MainLoop *l = opaque;

    while (true) {
        main_loop_lock(l);
        if (!l->run) {
            main_loop_unlock(l);
            break;
        }
        main_loop_wait(l->blocking);
        main_loop_unlock(l);
    }

    return NULL;
}

static void main_loop_start(MainLoop *l)
{
    main_loop_lock(l);
    if (!l->run) {
        while (!l->joined) {
            qemu_cond_wait(&l->joined_cond, &l->mutex);
        }
        l->run = true;
        l->joined = false;
        qemu_thread_create(&l->thread, main_loop_threadfn, l,
                           QEMU_THREAD_JOINABLE);
    }
    main_loop_unlock(l);
}

static void main_loop_stop(MainLoop *l)
{
    main_loop_lock(l);
    if (l->joined) {
        main_loop_unlock(l);
        return;
    }
    l->run = false;
    qemu_notify_event();
    main_loop_unlock(l);

    qemu_thread_join(&l->thread);

    main_loop_lock(l);
    l->joined = true;
    qemu_cond_signal(&l->joined_cond);
    main_loop_unlock(l);
}

static MainLoop *main_loop_new(bool blocking, bool global_mutex)
{
    MainLoop *l = g_new0(MainLoop, 1);

    l->run = false;
    l->blocking = false;
    l->joined = true;
    l->global_mutex = global_mutex;
    qemu_cond_init(&l->joined_cond);
    qemu_mutex_init(&l->mutex);
    clear_events(WAIT_TIMEOUT);
    if (global_mutex) {
        qemu_main_loop = l;
    }

    return l;
}

static void main_loop_cleanup(MainLoop *l)
{
    if (l->global_mutex) {
        qemu_main_loop = NULL;
    }
    qemu_cond_destroy(&l->joined_cond);
    qemu_mutex_destroy(&l->mutex);
    g_free(l);
}

/* tests for fd handlers */

static int qemu_set_fd_handler2_locked(int fd,
                                       IOCanReadHandler *fd_read_poll,
                                       IOHandler *fd_read,
                                       IOHandler *fd_write,
                                       void *opaque)
{
    int ret;
    qemu_mutex_lock_iothread();
    ret = qemu_set_fd_handler2(fd, fd_read_poll, fd_read, fd_write, opaque);
    qemu_mutex_unlock_iothread();
    return ret;
}

static void test_idle_loop(void)
{
    int ret;

    clear_events(WAIT_TIMEOUT);
    ret = main_loop_wait(true);
    g_assert_cmpint(ret, ==, 0);
}

static void test_busy_loop(void)
{
    int ret;

    clear_events(WAIT_TIMEOUT);
    qemu_notify_event();
    ret = main_loop_wait(true);
    g_assert_cmpint(ret, ==, 1);
}

#ifndef _WIN32

typedef enum PipeHandlerState {
    PIPE_HANDLER_STATE_INIT,
    PIPE_HANDLER_STATE_READY,
    PIPE_HANDLER_STATE_READING,
    PIPE_HANDLER_STATE_WRITING,
    PIPE_HANDLER_STATE_DONE
} PipeHandlerState;

typedef struct PipeHandlerData {
    int fds[2];
#define PIPE_HANDLER_CHUNK 256
#define PIPE_HANDLER_BUF_LEN (512*1024)
    char buf_in[PIPE_HANDLER_BUF_LEN];
    char buf_out[PIPE_HANDLER_BUF_LEN];
    size_t bytes_read;
    size_t bytes_written;
    PipeHandlerState read_state;
    PipeHandlerState write_state;
} PipeHandlerData;

static PipeHandlerData *pipe_handler_data_new(bool blocking)
{
    int i, ret;
    PipeHandlerData *phd = g_new0(PipeHandlerData, 1);

    ret = pipe2(phd->fds, blocking ? 0 : O_NONBLOCK);
    g_assert_cmpint(ret, ==, 0);

    srand(time(NULL));

    for (i = 0; i < PIPE_HANDLER_BUF_LEN; i++) {
        phd->buf_out[i] = rand();
    }

    phd->read_state = PIPE_HANDLER_STATE_INIT;
    phd->write_state = PIPE_HANDLER_STATE_INIT;

    return phd;
}

static void pipe_handler_data_cleanup(PipeHandlerData *phd)
{
    if (phd->read_state != PIPE_HANDLER_STATE_DONE) {
        close(phd->fds[0]);
    }
    if (phd->write_state != PIPE_HANDLER_STATE_DONE) {
        close(phd->fds[1]);
    }
    g_free(phd);
}

static void pipe_write(void *opaque)
{
    PipeHandlerData *phd = opaque;
    int ret;
    size_t bytes_pending =
        MIN(PIPE_HANDLER_CHUNK, PIPE_HANDLER_BUF_LEN - phd->bytes_written);

    if (phd->write_state != PIPE_HANDLER_STATE_WRITING) {
        phd->write_state = PIPE_HANDLER_STATE_WRITING;
    }

    do {
        ret = write(phd->fds[1], &phd->buf_out[phd->bytes_written],
                    bytes_pending);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        g_assert(errno == EAGAIN);
    } else {
        phd->bytes_written += ret;
    }

    if (phd->bytes_written == PIPE_HANDLER_BUF_LEN) {
        qemu_set_fd_handler(phd->fds[1], NULL, NULL, NULL);
        close(phd->fds[1]);
        phd->write_state = PIPE_HANDLER_STATE_DONE;
    }
}

static void pipe_read(void *opaque)
{
    PipeHandlerData *phd = opaque;
    int ret;
    size_t bytes_pending =
        MIN(PIPE_HANDLER_CHUNK, PIPE_HANDLER_BUF_LEN - phd->bytes_read);

    if (phd->read_state != PIPE_HANDLER_STATE_READING) {
        phd->read_state = PIPE_HANDLER_STATE_READING;
    }

    do {
        ret = read(phd->fds[0], &phd->buf_in[phd->bytes_read], bytes_pending);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        g_assert(errno == EAGAIN);
    } else {
        phd->bytes_read += ret;
    }
   
    if (ret == 0 || phd->bytes_read == PIPE_HANDLER_BUF_LEN) {
        qemu_set_fd_handler(phd->fds[0], NULL, NULL, NULL);
        close(phd->fds[0]);
        phd->read_state = PIPE_HANDLER_STATE_DONE;
    }
}

static int pipe_can_read(void *opaque)
{
    PipeHandlerData *phd = opaque;
    int bytes_pending =
        MIN(PIPE_HANDLER_CHUNK, PIPE_HANDLER_BUF_LEN - phd->bytes_read);

    return phd->read_state >= PIPE_HANDLER_STATE_READY ? bytes_pending : 0;
}

static void pipe_handler_wait_for_read_state(PipeHandlerData *phd,
                                             PipeHandlerState state,
                                             int timeout_ms)
{
    while (phd->read_state < state) {
        usleep(1000);
        timeout_ms--;
        g_assert(timeout_ms > 0);
    }
}

static void pipe_handler_wait_for_write_state(PipeHandlerData *phd,
                                              PipeHandlerState state,
                                              int timeout_ms)
{
    while (phd->write_state < state) {
        usleep(1000);
        timeout_ms--;
        g_assert(timeout_ms > 0);
    }
}

static void pipe_handler_data_check(PipeHandlerData *phd)
{
    int i;

    g_assert_cmpint(phd->bytes_written, ==, PIPE_HANDLER_BUF_LEN);
    g_assert_cmpint(phd->bytes_written, ==, phd->bytes_read);

    for (i = 0; i < PIPE_HANDLER_BUF_LEN; i++) {
        g_assert_cmpint(phd->buf_in[i], ==, phd->buf_out[i]);
    }
}

static void test_pipe_helper(bool blocking_fd, bool blocking_main_loop)
{
    PipeHandlerData *phd = pipe_handler_data_new(blocking_fd);
    MainLoop *ml = main_loop_new(blocking_main_loop, true);

    main_loop_start(ml);

    qemu_set_fd_handler2_locked(phd->fds[0], pipe_can_read, pipe_read, NULL, phd);
    qemu_set_fd_handler2_locked(phd->fds[1], NULL, NULL, pipe_write, phd);

    usleep(100 * 1000);
    g_assert(phd->read_state == PIPE_HANDLER_STATE_INIT);
    phd->read_state = PIPE_HANDLER_STATE_READY;
    qemu_notify_event();

    pipe_handler_wait_for_write_state(phd, PIPE_HANDLER_STATE_WRITING, WAIT_TIMEOUT);
    pipe_handler_wait_for_read_state(phd, PIPE_HANDLER_STATE_READING, WAIT_TIMEOUT);
    pipe_handler_wait_for_write_state(phd, PIPE_HANDLER_STATE_DONE, WAIT_TIMEOUT);
    pipe_handler_wait_for_read_state(phd, PIPE_HANDLER_STATE_DONE, WAIT_TIMEOUT);

    main_loop_stop(ml);
    main_loop_cleanup(ml);
    pipe_handler_data_check(phd);
    pipe_handler_data_cleanup(phd);
}

static void test_pipe_blocking_main_loop_blocking(void)
{
    test_pipe_helper(true, true);
}

static void test_pipe_blocking_main_loop_nonblocking(void)
{
    test_pipe_helper(true, false);
}

static void test_pipe_nonblocking_main_loop_blocking(void)
{
    test_pipe_helper(false, true);
}

static void test_pipe_nonblocking_main_loop_nonblocking(void)
{
    test_pipe_helper(false, false);
}

#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#define SERVER_ADDR "127.0.0.1"
#define SERVER_PORT_START "7777"
#define SERVER_PORT_END "9001"
#define SERVER_STR SERVER_ADDR":"SERVER_PORT_START",to="SERVER_PORT_END""

typedef enum ServerState {
    SOCKET_STATEINIT = 0,
    SOCKET_STATELISTENING,
    SOCKET_STATECONNECTED,
    SOCKET_STATEPAUSED,
    SOCKET_STATECONTINUE,
    SOCKET_STATEREADING,
    SOCKET_STATEREADING_COMPLETE,
    SOCKET_STATEWRITING,
    SOCKET_STATEWRITING_COMPLETE,
    SOCKET_STATECLOSED
} ServerState;

static void randomize_data_buffer(uint8_t *buf, size_t size)
{
    int i;

    srand(time(NULL));

    for (i = 0; i < size; i++) {
        buf[i] = rand();
    }
}

static bool check_data_buffers(uint8_t *buf_out, size_t bytes_written,
                               uint8_t *buf_in, size_t bytes_read)
{
    int i;

    if (bytes_written != bytes_read) {
        return false;
    }

    for (i = 0; i < bytes_written; i++) {
        if (buf_out[i] != buf_in[i]) {
            return false;
        }
    }

    return true;
}

typedef enum SocketStateOffset {
    /* states that are temporary in the sense that they can be left without
     * "manual" intervention from controlling code
     */
    SOCKET_STATE_OFFSET_LISTENING = 0,
    SOCKET_STATE_OFFSET_CONNECTING,
    SOCKET_STATE_OFFSET_WRITING,
    SOCKET_STATE_OFFSET_READING,
    SOCKET_STATE_OFFSET_TEMPORARY,

    SOCKET_STATE_OFFSET_INIT,
    SOCKET_STATE_OFFSET_CONNECTED,
    SOCKET_STATE_OFFSET_WRITING_COMPLETE,
    SOCKET_STATE_OFFSET_PAUSED,
    SOCKET_STATE_OFFSET_READING_COMPLETE,
    SOCKET_STATE_OFFSET_CLOSED,
} SocketStateOffset;

typedef enum SocketState {
    SOCKET_STATE_INIT               = 1 << SOCKET_STATE_OFFSET_INIT,
    SOCKET_STATE_LISTENING          = 1 << SOCKET_STATE_OFFSET_LISTENING,
    SOCKET_STATE_CONNECTING         = 1 << SOCKET_STATE_OFFSET_CONNECTING,
    SOCKET_STATE_CONNECTED          = 1 << SOCKET_STATE_OFFSET_CONNECTED,
    SOCKET_STATE_WRITING            = 1 << SOCKET_STATE_OFFSET_WRITING,
    SOCKET_STATE_WRITING_COMPLETE   = 1 << SOCKET_STATE_OFFSET_WRITING_COMPLETE,
    SOCKET_STATE_READING_PAUSED     = 1 << SOCKET_STATE_OFFSET_PAUSED,
    SOCKET_STATE_READING            = 1 << SOCKET_STATE_OFFSET_READING,
    SOCKET_STATE_READING_COMPLETE   = 1 << SOCKET_STATE_OFFSET_READING_COMPLETE,
    SOCKET_STATE_CLOSED             = 1 << SOCKET_STATE_OFFSET_CLOSED,
    SOCKET_STATE_TEMPORARY          = 1 << SOCKET_STATE_OFFSET_TEMPORARY
} SocketState;

typedef enum SocketType {
    SOCKET_TYPE_CLIENT,
    SOCKET_TYPE_SERVER,
} SocketType;

typedef enum SocketCap {
    SOCKET_CAP_READ = 1,
    SOCKET_CAP_WRITE = 2,
} SocketCap;

typedef struct SocketData {
    SocketType type;
    uint32_t state;
    uint32_t tests;
    uint32_t caps;
    int fd;
#define SOCKET_DATA_READ_CHUNK 256
#define SOCKET_DATA_WRITE_CHUNK 256
#define SOCKET_DATA_BUF_LEN (512*1024)
    uint8_t buf_in[SOCKET_DATA_BUF_LEN];
    uint8_t buf_out[SOCKET_DATA_BUF_LEN];
    size_t bytes_written;
    size_t bytes_read;
} SocketData;

static bool socket_data_set_state(SocketData *d, uint32_t state, bool set)
{
    /* TODO: add some state checking here */
    if (set) {
        d->state |= state;
    } else {
        d->state &= ~(state);
    }

    return true;
}

static bool socket_data_set_caps(SocketData *d, uint32_t caps, bool set)
{
    /* TODO: add some state checking here */
    if (set) {
        d->caps |= caps;
    } else {
        d->caps &= ~(caps);
    }

    return true;
}

static bool socket_data_wait_for_state_timeout(SocketData *d,
                                               uint32_t state,
                                               int timeout_ms)
{
    /* waiting for a temporary state is racey and not allowed */
    g_assert(state > SOCKET_STATE_TEMPORARY);

    while ((d->state & state) == false) {
        usleep(1000);
        timeout_ms--;
        if (timeout_ms <= 0) {
            return false;
        }
    }

    return true;
}

static bool socket_data_wait_for_state(SocketData *d,
                                       SocketState state)
{
    return socket_data_wait_for_state_timeout(d, state, WAIT_TIMEOUT);
}

static void socket_data_init(SocketData *d, SocketType type)
{
    d->fd = -1;
    d->type = type;
    randomize_data_buffer(d->buf_out, SOCKET_DATA_BUF_LEN);
    socket_data_set_state(d, SOCKET_STATE_INIT, true);
}

static void socket_data_cleanup(SocketData *d)
{
    if (!(d->state & SOCKET_STATE_CLOSED)) {
        close(d->fd);
    }
}

typedef struct ServerData {
    SocketData d;
    int listen_fd;
    MainLoop *ml;
    char *addr;
    char *port;
} ServerData;

static ServerData *server_data_new(void)
{
    ServerData *sd = g_new0(ServerData, 1);
    char oaddr[256];
    char *saveptr;
    Error *err = NULL;

    socket_data_init(&sd->d, SOCKET_TYPE_SERVER);

    sd->listen_fd = inet_listen(SERVER_STR, oaddr, 256, SOCK_STREAM, 0, &err);
    if (sd->listen_fd == -1 || err) {
        g_free(sd);
        if (err) {
            g_warning("server data error: %s\n", error_get_pretty(err));
            error_free(err);
        }
        return NULL;
    }
    sd->addr = g_strdup(strtok_r(oaddr, ":", &saveptr));
    sd->port = g_strdup(strtok_r(NULL, ",", &saveptr));

    g_assert(socket_data_set_state(&sd->d, SOCKET_STATE_LISTENING, true));

    return sd;
}

static void server_data_cleanup(ServerData *sd)
{
    g_free(sd->addr);
    g_free(sd->port);
    if (sd->listen_fd != -1) {
        close(sd->listen_fd);
    }
    socket_data_cleanup(&sd->d);
    g_free(sd);
}

typedef struct ClientData {
    SocketData d;
} ClientData;

static ClientData *client_data_new(const char *addr, const char *port)
{
    ClientData *cd = g_new0(ClientData, 1);
    SocketData *d = &cd->d;
    struct addrinfo ai, *res, *e;
    int on = 1;
    int rc;

    socket_data_init(d, SOCKET_TYPE_CLIENT);

    memset(&ai, 0, sizeof(ai));
    ai.ai_flags = AI_CANONNAME | AI_ADDRCONFIG;
    ai.ai_family = PF_UNSPEC;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_family = PF_INET;

    rc = getaddrinfo(addr, port, &ai, &res);
    if (rc != 0) {
        return NULL;
    }

    for (e = res; e != NULL; e = e->ai_next) {
        d->fd = qemu_socket(e->ai_family, e->ai_socktype, e->ai_protocol);
        if (d->fd == -1) {
            continue;
        }
        qemu_setsockopt(d->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        qemu_set_nonblock(d->fd);
        do {
            rc = 0;
            if (connect(d->fd, e->ai_addr, e->ai_addrlen) == -1) {
                rc = -socket_error();
            }
        } while (rc == -EINTR);
        if (QEMU_SOCKET_RC_INPROGRESS(rc)) {
            socket_data_set_state(d, SOCKET_STATE_CONNECTING, true);
            break;
        } else if (!rc) {
            socket_data_set_state(d, SOCKET_STATE_CONNECTED, true);
            break;
        }
    }

    freeaddrinfo(res);
    if (!(d->state & (SOCKET_STATE_CONNECTED|SOCKET_STATE_CONNECTING))) {
        socket_data_cleanup(d);
        g_free(cd);
        return NULL;
    }

    return cd;
}

static void client_data_cleanup(ClientData *cd)
{
    socket_data_cleanup(&cd->d);
    g_free(cd);
}

/* socket fd callbacks */

static void server_listen(void *opaque)
{
    SocketData *d = opaque;
    ServerData *sd = DO_UPCAST(ServerData, d, d);
    int rc;

    g_assert(d->state & SOCKET_STATE_LISTENING);
    do {
        rc = qemu_accept(sd->listen_fd, NULL, NULL);
    } while (rc == -1 && socket_error() == EINTR);
    if (rc == -1) {
        return;
    }
    sd->d.fd = rc;
    socket_data_set_state(d, SOCKET_STATE_CONNECTED, true);
    qemu_set_fd_handler2(sd->listen_fd, NULL, NULL, NULL, NULL);
    socket_data_set_state(d, SOCKET_STATE_LISTENING, false);
}

static void client_connect(void *opaque)
{
    SocketData *d = opaque;
    ClientData *cd = DO_UPCAST(ClientData, d, d);

    g_assert(d->state & SOCKET_STATE_CONNECTING);
    g_assert(socket_data_set_state(d, SOCKET_STATE_CONNECTED, true));
    qemu_set_fd_handler2(cd->d.fd, NULL, NULL, NULL, NULL);
    g_assert(socket_data_set_state(d, SOCKET_STATE_CONNECTING, false));
}

static int socket_can_read(void *opaque)
{
    SocketData *d = opaque;

    return (d->state & SOCKET_STATE_READING_PAUSED) ? 0
        : SOCKET_DATA_BUF_LEN - d->bytes_read; 
}

static void socket_write(void *opaque);

static void socket_read(void *opaque)
{
    SocketData *d = opaque;
    size_t bytes_pending = MIN(SOCKET_DATA_READ_CHUNK,
                               SOCKET_DATA_BUF_LEN - d->bytes_read);

    int ret;

    g_assert(d->caps & SOCKET_CAP_READ);
    g_assert(!(d->state & SOCKET_STATE_READING_PAUSED));
    g_assert(!(d->state & SOCKET_STATE_READING_COMPLETE));
    socket_data_set_state(d, SOCKET_STATE_READING, true);

    do {
        ret = recv(d->fd, &d->buf_in[d->bytes_read],
                   bytes_pending,
                   MSG_DONTWAIT);
    } while(ret == -1 && socket_error() == EINTR);

    if (ret == -1) {
        g_assert(QEMU_SOCKET_RC_WOULDBLOCK(socket_error()));
    } else {
        d->bytes_read += ret;
    }

    if (ret == 0 || d->bytes_read == SOCKET_DATA_BUF_LEN) {
        socket_data_set_caps(d, SOCKET_CAP_READ, false);
        if (d->caps & SOCKET_CAP_WRITE) {
            qemu_set_fd_handler2(d->fd, NULL, NULL, socket_write, d);
        } else {
            qemu_set_fd_handler2(d->fd, NULL, NULL, NULL, NULL);
        }
        socket_data_set_state(d, SOCKET_STATE_READING, false);
        socket_data_set_state(d, SOCKET_STATE_READING_COMPLETE, true);
    }
}

static void socket_write(void *opaque)
{
    SocketData *d = opaque;
    int ret;
    size_t bytes_pending = MIN(SOCKET_DATA_WRITE_CHUNK,
                               SOCKET_DATA_BUF_LEN - d->bytes_written);

    g_assert(d->caps & SOCKET_CAP_WRITE);
    g_assert(!(d->state & SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_set_state(d, SOCKET_STATE_WRITING, true));

    do {
        ret = send(d->fd, &d->buf_out[d->bytes_written], bytes_pending,
                   MSG_DONTWAIT);
    } while (ret == -1 && socket_error() == EINTR);

    if (ret == -1) {
        g_assert(QEMU_SOCKET_RC_WOULDBLOCK(socket_error()));
    } else {
        d->bytes_written += ret;
    }

    if (d->bytes_written == SOCKET_DATA_BUF_LEN) {
        socket_data_set_caps(d, SOCKET_CAP_WRITE, false);
        if (d->caps & SOCKET_CAP_READ) {
            qemu_set_fd_handler2(d->fd, socket_can_read, socket_read, NULL, d);
        } else {
            qemu_set_fd_handler2(d->fd, NULL, NULL, NULL, NULL);
        }
        g_assert(socket_data_set_state(d, SOCKET_STATE_WRITING, false));
        g_assert(socket_data_set_state(d, SOCKET_STATE_WRITING_COMPLETE, true));
    }
}

/* socket handler tests */

static void test_socket_connect_helper(bool main_loop_blocking)
{
    MainLoop *ml = main_loop_new(main_loop_blocking, true);
    ServerData *sd;
    ClientData *cd;

    main_loop_start(ml);

    sd = server_data_new();
    g_assert(sd);
    cd = client_data_new(sd->addr, sd->port);
    g_assert(cd);

    qemu_set_fd_handler2_locked(sd->listen_fd,
                                NULL, server_listen, NULL, &sd->d);
    if (cd->d.state & SOCKET_STATE_CONNECTING) {
        qemu_set_fd_handler2_locked(cd->d.fd,
                                    NULL, NULL, client_connect, &cd->d);
    }

    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_CONNECTED));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_CONNECTED));

    server_data_cleanup(sd);
    client_data_cleanup(cd);
    main_loop_stop(ml);
    main_loop_cleanup(ml);
}

static void test_socket_connect_main_loop_blocking(void)
{
    test_socket_connect_helper(true);
}

static void test_socket_connect_main_loop_nonblocking(void)
{
    test_socket_connect_helper(false);
}

static void test_socket_server_read(bool main_loop_blocking)
{
    MainLoop *ml = main_loop_new(main_loop_blocking, true);
    //MainLoop *ml = main_loop_new(main_loop_blocking, false);
    ServerData *sd;
    ClientData *cd;

    main_loop_start(ml);

    sd = server_data_new();
    g_assert(sd);
    cd = client_data_new(sd->addr, sd->port);
    g_assert(cd);

    if (cd->d.state & SOCKET_STATE_CONNECTING) {
        qemu_set_fd_handler2_locked(cd->d.fd,
                                    NULL, NULL, client_connect, &cd->d);
    }
    qemu_set_fd_handler2_locked(sd->listen_fd,
                                NULL, server_listen, NULL, &sd->d);

    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_CONNECTED));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_CONNECTED));

    socket_data_set_caps(&sd->d, SOCKET_CAP_READ, true);
    socket_data_set_caps(&cd->d, SOCKET_CAP_WRITE, true);
    g_assert(socket_data_set_state(&sd->d, SOCKET_STATE_READING_PAUSED, true));
    qemu_notify_event();

    qemu_set_fd_handler2_locked(sd->d.fd,
                                socket_can_read, socket_read, NULL, &sd->d);
    qemu_set_fd_handler2_locked(cd->d.fd,
                                NULL, NULL, socket_write, &cd->d);

    g_assert(socket_data_wait_for_state(&cd->d,
                SOCKET_STATE_WRITING|SOCKET_STATE_WRITING_COMPLETE));
    usleep(100000);

    g_assert(sd->d.state & SOCKET_STATE_READING_PAUSED);
    g_assert(socket_data_set_state(&sd->d, SOCKET_STATE_READING_PAUSED, false));
    qemu_notify_event();

    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_READING_COMPLETE));
    g_assert(cd->d.state & SOCKET_STATE_WRITING_COMPLETE);
    g_assert(sd->d.state & SOCKET_STATE_READING_COMPLETE);

    g_assert(check_data_buffers(cd->d.buf_out, cd->d.bytes_written,
                                sd->d.buf_in, sd->d.bytes_read));

    /* FIXME: if deleting fd handlers is not effectively atomic, we must
     * synchronize on the global mutex, or a mutex for the socket, else
     * we run the risk of handlers executing on free'd data structures.
     */
    server_data_cleanup(sd);
    client_data_cleanup(cd);
    main_loop_stop(ml);
    main_loop_cleanup(ml);
}

static void test_socket_server_read_main_loop_blocking(void)
{
    test_socket_server_read(true);
}

static void test_socket_server_read_main_loop_nonblocking(void)
{
    test_socket_server_read(false);
}

static void test_socket_server_write_helper(bool main_loop_blocking)
{
    MainLoop *ml = main_loop_new(main_loop_blocking, true);
    //MainLoop *ml = main_loop_new(main_loop_blocking, false);
    ServerData *sd;
    ClientData *cd;

    main_loop_start(ml);

    sd = server_data_new();
    g_assert(sd);
    cd = client_data_new(sd->addr, sd->port);
    g_assert(cd);

    qemu_set_fd_handler2_locked(sd->listen_fd,
                                NULL, server_listen, NULL, &sd->d);
    if (cd->d.state & SOCKET_STATE_CONNECTING) {
        qemu_set_fd_handler2_locked(cd->d.fd,
                                    NULL, NULL, client_connect, &cd->d);
    }

    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_CONNECTED));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_CONNECTED));

    socket_data_set_caps(&sd->d, SOCKET_CAP_WRITE, true);
    socket_data_set_caps(&cd->d, SOCKET_CAP_READ, true);
    g_assert(socket_data_set_state(&cd->d, SOCKET_STATE_READING_PAUSED, true));

    qemu_set_fd_handler2_locked(sd->d.fd, NULL, NULL, socket_write, &sd->d);
    qemu_set_fd_handler2_locked(cd->d.fd,
                                socket_can_read, socket_read, NULL, &cd->d);

    g_assert(socket_data_wait_for_state(&sd->d,
                SOCKET_STATE_WRITING|SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_READING_PAUSED));
    usleep(100000);

    g_assert(cd->d.state & SOCKET_STATE_READING_PAUSED);
    /* signal can_read handler for server socket that it can continue */
    g_assert(socket_data_set_state(&cd->d, SOCKET_STATE_READING_PAUSED, false));
    qemu_notify_event();

    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_READING_COMPLETE));
    g_assert(cd->d.state & SOCKET_STATE_READING_COMPLETE);
    g_assert(sd->d.state & SOCKET_STATE_WRITING_COMPLETE);

    check_data_buffers(sd->d.buf_out, sd->d.bytes_written,
                       cd->d.buf_in, cd->d.bytes_read);

    server_data_cleanup(sd);
    client_data_cleanup(cd);
    main_loop_stop(ml);
    main_loop_cleanup(ml);
}

static void test_socket_server_write_main_loop_blocking(void)
{
    test_socket_server_write_helper(true);
}

static void test_socket_server_write_main_loop_nonblocking(void)
{
    test_socket_server_write_helper(false);
}

static void test_socket_server_read_write_helper(bool main_loop_blocking)
{
    MainLoop *ml = main_loop_new(main_loop_blocking, true);
    ServerData *sd;
    ClientData *cd;

    main_loop_start(ml);

    sd = server_data_new();
    g_assert(sd);
    cd = client_data_new(sd->addr, sd->port);
    g_assert(cd);

    if (cd->d.state & SOCKET_STATE_CONNECTING) {
        qemu_set_fd_handler2_locked(cd->d.fd, NULL, NULL, client_connect,
                                    &cd->d);
    }
    qemu_set_fd_handler2_locked(sd->listen_fd,
                                NULL, server_listen, NULL, &sd->d);
    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_CONNECTED));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_CONNECTED));

    socket_data_set_caps(&sd->d, SOCKET_CAP_READ | SOCKET_CAP_WRITE, true);
    socket_data_set_caps(&cd->d, SOCKET_CAP_READ | SOCKET_CAP_WRITE, true);
    g_assert(socket_data_set_state(&sd->d, SOCKET_STATE_READING_PAUSED, true));
    g_assert(socket_data_set_state(&cd->d, SOCKET_STATE_READING_PAUSED, true));

    qemu_set_fd_handler2_locked(sd->d.fd,
                                socket_can_read, socket_read, socket_write,
                                &sd->d);
    qemu_set_fd_handler2_locked(cd->d.fd,
                                 socket_can_read, socket_read, socket_write,
                                 &cd->d);

    usleep(100000);

    g_assert(cd->d.state & SOCKET_STATE_READING_PAUSED);
    g_assert(sd->d.state & SOCKET_STATE_READING_PAUSED);
    g_assert(!(cd->d.state &
               (SOCKET_STATE_READING|SOCKET_STATE_READING_COMPLETE)));
    g_assert(!(sd->d.state &
               (SOCKET_STATE_READING|SOCKET_STATE_READING_COMPLETE)));
    /* signal can_read handler for server socket that it can continue */
    g_assert(socket_data_set_state(&cd->d, SOCKET_STATE_READING_PAUSED, false));
    g_assert(socket_data_set_state(&sd->d, SOCKET_STATE_READING_PAUSED, false));
    qemu_notify_event();

    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_READING_COMPLETE));
    g_assert(socket_data_wait_for_state(&cd->d, SOCKET_STATE_WRITING_COMPLETE));
    g_assert(socket_data_wait_for_state(&sd->d, SOCKET_STATE_READING_COMPLETE));

    g_assert(check_data_buffers(sd->d.buf_out, sd->d.bytes_written,
                                cd->d.buf_in, cd->d.bytes_read));
    g_assert(check_data_buffers(cd->d.buf_out, cd->d.bytes_written,
                                sd->d.buf_in, sd->d.bytes_read));

    server_data_cleanup(sd);
    client_data_cleanup(cd);
    main_loop_stop(ml);
    main_loop_cleanup(ml);
}

static void test_socket_server_read_write_main_loop_blocking(void)
{
    test_socket_server_read_write_helper(true);
}

static void test_socket_server_read_write_main_loop_nonblocking(void)
{
    test_socket_server_read_write_helper(false);
}

int main(int argc, char **argv)
{
    qemu_init_main_loop();

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/event_handlers/main_loop_idle", test_idle_loop);
    g_test_add_func("/event_handlers/main_loop_busy", test_busy_loop);
#ifndef _WIN32
    g_test_add_func("/event_handlers/fd/pipe_blocking/main_loop_blocking",
                    test_pipe_blocking_main_loop_blocking);
    g_test_add_func("/event_handlers/fd/pipe_blocking/main_loop_nonblocking",
                    test_pipe_blocking_main_loop_nonblocking);
    g_test_add_func("/event_handlers/fd/pipe_nonblocking/main_loop_blocking",
                    test_pipe_nonblocking_main_loop_blocking);
    g_test_add_func("/event_handlers/fd/pipe_nonblocking/main_loop_nonblocking",
                    test_pipe_nonblocking_main_loop_nonblocking);
#endif
    g_test_add_func("/event_handlers/socket/connect/main_loop_blocking",
                    test_socket_connect_main_loop_blocking);
    g_test_add_func("/event_handlers/socket/connect/main_loop_nonblocking",
                    test_socket_connect_main_loop_nonblocking);
    g_test_add_func("/event_handlers/socket/server_read/main_loop_blocking",
                    test_socket_server_read_main_loop_blocking);
    g_test_add_func("/event_handlers/socket/server_read/main_loop_nonblocking",
                    test_socket_server_read_main_loop_nonblocking);
    g_test_add_func("/event_handlers/socket/server_write/main_loop_blocking",
                    test_socket_server_write_main_loop_blocking);
    g_test_add_func("/event_handlers/socket/server_write/main_loop_nonblocking",
                    test_socket_server_write_main_loop_nonblocking);
    g_test_add_func("/event_handlers/socket/server_read_write/main_loop_blocking",
                    test_socket_server_read_write_main_loop_blocking);
    g_test_add_func("/event_handlers/socket/server_read_write/main_loop_nonblocking",
                    test_socket_server_read_write_main_loop_nonblocking);
    return g_test_run();
}
