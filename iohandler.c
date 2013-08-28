/*
 * QEMU System Emulator - managing I/O handler
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config-host.h"
#include "qemu-common.h"
#include "qemu/queue.h"
#include "block/aio.h"
#include "qemu/main-loop.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef _WIN32

typedef struct IOHandlerRecord {
    IOCanReadHandler *fd_read_poll;
    IOHandler *fd_read;
    IOHandler *fd_write;
    void *opaque;
    QLIST_ENTRY(IOHandlerRecord) next;
    GPollFD pfd;
    bool deleted;
#ifdef _WIN32
    int fd;
    int wsa_events;
    int wsa_event_mask;
#endif
} IOHandlerRecord;

typedef struct FDSource {
    GSource source;
    QemuMutex mutex;
    QLIST_HEAD(, IOHandlerRecord) io_handlers;
    bool dispatching;
    QemuCond dispatching_complete;
} FDSource;

static gboolean fd_source_prepare(GSource *source, gint *timeout)
{
    FDSource *fdsrc = (FDSource *)source;
    IOHandlerRecord *pioh, *ioh;

    qemu_mutex_lock(&fdsrc->mutex);

    QLIST_FOREACH_SAFE(ioh, &fdsrc->io_handlers, next, pioh) {
        int events = 0;

        if (ioh->deleted) {
            g_source_remove_poll(source, &ioh->pfd);
            QLIST_REMOVE(ioh, next);
            g_free(ioh);
            continue;
        }
        if (ioh->fd_read &&
            (!ioh->fd_read_poll ||
             ioh->fd_read_poll(ioh->opaque) != 0)) {
            events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
        }
        if (ioh->fd_write) {
            events |= G_IO_OUT | G_IO_ERR;
        }
        if (events) {
            ioh->pfd.events = events;
        } else {
            ioh->pfd.events = 0;
        }
    }

    qemu_mutex_unlock(&fdsrc->mutex);

    return false;
}

static gboolean fd_source_check(GSource *source)
{
    FDSource *fdsrc = (FDSource *)source;
    IOHandlerRecord *ioh;
    gboolean dispatch_needed = false;

    qemu_mutex_lock(&fdsrc->mutex);

    QLIST_FOREACH(ioh, &fdsrc->io_handlers, next) {
        if (ioh->pfd.revents) {
            dispatch_needed = true;
        }
    }

    qemu_mutex_unlock(&fdsrc->mutex);

    return dispatch_needed;
}

static gboolean fd_source_dispatch(GSource *source, GSourceFunc cb,
                                   gpointer user_data)
{
    FDSource *fdsrc = (FDSource *)source;
    IOHandlerRecord *pioh, *ioh;

    qemu_mutex_lock(&fdsrc->mutex);
    fdsrc->dispatching = true;
    qemu_mutex_unlock(&fdsrc->mutex);

    /* dispatch functions may modify io_handlers as we
     * call them here, but we are guaranteed no other thread will
     * access this list since, while we're dispatching, all functions
     * that attempt to access FDSource members must wait on
     * dispatching_complete condition unless g_main_context_is_owner()
     * is true for the calling thread. as a result, we can walk the
     * list here without holding the FSource mutex, since it's
     * guaranteed these conditions will hold due to
     * g_main_context_acquire() being required prior to calling
     * g_main_context_dispatch()
     */
    QLIST_FOREACH_SAFE(ioh, &fdsrc->io_handlers, next, pioh) {
        int revents = 0;

        if (!ioh->deleted) {
            revents = ioh->pfd.revents;
            ioh->pfd.revents = 0;
        }

        if (!ioh->deleted && ioh->fd_read &&
            (revents & (G_IO_IN | G_IO_HUP | G_IO_ERR))) {
            ioh->fd_read(ioh->opaque);
        }
        if (!ioh->deleted && ioh->fd_write &&
            (revents & (G_IO_OUT | G_IO_ERR))) {
            ioh->fd_write(ioh->opaque);
        }
    }

    qemu_mutex_lock(&fdsrc->mutex);
    fdsrc->dispatching = false;
    qemu_cond_broadcast(&fdsrc->dispatching_complete);
    qemu_mutex_unlock(&fdsrc->mutex);

    return true;
}

