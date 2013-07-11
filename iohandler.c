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

typedef struct IOHandlerRecord {
    IOCanReadHandler *fd_read_poll;
    IOHandler *fd_read;
    IOHandler *fd_write;
    void *opaque;
    QLIST_ENTRY(IOHandlerRecord) next;
    int fd;
    int pollfds_idx;
    bool deleted;
} IOHandlerRecord;

static QLIST_HEAD(, IOHandlerRecord) io_handlers =
    QLIST_HEAD_INITIALIZER(io_handlers);


#ifndef _WIN32
/* XXX: fd_read_poll should be suppressed, but an API change is
   necessary in the character devices to suppress fd_can_read(). */
int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    IOHandlerRecord *ioh;

    assert(fd >= 0);

    if (!fd_read && !fd_write) {
        QLIST_FOREACH(ioh, &io_handlers, next) {
            if (ioh->fd == fd) {
                ioh->deleted = 1;
                break;
            }
        }
    } else {
        QLIST_FOREACH(ioh, &io_handlers, next) {
            if (ioh->fd == fd)
                goto found;
        }
        ioh = g_malloc0(sizeof(IOHandlerRecord));
        QLIST_INSERT_HEAD(&io_handlers, ioh, next);
    found:
        ioh->fd = fd;
        ioh->fd_read_poll = fd_read_poll;
        ioh->fd_read = fd_read;
        ioh->fd_write = fd_write;
        ioh->opaque = opaque;
        ioh->pollfds_idx = -1;
        ioh->deleted = 0;
        qemu_notify_event();
    }
    return 0;
}
#else

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
} SocketHandler;

static gboolean socket_handler_prepare(GSource *source, gint *timeout)
{
    SocketHandler *socket_handler = (SocketHandler *)source;
    socket_handler->pfd.events = 0;

    WSAEventSelect(socket_handler->fd, socket_handler->event,
                   socket_handler->network_events_mask);

    /* XXX: glib only sets G_IO_IN for event handles */
    socket_handler->pfd.events =
        socket_handler->network_events_mask ? G_IO_IN : 0;

#if 0
    if (socket_handler->network_events & (FD_READ | FD_ACCEPT)) {
        socket_handler->pfd.events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
    }

    if (socket_handler->network_events & (FD_WRITE | FD_CONNECT | FD_OOB)) {
        socket_handler->pfd.events |= G_IO_OUT | G_IO_ERR;
    }
#endif

    if (!socket_handler->pfd_added) {
        /* TODO: fix this cast */
        socket_handler->pfd.fd = (gint)socket_handler->event;
        socket_handler->pfd.revents = 0;
        g_source_add_poll(source, &socket_handler->pfd);
        socket_handler->pfd_added = true;
    }

    return false;
}

static gboolean socket_handler_check(GSource *source)
{
    SocketHandler *socket_handler = (SocketHandler *)source;
    int ret;

    if ((socket_handler->pfd.events & socket_handler->pfd.revents) == 0) {
        return false;
    }

    ret = WSAEnumNetworkEvents(socket_handler->fd, socket_handler->event,
                               &socket_handler->network_events);
    if (ret) {
        /* TODO: check for WSAEINPROGRESS */
        g_warning("socket_handler error");
        return false;
    }

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

    if ((revents & socket_handler->pfd.events) == 0) {
        return false;
    }

    network_events_active = socket_handler->network_events.lNetworkEvents;

    /* TODO: should we suppress any of these? what about OOB/HUP/etc? */
    if (socket_handler->read &&
        (network_events_active & (FD_READ | FD_ACCEPT))) {
        if (!socket_handler->read_poll || 
            socket_handler->read_poll(socket_handler->opaque)) {
            socket_handler->read(socket_handler->opaque);
            dispatched = true;
        }
    }

    if (socket_handler->write &&
        (network_events_active & (FD_WRITE | FD_CONNECT))) {
        socket_handler->write(socket_handler->opaque);
        dispatched = true;
    }

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
        network_events_mask |= FD_READ | FD_ACCEPT | FD_CLOSE;
    }
   
    if (fd_write) {
        /* TODO: double-check these */
        network_events_mask |= FD_WRITE | FD_CONNECT | FD_OOB;
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

void qemu_iohandler_fill(GArray *pollfds)
{
    IOHandlerRecord *ioh;

    QLIST_FOREACH(ioh, &io_handlers, next) {
        int events = 0;

        if (ioh->deleted)
            continue;
        if (ioh->fd_read &&
            (!ioh->fd_read_poll ||
             ioh->fd_read_poll(ioh->opaque) != 0)) {
            events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
        }
        if (ioh->fd_write) {
            events |= G_IO_OUT | G_IO_ERR;
        }
        if (events) {
            GPollFD pfd = {
                .fd = ioh->fd,
                .events = events,
            };
            ioh->pollfds_idx = pollfds->len;
            g_array_append_val(pollfds, pfd);
        } else {
            ioh->pollfds_idx = -1;
        }
    }
}

void qemu_iohandler_poll(GArray *pollfds, int ret)
{
    if (ret > 0) {
        IOHandlerRecord *pioh, *ioh;

        QLIST_FOREACH_SAFE(ioh, &io_handlers, next, pioh) {
            int revents = 0;

            if (!ioh->deleted && ioh->pollfds_idx != -1) {
                GPollFD *pfd = &g_array_index(pollfds, GPollFD,
                                              ioh->pollfds_idx);
                revents = pfd->revents;
            }

            if (!ioh->deleted && ioh->fd_read &&
                (revents & (G_IO_IN | G_IO_HUP | G_IO_ERR))) {
                ioh->fd_read(ioh->opaque);
            }
            if (!ioh->deleted && ioh->fd_write &&
                (revents & (G_IO_OUT | G_IO_ERR))) {
                ioh->fd_write(ioh->opaque);
            }

            /* Do this last in case read/write handlers marked it for deletion */
            if (ioh->deleted) {
                QLIST_REMOVE(ioh, next);
                g_free(ioh);
            }
        }
    }
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
