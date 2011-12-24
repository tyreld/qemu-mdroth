#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#include <qga/transport.h>
#include <windows.h>
#include <errno.h>
#include <io.h>

struct GAChannel {
    int fd;
    GAChannelCallback cb;
    gpointer user_data;
};

typedef struct GAWatch {
    GSource source;
    GPollFD pollfd;
    GAChannel *channel;
    GIOCondition condition;
} GAWatch;

static gboolean ga_channel_prepare(GSource *source, gint *timeout)
{
    g_debug("prepare");
    *timeout = -1;
    return false;
}

static gboolean ga_channel_check(GSource *source)
{
    GAWatch *watch = (GAWatch *)source;

    g_debug("check");
    return watch->pollfd.revents & watch->condition;
}

static gboolean ga_channel_dispatch(GSource *source, GSourceFunc unused,
                                     gpointer user_data)
{
    GAWatch *watch = (GAWatch *)source;
    GAChannel *c = (GAChannel *)watch->channel;
    g_debug("dispatch");
    return c->cb(watch->pollfd.revents, c->user_data);
}

static void ga_channel_finalize(GSource *source)
{
}

GSourceFuncs ga_channel_watch_funcs = {
    ga_channel_prepare,
    ga_channel_check,
    ga_channel_dispatch,
    ga_channel_finalize
};

static GSource *ga_channel_create_watch(GAChannel *c, GIOCondition condition)
{
    GSource *source = g_source_new(&ga_channel_watch_funcs, sizeof(GAWatch));
    GAWatch *watch = (GAWatch *)source;
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

GAChannel *ga_channel_new(int fd, GIOCondition condition, GAChannelCallback cb, gpointer user_data)
{
    GSource *source;
    GAChannel *c = g_malloc0(sizeof(GAChannel));

    c->cb = cb;
    c->fd = fd;
    c->user_data = user_data;
    source = ga_channel_create_watch(c, condition);
    g_source_attach(source, NULL);
    return c;
}

GIOStatus ga_channel_read(GAChannel *c, char *buf, int size, gsize *count)
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

GIOStatus ga_channel_write_all(GAChannel *c, const char *buf, int size,
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