static void fd_source_finalize(GSource *source)
{
}

static GSourceFuncs fd_source_funcs = {
    fd_source_prepare,
    fd_source_check,
    fd_source_dispatch,
    fd_source_finalize
};

/* TODO: do we still need this ? */
static gboolean socket_source_cb(gpointer user_data)
{
    return true;
}

static gboolean fd_source_cb(gpointer user_data)
{
    return true;
}

void fd_source_attach(GMainContext *ctx)
{
    GSource *src = g_source_new(&fd_source_funcs, sizeof(FDSource));
    FDSource *fdsrc = (FDSource *)src;

    QLIST_INIT(&fdsrc->io_handlers);
    qemu_mutex_init(&fdsrc->mutex);
    g_source_set_callback(src, fd_source_cb, NULL, NULL);
    g_source_attach(src, ctx);
}

static int fd_source_set_handler(GMainContext *ctx,
                                 int fd,
                                 IOCanReadHandler *fd_read_poll,
                                 IOHandler *fd_read,
                                 IOHandler *fd_write,
                                 void *opaque)
{
    GSource *src;
    FDSource *fdsrc;
    IOHandlerRecord *ioh;
    bool in_dispatch;

    assert(fd >= 0);

    if (!ctx) {
        ctx = g_main_context_default();
    }
    /* FIXME: we need a more reliable way to find our GSource */
    src = g_main_context_find_source_by_funcs_user_data(
            ctx, &fd_source_funcs, NULL);
    assert(src);
    fdsrc = (FDSource *)src;

    qemu_mutex_lock(&fdsrc->mutex);
    in_dispatch = fdsrc->dispatching && g_main_context_is_owner(ctx);

    if (!in_dispatch) {
        while (fdsrc->dispatching) {
            qemu_cond_wait(&fdsrc->dispatching_complete, &fdsrc->mutex);
        }
    }

    if (!fd_read && !fd_write) {
        QLIST_FOREACH(ioh, &fdsrc->io_handlers, next) {
            if (ioh->pfd.fd == fd) {
                ioh->deleted = 1;
                break;
            }
        }
    } else {
        QLIST_FOREACH(ioh, &fdsrc->io_handlers, next) {
            if (ioh->pfd.fd == fd)
                goto found;
        }
        ioh = g_malloc0(sizeof(IOHandlerRecord));
        QLIST_INSERT_HEAD(&fdsrc->io_handlers, ioh, next);
        ioh->pfd.fd = fd;
        g_source_add_poll(src, &ioh->pfd);
found:
        ioh->fd_read_poll = fd_read_poll;
        ioh->fd_read = fd_read;
        ioh->fd_write = fd_write;
        ioh->opaque = opaque;
        ioh->deleted = 0;
        g_main_context_wakeup(ctx);
    }

    qemu_mutex_unlock(&fdsrc->mutex);

    return 0;
}

#else

typedef struct SocketSource {
    GSource source;
    QemuMutex mutex;
    QLIST_HEAD(, IOHandlerRecord) io_handlers;
    bool dispatching;
    QemuCond dispatching_complete;
} SocketSource;

