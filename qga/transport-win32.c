#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#include <qga/transport.h>
#include <windows.h>
#include <errno.h>
#include <io.h>

struct QgaChannel {
    int fd;
    QgaChannelCallback cb;
    gpointer user_data;
};

typedef struct QgaWatch {
    GSource source;
    GPollFD pollfd;
    QgaChannel *channel;
    GIOCondition condition;
} QgaWatch;

static gboolean qga_channel_prepare(GSource *source, gint *timeout)
{
    g_debug("prepare");
    *timeout = -1;
    return false;
}

static gboolean qga_channel_check(GSource *source)
{
    QgaWatch *watch = (QgaWatch *)source;

    g_debug("check");
    return watch->pollfd.revents & watch->condition;
}

static gboolean qga_channel_dispatch(GSource *source, GSourceFunc unused,
                                     gpointer user_data)
{
    QgaWatch *watch = (QgaWatch *)source;
    QgaChannel *c = (QgaChannel *)watch->channel;
    g_debug("dispatch");
    return c->cb(watch->pollfd.revents, c->user_data);
}

static void qga_channel_finalize(GSource *source)
{
}

GSourceFuncs qga_channel_watch_funcs = {
    qga_channel_prepare,
    qga_channel_check,
    qga_channel_dispatch,
    qga_channel_finalize
};

static GSource *qga_channel_create_watch(QgaChannel *c, GIOCondition condition)
{
    GSource *source = g_source_new(&qga_channel_watch_funcs, sizeof(QgaWatch));
    QgaWatch *watch = (QgaWatch *)source;
    //SECURITY_ATTRIBUTES sec_attrs;

    watch->channel = c;
    watch->condition = condition;
    watch->pollfd.fd = (gintptr)_get_osfhandle(c->fd);
    watch->pollfd.events = condition;
    g_source_add_poll(source, &watch->pollfd);

    /* we don't use this, but need to initialize it to pass event to child */
    /*
    sec_attrs.nLength = sizeof(SECURITY_ATTRIBUTES);
    sec_attrs.lpSecurityDescriptor = NULL;
    sec_attrs.bInheritHandle = FALSE;

    watch->event_read_completion = CreateEvent(&sec_attrs, TRUE, FALSE);
    watch->event_write_completion = CreateEvent(&sec_attrs, TRUE, FALSE);
    */
    return source;
}

QgaChannel *qga_channel_new(int fd, GIOCondition condition, QgaChannelCallback cb, gpointer user_data)
{
    GSource *source;
    QgaChannel *c = g_malloc0(sizeof(QgaChannel));

    c->cb = cb;
    c->fd = fd;
    c->user_data = user_data;
    source = qga_channel_create_watch(c, condition);
    g_source_attach(source, NULL);
    return c;
}

GIOStatus qga_channel_read(QgaChannel *c, char *buf, int size, gsize *count)
{
    GIOStatus status;
    *count = read(c->fd, buf, size);

    if (*count == 0) {
        status = G_IO_STATUS_EOF;
    } else if (*count == -1) {
        if (errno == EAGAIN || errno == EINTR) {
            status = G_IO_STATUS_AGAIN;
        } else {
            status = G_IO_STATUS_ERROR;
            g_warning("error: %s", strerror(errno));
        }
    } else {
        status = G_IO_STATUS_NORMAL;
    }

    return status;
}

GIOStatus qga_channel_write_all(QgaChannel *c, const char *buf, int size,
                                gsize *count)
{
    GIOStatus status = G_IO_STATUS_NORMAL;
    int written = 0, ret;

    while (written != size) {
        ret = write(c->fd, buf, size);
        if (ret == 0) {
            status = G_IO_STATUS_EOF;
            break;
        } else if (ret == -1) {
            if (errno != EAGAIN || errno != EINTR) {
                status = G_IO_STATUS_ERROR;
                break;
            }
        }
        written += ret;
        size  -= ret;
        buf += ret;
    }
    if (count) {
        *count = written;
    }
    return status;
}
