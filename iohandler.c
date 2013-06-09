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
    GPollFD pfd;
    bool pfd_added;
} IOHandlerRecord;

typedef struct IOHandlerSource {
    GSource source;
    QLIST_HEAD(, IOHandlerRecord) io_handlers;
    QemuMutex mutex;
} IOHandlerSource;

static gboolean iohandler_prepare(GSource *source, gint *timeout)
{
    IOHandlerSource *s = (IOHandlerSource *)source;
    IOHandlerRecord *ioh;

    qemu_mutex_lock(&s->mutex);

    QLIST_FOREACH(ioh, &s->io_handlers, next) {
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
            ioh->pfd.fd = ioh->fd;
            ioh->pfd.events = events;
            if (!ioh->pfd_added) {
                g_source_add_poll(source, &ioh->pfd);
                ioh->pfd_added = true;
            }
        } else {
            ioh->pfd.events = 0;
            ioh->pfd.revents = 0;
        }
    }

    qemu_mutex_unlock(&s->mutex);

    *timeout = 10;
    return false;
}

static gboolean iohandler_check(GSource *source)
{
    IOHandlerSource *s = (IOHandlerSource *)source;
    IOHandlerRecord *ioh;

    qemu_mutex_lock(&s->mutex);

    QLIST_FOREACH(ioh, &s->io_handlers, next) {
        if (ioh->deleted) {
            continue;
        }
        if (ioh->pfd.revents) {
            return true;
        }
    }

    qemu_mutex_unlock(&s->mutex);

    return false;
}

static gboolean iohandler_dispatch(GSource *source, GSourceFunc cb,
                                   gpointer user_data)
{
    IOHandlerSource *s = (IOHandlerSource *)source;
    IOHandlerRecord *pioh, *ioh;


    qemu_mutex_lock(&s->mutex);

    QLIST_FOREACH_SAFE(ioh, &s->io_handlers, next, pioh) {
        int revents = ioh->pfd.revents;
        if (!ioh->deleted && ioh->fd_read &&
            (revents && (G_IO_IN | G_IO_HUP | G_IO_ERR))) {
            ioh->fd_read(ioh->opaque);
        }

        if (!ioh->deleted && ioh->fd_write &&
            (revents & (G_IO_OUT | G_IO_ERR))) {
            ioh->fd_write(ioh->opaque);
        }

        /* Do this last in case read/write handlers marked it for deletion */
        if (ioh->deleted) {
            if (ioh->pfd_added) {
                g_source_remove_poll(source, &ioh->pfd);
            }
            QLIST_REMOVE(ioh, next);
            g_free(ioh);
        }
    }

    qemu_mutex_unlock(&s->mutex);

    return true;
}

static void iohandler_finalize(GSource *source)
{
    IOHandlerSource *s = (IOHandlerSource *)source;
    IOHandlerRecord *pioh, *ioh;

    qemu_mutex_lock(&s->mutex);

    QLIST_FOREACH_SAFE(ioh, &s->io_handlers, next, pioh) {
        if (ioh->pfd_added) {
            g_source_remove_poll(source, &ioh->pfd);
        }
        QLIST_REMOVE(ioh, next);
        g_free(ioh);
    }

    qemu_mutex_unlock(&s->mutex);

    g_free(s);
}

static GSourceFuncs iohandler_source_funcs = {
    iohandler_prepare,
    iohandler_check,
    iohandler_dispatch,
    iohandler_finalize,
};

void iohandler_attach(QContext *ctx)
{
    GSource *gs = g_source_new(&iohandler_source_funcs,
                               sizeof(IOHandlerSource));
    IOHandlerSource *s = (IOHandlerSource *)gs;

    QLIST_INIT(&s->io_handlers);
    qemu_mutex_init_recursive(&s->mutex);

    qcontext_attach_source(ctx, gs, SOURCE_TYPE_IOHANDLER);
}

int set_fd_handler2(QContext *ctx,
                    int fd,
                    IOCanReadHandler *fd_read_poll,
                    IOHandler *fd_read,
                    IOHandler *fd_write,
                    void *opaque)
{
    GSource *source;
    IOHandlerSource *s;
    IOHandlerRecord *ioh;

    source = qcontext_find_source_by_name(ctx, SOURCE_TYPE_IOHANDLER);
    if (!source) {
        assert(0);
    }
    s = (IOHandlerSource *)source;

    assert(fd >= 0);

    qemu_mutex_lock(&s->mutex);

    if (!fd_read && !fd_write) {
        QLIST_FOREACH(ioh, &s->io_handlers, next) {
            if (ioh->fd == fd) {
                ioh->deleted = 1;
                break;
            }
        }
    } else {
        QLIST_FOREACH(ioh, &s->io_handlers, next) {
            if (ioh->fd == fd)
                goto found;
        }
        ioh = g_malloc0(sizeof(IOHandlerRecord));
        QLIST_INSERT_HEAD(&s->io_handlers, ioh, next);
    found:
        ioh->fd = fd;
        ioh->fd_read_poll = fd_read_poll;
        ioh->fd_read = fd_read;
        ioh->fd_write = fd_write;
        ioh->opaque = opaque;
        ioh->pollfds_idx = -1;
        ioh->deleted = 0;

        g_main_context_wakeup(qcontext_get_context(ctx));
    }

    qemu_mutex_unlock(&s->mutex);

    return 0;
}

int set_fd_handler(QContext *ctx,
                   int fd,
                   IOHandler *fd_read,
                   IOHandler *fd_write,
                   void *opaque)
{
    return set_fd_handler2(ctx, fd, NULL, fd_read, fd_write, opaque);
}

/* XXX: fd_read_poll should be suppressed, but an API change is
   necessary in the character devices to suppress fd_can_read(). */
int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    return set_fd_handler2(qemu_get_qcontext(), fd,
                           fd_read_poll, fd_read, fd_write,
                           opaque);
}

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