static gboolean socket_source_prepare(GSource *source, gint *timeout)
{
    SocketSource *ssrc = (SocketSource *)source;
    IOHandlerRecord *pioh, *ioh;

    qemu_mutex_lock(&ssrc->mutex);

    QLIST_FOREACH_SAFE(ioh, &ssrc->io_handlers, next, pioh) {
        int events = 0;

        if (ioh->deleted) {
            g_source_remove_poll(source, &ioh->pfd);
            WSAEventSelect(ioh->fd, NULL, 0);
            QLIST_REMOVE(ioh, next);
            g_free(ioh);
            continue;
        }
        ioh->wsa_event_mask = 0;
        if (ioh->fd_read &&
            (!ioh->fd_read_poll ||
             ioh->fd_read_poll(ioh->opaque) != 0)) {
            ioh->wsa_event_mask = FD_READ | FD_ACCEPT | FD_CLOSE;
        }

        if (ioh->fd_write) {
            ioh->wsa_event_mask |= FD_WRITE | FD_CONNECT | FD_OOB;
        }

        if (!ioh->wsa_event_mask && !fd_read_poll) {
            deleted = true;
        }

        if (ioh->wsa_event_mask) {
            WSAEventSelect(ioh->fd, ioh->pfd.fd, ioh->wsa_event_mask);
        }
    }

    qemu_mutex_unlock(&ssrc->mutex);

    return false;
}

static gboolean socket_source_check(GSource *source)
{
    SocketSource *ssrc = (SocketSource *)source;
    IOHandlerRecord *ioh;
    gboolean dispatch_needed = false;

    qemu_mutex_lock(&ssrc->mutex);

    QLIST_FOREACH(ioh, &ssrc->io_handler, next) {
        WSANETWORKEVENTS wsa_events = { 0 };
        int ret;
        if (ioh->pfd.revents == 0) {
            continue;
        }
        ret = WSAEnumNetworkEvents(ioh->fd, ioh->pfd.fd, &wsa_events);
        g_assert(ret == 0);

        ioh->wsa_events = wsa_events.lNetworkEvents;
        if (ioh->wsa_events & ioh->wsa_event_mask) {
            dispatch_needed = true;
        }
    }

    qemu_mutex_unlock(&ssrc->mutex);

    return dispatch_needed;
}

static gboolean socket_source_dispatch(GSource *source,
                                       GSourceFunc cb,
                                       gpointer user_data)
{
    SocketSource *fdsrc = (SocketSource *)source;
    IOHandlerRecord *pioh, *ioh;

    qemu_mutex_lock(&fdsrc->mutex);
    fdsrc->dispatching = true;
    qemu_mutex_unlock(&fdsrc->mutex);

    /* dispatch functions may modify io_handlers as we
     * call them here, but we are guaranteed no other thread will
     * access this list since, while we're dispatching, all functions
     * that attempt to access FDSource members must wait on
     * dispatching_complete condition unless g_main_context_is_owner()
     * is true for the calling thread. as a result, we can walk the
     * list here without holding the FSource mutex, since it's
     * guaranteed these conditions will hold due to
     * g_main_context_acquire() being required prior to calling
     * g_main_context_dispatch()
     */
    QLIST_FOREACH_SAFE(ioh, &fdsrc->io_handlers, next, pioh) {
        int revents = 0;

        if (ioh->deleted) {
            continue;
        }

        if (ioh->fd_read &&
            (ioh->wsa_events & (FD_READ | FD_ERR | FD_OOB))) {
            ioh->fd_read(ioh->opaque);
        }

        if (ioh->fd_write &&
            (ioh->wsa_events & (FD_WRITE | FD_CONNECT))) {
            ioh->fd_write(ioh->opaque);
        }

        ioh->wsa_events = 0;
        ioh->pfd.revents = 0;
    }

    qemu_mutex_lock(&ssrc->mutex);
    ssrc->dispatching = false;
    qemu_cond_broadcast(&ssrc->dispatching_complete);
    qemu_mutex_unlock(&ssrc->mutex);

    return true;
}

static void socket_source_finalize(GSource *source)
{
}

static GSourceFuncs socket_source_funcs = {
    socket_source_prepare,
    socket_source_check,
    socket_source_dispatch,
    socket_source_finalize,
};

void socket_source_attach(GMainContext *ctx)
{
    GSource *src = g_source_new(&socket_source_funcs, sizeof(SocketSource));
    SocketSource *ssrc = (SocketSource *)src;

    QLIST_INIT(&ssrc->io_handlers);
    qemu_mutex_init(&fdsrc->mutex);
    g_source_set_callback(src, socket_source_cb, NULL, NULL);
    g_source_attach(src, ctx);

}

static int socket_source_set_handler(GMainContext *ctx,
                                     int fd,
                                     IOCanReadHandler *fd_read_poll,
                                     IOHandler *fd_read,
                                     IOHandler *fd_write,
                                     void *opaque)
{
    GSource *src;
    SocketSource *ssrc;
    IOHandlerRecord *ioh;
    bool in_dispatch;

    assert(fd >= 0);

    if (!ctx) {
        ctx = g_main_context_default();
    }
    /* FIXME: we need a more reliable way to find our GSource */
    src = g_main_context_find_source_by_funcs_user_data(
            ctx, &socket_source_funcs, NULL);
    assert(src);
    ssrc = (SocketSource *)src;

    qemu_mutex_lock(&ssrc->mutex);
    in_dispatch = ssrc->dispatching && g_main_context_is_owner(ctx);

    if (!in_dispatch) {
        while (ssrc->dispatching) {
            qemu_cond_wait(&ssrc->dispatching_complete, &ssrc->mutex);
        }
    }

    if (!fd_read && !fd_write) {
        QLIST_FOREACH(ioh, &ssrc->io_handlers, next) {
            if (ioh->fd == fd) {
                ioh->deleted = 1;
                break;
            }
        }
    } else {
        QLIST_FOREACH(ioh, &ssrc->io_handlers, next) {
            if (ioh->fd == fd)
                goto found;
        }
        ioh = g_malloc0(sizeof(IOHandlerRecord));
        QLIST_INSERT_HEAD(&ssrc->io_handlers, ioh, next);
        ioh->fd = fd;
        ioh->pfd.fd = WSACreateEvent();
        /* TODO: is G_IO_IN sufficient for all socket events? */
        ioh->pfd.events = G_IO_IN;
        /* TODO: only check for events our handlers would be interested in */
        WSAEventSelect(ioh->fd, ioh->pfd.fd,
                       FD_READ | FD_ACCEPT | FD_CLOSE |
                       FD_CONNECT | FD_WRITE | FD_OOB);
        g_source_add_poll(src, &ioh->pfd);
found:
        ioh->fd_read_poll = fd_read_poll;
        ioh->fd_read = fd_read;
        ioh->fd_write = fd_write;
        ioh->opaque = opaque;
        ioh->deleted = 0;
        g_main_context_wakeup(ctx);
    }

    qemu_mutex_unlock(&ssrc->mutex);

    return 0;

}
#endif

int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
#ifndef _WIN32
    return fd_source_set_handler(NULL, fd, fd_read_poll, fd_read, fd_write,
                                 opaque);
#else
    return socket_source_set_handler(NULL, fd, fd_read_poll, fd_read, fd_write,
                                     opaque);
#endif
}

#if 0
//#else

typedef struct SocketHandler {
    GSource source;
    QemuMutex mutex;
    int fd;
    HANDLE event;
    WSANETWORKEVENTS network_events;
    long network_events_mask;
    IOCanReadHandler *read_poll;
    IOHandler *read;
    IOHandler *write;
    void *opaque;
    GPollFD pfd;
    bool pfd_added;
    bool writeable; /* we only get FD_WRITE once, so capture it */
    bool deleted;
    bool read_deferred;
} SocketHandler;

static gboolean socket_handler_prepare(GSource *source, gint *timeout)
{
    SocketHandler *socket_handler = (SocketHandler *)source;
    socket_handler->pfd.events = 0;

    WSAEventSelect(socket_handler->fd, socket_handler->event,
                   socket_handler->network_events_mask);
    /* TODO: does this reset events already set? */
#if 0
    WSAEventSelect(socket_handler->fd, socket_handler->event,
                   socket_handler->network_events_mask);
#endif

    /* XXX: glib only sets G_IO_IN for event handles */
    if (socket_handler->network_events.lNetworkEvents & (FD_READ | FD_ACCEPT)) {
        socket_handler->pfd.events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
    }

    if (socket_handler->network_events.lNetworkEvents & (FD_WRITE | FD_CONNECT | FD_OOB)) {
        socket_handler->pfd.events |= G_IO_OUT | G_IO_HUP | G_IO_ERR;
    }

    if (!socket_handler->pfd_added) {
        /* TODO: fix this cast */
        socket_handler->pfd.fd = (gint)socket_handler->event;
        socket_handler->pfd.revents = 0;
        g_source_add_poll(source, &socket_handler->pfd);
        socket_handler->pfd_added = true;
    }

    if (socket_handler->read_deferred) {
        return true;
    }

    return false;
}

static gboolean socket_handler_check(GSource *source)
{
    SocketHandler *socket_handler = (SocketHandler *)source;
    int ret;

#if 0
    if ((socket_handler->pfd.events & socket_handler->pfd.revents) == 0) {
        return false;
    }
#endif
    if (socket_handler->deleted) {
        return false;
    }

    ret = WSAEnumNetworkEvents(socket_handler->fd, socket_handler->event,
                               &socket_handler->network_events);
    if (ret) {
        /* TODO: check for WSAEINPROGRESS */
        g_warning("socket_handler error");
        return false;
    }

    /* FIXME: this may break if they remove write handler and re-add later,
     * as we may 'lose' the FD_WRITE and never trigger again. we should
     * probably just poll if FD_WRITE is set
     */
    //if (socket_handler->network_events_mask & FD_WRITE) {
#if 0
    if (socket_handler->write) {
        socket_handler->writeable = true;
        return true;
        if (!socket_handler->writeable) {
            if (socket_handler->network_events.lNetworkEvents & FD_WRITE) {
                socket_handler->writeable = true;
            }
        }
        if (socket_handler->writeable) {
            return true;
        }
    }
#endif

    return socket_handler->network_events.lNetworkEvents &
           socket_handler->network_events_mask;
}

static gboolean socket_handler_dispatch(GSource *source, GSourceFunc cb,
                                        gpointer user_data)
{
    SocketHandler *socket_handler = (SocketHandler *)source;
    gushort revents = socket_handler->pfd.revents;
    gboolean dispatched = false;
    long network_events_active;

    socket_handler->pfd.revents = 0;

    if (socket_handler->deleted) {
        return false;
    }

    network_events_active = socket_handler->network_events.lNetworkEvents;

    /* TODO: should we suppress any of these? what about OOB/HUP/etc? */
    if (socket_handler->read &&
        ((network_events_active & (FD_READ | FD_ACCEPT)) || socket_handler->read_deferred)) {
        if (!socket_handler->read_poll ||
            socket_handler->read_poll(socket_handler->opaque)) {
            socket_handler->read(socket_handler->opaque);
            dispatched = true;
            socket_handler->read_deferred = false;
        } else {
            /* TODO: why are we not getting FD_READ again after a
             * read_poll fail???
             */
            /*
            WSAEventSelect(socket_handler->fd, socket_handler->event,
                           socket_handler->network_events_mask);
                           */
            socket_handler->read_deferred = true;
            dispatched = true;
        }
    }

    if (socket_handler->write &&
        (network_events_active & (FD_WRITE | FD_CONNECT))) {
        socket_handler->write(socket_handler->opaque);
        dispatched = true;
    }
#if 0
    if (socket_handler->write && socket_handler->writeable) {
        socket_handler->write(socket_handler->opaque);
        dispatched = true;
    }
#endif

    return dispatched;
}

static void socket_handler_finalize(GSource *source)
{
    SocketHandler *socket_handler = (SocketHandler *)source;

    if (socket_handler->pfd_added) {
        g_source_remove_poll(source, &socket_handler->pfd);
    }
    WSACloseEvent(socket_handler->event);
}

static GSourceFuncs socket_handler_funcs = {
    socket_handler_prepare,
    socket_handler_check,
    socket_handler_dispatch,
    socket_handler_finalize
};

int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    GSource *source;
    SocketHandler *socket_handler;
    GMainContext *ctx = g_main_context_default();
    long network_events_mask = 0;
    bool deleted = false;

    if (fd_read) {
        /* TODO: double-check these */
        //network_events_mask |= FD_READ | FD_ACCEPT | FD_CLOSE;
        network_events_mask |= FD_READ | FD_ACCEPT | FD_CLOSE | FD_CONNECT | FD_OOB;
    }

    if (fd_write) {
        /* TODO: double-check these */
        //network_events_mask |= FD_WRITE | FD_CONNECT | FD_OOB;
        network_events_mask |= FD_WRITE | FD_CONNECT | FD_OOB | FD_ACCEPT;
    }

    if (!network_events_mask && !fd_read_poll) {
        deleted = true;
    }

    source = g_main_context_find_source_by_funcs_user_data(
                ctx, &socket_handler_funcs, (gpointer)fd);
    socket_handler = (SocketHandler *)source;

    if (deleted) {
        if (source) {
            /* FIXME: need to finalize/unref at some point, have
             * intermittent segfaults if we unref while in dispatch
             */
            //g_source_unref(source);
            socket_handler->deleted = true;
            g_source_destroy(source);
        }
        return 0;
    }

    if (!source) {
        source = g_source_new(&socket_handler_funcs, sizeof(SocketHandler));
        socket_handler = (SocketHandler *)source;
        socket_handler->fd = fd;
        socket_handler->event = WSACreateEvent();
        qemu_mutex_init(&socket_handler->mutex);
        /* XXX: thread-safe to modify after attach? */
        g_source_attach(source, ctx);
        g_source_set_callback(source, NULL, (gpointer)fd, NULL);
    }

    socket_handler->read_poll =  fd_read_poll;
    socket_handler->read = fd_read;
    socket_handler->write = fd_write;
    socket_handler->opaque = opaque;
    socket_handler->network_events_mask = network_events_mask;
    WSAEventSelect(socket_handler->fd, socket_handler->event,
                   socket_handler->network_events_mask);
    //SetEvent(socket_handler->event);
    g_main_context_wakeup(ctx);

    return 0;
}
#endif

int qemu_set_fd_handler(int fd,
                        IOHandler *fd_read,
                        IOHandler *fd_write,
                        void *opaque)
{
    return qemu_set_fd_handler2(fd, NULL, fd_read, fd_write, opaque);
}

/* reaping of zombies.  right now we're not passing the status to
   anyone, but it would be possible to add a callback.  */
#ifndef _WIN32
typedef struct ChildProcessRecord {
    int pid;
    QLIST_ENTRY(ChildProcessRecord) next;
} ChildProcessRecord;

static QLIST_HEAD(, ChildProcessRecord) child_watches =
    QLIST_HEAD_INITIALIZER(child_watches);

static QEMUBH *sigchld_bh;

static void sigchld_handler(int signal)
{
    qemu_bh_schedule(sigchld_bh);
}

static void sigchld_bh_handler(void *opaque)
{
    ChildProcessRecord *rec, *next;

    QLIST_FOREACH_SAFE(rec, &child_watches, next, next) {
        if (waitpid(rec->pid, NULL, WNOHANG) == rec->pid) {
            QLIST_REMOVE(rec, next);
            g_free(rec);
        }
    }
}

static void qemu_init_child_watch(void)
{
    struct sigaction act;
    sigchld_bh = qemu_bh_new(sigchld_bh_handler, NULL);

    act.sa_handler = sigchld_handler;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, NULL);
}

int qemu_add_child_watch(pid_t pid)
{
    ChildProcessRecord *rec;

    if (!sigchld_bh) {
        qemu_init_child_watch();
    }

    QLIST_FOREACH(rec, &child_watches, next) {
        if (rec->pid == pid) {
            return 1;
        }
    }
    rec = g_malloc0(sizeof(ChildProcessRecord));
    rec->pid = pid;
    QLIST_INSERT_HEAD(&child_watches, rec, next);
    return 0;
}
#endif
