#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#include <windows.h>
#include <errno.h>
#include <io.h>
#include "qga/guest-agent-core.h"
#include "qga/transport.h"

typedef struct GAChannelReadState {
    HANDLE data_avail_event;
    HANDLE space_avail_event;
    guint thread_id;
    CRITICAL_SECTION mutex;
    /* below fields require lock for r/w */
    uint8_t *buf;
    size_t buf_size;
    size_t cur; /* current buffer start */
    size_t pending; /* pending buffered bytes to read */
    GIOCondition pending_events;
} GAChannelReadState;

struct GAChannel {
    HANDLE handle;
    GAChannelCallback cb;
    gpointer user_data;
    GAChannelReadState rstate;
    bool running;
};

typedef struct GAWatch {
    GSource source;
    GPollFD pollfd;
    GAChannel *channel;
    GIOCondition event_mask;
} GAWatch;

static gboolean ga_channel_prepare(GSource *source, gint *timeout)
{
    //GAWatch *watch = (GAWatch *)source;
    //GAChannel *c = (GAChannel *)watch->channel;

    g_debug("prepare");
    *timeout = -1;

    return false;
}

static gboolean ga_channel_check(GSource *source)
{
    GAWatch *watch = (GAWatch *)source;
    GAChannel *c = (GAChannel *)watch->channel;
    GAChannelReadState *rs = &c->rstate;

    g_debug("check");
    EnterCriticalSection(&rs->mutex);
    watch->pollfd.revents = rs->pending_events & watch->pollfd.events;
g_debug("check: got events: %d", (int)watch->pollfd.revents);
    if ((rs->pending_events & G_IO_IN) == 0) {
        rs->pending_events = 0;
        ResetEvent(rs->data_avail_event);
    }
    LeaveCriticalSection(&rs->mutex);
    return watch->pollfd.revents & watch->event_mask;
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

    watch->channel = c;
    watch->event_mask = condition;
    watch->pollfd.fd = (gintptr) c->rstate.data_avail_event;
    //watch->pollfd.fd = (gintptr)_get_osfhandle(c->fd);
    watch->pollfd.events = condition;
    g_source_add_poll(source, &watch->pollfd);

    return source;
}

static unsigned __stdcall ga_channel_read_thread(void *param)
{
    GAChannel *c = param;
    GAChannelReadState *rs = &c->rstate;
    DWORD count_read, count_to_read;
    bool success;
    GIOCondition new_events;
    uint8_t *buf = g_malloc(rs->buf_size);
    OVERLAPPED ov = {0};
    DWORD error;

    //ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_debug("reader thread starting...");

    while (c->running) {
g_debug("thread: looping");
        EnterCriticalSection(&rs->mutex);
        /* TODO: use a ring buffer instead */
        while (rs->cur + rs->pending == rs->buf_size) {
            if (rs->cur > 0) {
                memmove(rs->buf, rs->buf + rs->cur, rs->pending);
                rs->cur = 0;
            } else {
                LeaveCriticalSection(&rs->mutex);
g_debug("thread: waiting for space_avail_event"); 
                WaitForSingleObject(rs->space_avail_event, INFINITE);
                EnterCriticalSection(&rs->mutex);
            }
        }
        count_read = 0;
        new_events = 0;
        /* note: we are the only producer, so the amount of buffer space
         * available can only increase after we release the lock
         */
        count_to_read = rs->buf_size - (rs->pending + rs->cur);
        //count_to_read = 1;
        //ov.
retry_read:
        LeaveCriticalSection(&rs->mutex);
g_debug("thread: starting read"); 
        success = ReadFile(c->handle, buf, count_to_read, &count_read, &ov);
        error = success ? 0 : GetLastError();
        //ret = ReadFile(c->handle, buf, count_to_read, &count_read, NULL);
/*
        ret = ReadFile(c->handle, rs->buf + rs->cur + rs->pending, count_to_read,
                       &count_read, NULL);
                       */
        EnterCriticalSection(&rs->mutex);
g_debug("thread: done reading, success: %d, count_to_read: %d, count_read: %d",
        success, (int)count_to_read, (int)count_read);

        if (success) {
g_debug("readfile success");
            /* read completed immediately */
            memcpy(rs->buf + rs->cur + rs->pending, buf, count_read);
            rs->pending += count_read;
            new_events |= G_IO_IN;
        } else {
g_debug("readfile fail");
            if (error == ERROR_IO_PENDING) {
g_debug("readfile fail: io pending");
                /* read pending */
                LeaveCriticalSection(&rs->mutex);
                success = GetOverlappedResult(c->handle, &ov, &count_read, TRUE);
                EnterCriticalSection(&rs->mutex);
                if (success) {
g_debug("thread: overlapped result, count_read: %d", (int)count_read); 
                    memcpy(rs->buf + rs->cur + rs->pending, buf, count_read);
                    rs->pending += count_read;
                    new_events |= G_IO_IN;
                } else {
                    error = GetLastError();
                    if (error == 0 || error == ERROR_HANDLE_EOF) {
                        new_events |= G_IO_HUP;
                    } else if (error == ERROR_NO_SYSTEM_RESOURCES) {
                        g_debug("thread: no system resources, retrying...");
                        usleep(10*1000);
                        goto retry_read;
                    } else {
                        g_critical("error retrieving overlapped result: %d",
                                   (int)error);
                        c->running = 0;
                        new_events |= G_IO_ERR;
                    }
                }
                //ResetEvent(ov.hEvent);
            } else if (!error) {
g_debug("readfile fail: getlasterror() == 0");
                new_events |= G_IO_HUP;
                usleep(1000*1000);
            } else {
g_debug("readfile fail: getlasterror() == %d", (int)GetLastError());
                /* read error */
                g_critical("error reading channel: %d", (int)GetLastError());
                //c->running = 0;
                //new_events |= G_IO_ERR;
            }
        }
#if 0
        if (!ret) {
            if (!GetLastError() || GetLastError() == ERROR_HANDLE_EOF) {
                if (GetLastError() == ERROR_HANDLE_EOF) {
                    g_debug("thread: got EOF");
                }
                new_events |= G_IO_HUP;
            } else {
                new_events |=  G_IO_ERR;
            }
        } else if (count_read) {
            memcpy(rs->buf + rs->cur + rs->pending, buf, count_read);
            rs->pending += count_read;
            new_events |= G_IO_IN;
        }
#endif

        /* pending events are reset upon processing, so only notify if we add
         * new events
         */
        if (new_events != rs->pending_events) {
            rs->pending_events |= new_events;
            LeaveCriticalSection(&rs->mutex);
g_debug("thread: setting data_avail_event");
            SetEvent(rs->data_avail_event);
        } else {
            LeaveCriticalSection(&rs->mutex);
g_debug("thread: sleeping");
            usleep(10*1000);
            //usleep(1000*1000);
            SetEvent(rs->data_avail_event);
        }
    }
    g_free(buf);
    g_debug("reader thread exiting...");

    return 0;
}

GIOStatus ga_channel_read(GAChannel *c, char *buf, size_t size, gsize *count)
{
    GAChannelReadState *rs = &c->rstate;
    GIOStatus status;
    size_t to_read = 0;

g_debug("marker 0");
    EnterCriticalSection(&rs->mutex);
g_debug("marker 1");
    *count = to_read = MIN(size, rs->pending);
g_debug("marker 2");
    if (to_read) {
g_debug("marker 3");
        memcpy(buf, rs->buf + rs->cur, to_read);
        rs->cur += to_read;
        rs->pending -= to_read;
        SetEvent(&rs->space_avail_event);
        status = G_IO_STATUS_NORMAL;
    } else {
g_debug("marker 4");
        status = G_IO_STATUS_AGAIN;
    }
    if (rs->pending == 0) {
g_debug("marker 5");
        rs->pending_events &= ~G_IO_IN;
        ResetEvent(rs->data_avail_event);
    }
g_debug("marker 6");
    LeaveCriticalSection(&rs->mutex);
g_debug("marker 7");

    return status;
}

/*
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
*/

/*
static gboolean ga_channel_idle_cb(gpointer user_data)
{
    GAChannel *c = user_data;
    gboolean ret;
    ret = c->cb(c->pending_events, c);
    c->pending_events = 0;
    return ret;
}
*/

GAChannel *ga_channel_new(GAHandle handle, GIOCondition condition, GAChannelCallback cb,
                          gpointer user_data)
{
    GSource *source;
    GAChannel *c = g_malloc0(sizeof(GAChannel));
    SECURITY_ATTRIBUTES sec_attrs;
    HANDLE read_thread_handle;
    COMMTIMEOUTS timeout;

    c->cb = cb;
    /* TODO: shouldn't need both */
    /*
    c->fd = fd;
    c->handle = (HANDLE) _get_osfhandle(fd);
    */
    c->handle = handle;
    c->user_data = user_data;
    c->running = true;
    InitializeCriticalSection(&c->rstate.mutex);

    /* we don't use this, but need to initialize it to pass event to child */
    sec_attrs.nLength = sizeof(SECURITY_ATTRIBUTES);
    sec_attrs.lpSecurityDescriptor = NULL;
    sec_attrs.bInheritHandle = false;

    c->rstate.buf_size = QGA_READ_COUNT_DEFAULT;
    c->rstate.buf = g_malloc(QGA_READ_COUNT_DEFAULT);
    c->rstate.data_avail_event = CreateEvent(&sec_attrs, TRUE, FALSE, NULL);
    c->rstate.space_avail_event = CreateEvent(&sec_attrs, FALSE, FALSE, NULL);

    source = ga_channel_create_watch(c, condition);
    g_source_attach(source, NULL);

    /* set non-blocking reads, blocking writes */
    /*
    timeout.ReadIntervalTimeout = MAXDWORD;
    timeout.ReadTotalTimeoutMultiplier = 0;
    timeout.ReadTotalTimeoutConstant = 0;
    timeout.WriteTotalTimeoutMultiplier = 0;
    timeout.WriteTotalTimeoutConstant = 0;
    */


    /* set blocking reads with timeout, blocking writes */
    timeout.ReadIntervalTimeout = MAXDWORD;
    timeout.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeout.ReadTotalTimeoutConstant = 0;
    timeout.WriteTotalTimeoutMultiplier = 0;
    timeout.WriteTotalTimeoutConstant = 0;
    //SetCommTimeouts(c->handle, &timeout);

    read_thread_handle = (HANDLE)_beginthreadex(NULL, 0, ga_channel_read_thread,
                                                c, 0, &c->rstate.thread_id);
    if (read_thread_handle == 0) {
        g_warning("error creating reader thread for channel");
        g_free(c);
        return NULL;
    } else if (!CloseHandle(read_thread_handle)) {
        g_warning("error closing thread handle");
        g_free(c);
        return NULL;
    }

    return c;
}

void ga_channel_close(GAChannel *c)
{
    DeleteCriticalSection(&c->rstate.mutex);
}

static GIOStatus ga_channel_write(GAChannel *c, const char *buf, size_t size, size_t *count)
{
    GIOStatus status;
    OVERLAPPED ov = {0};
    BOOL ret;
    DWORD written;

    ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    ret = WriteFile(c->handle, buf, size, &written, &ov);
    if (!ret) {
        if (GetLastError() == ERROR_IO_PENDING) {
            /* write is pending */
            ret = GetOverlappedResult(c->handle, &ov, &written, TRUE);
            if (!ret) {
                if (!GetLastError()) {
                    status = G_IO_STATUS_AGAIN;
                } else {
                    status = G_IO_STATUS_ERROR;
                }
            } else {
                /* write is complete */
                status = G_IO_STATUS_NORMAL;
                *count = written;
            }
        } else {
            status = G_IO_STATUS_ERROR;
        }
    } else {
        /* write returned immediately */
        status = G_IO_STATUS_NORMAL;
        *count = written;
    }

    return status;
}

GIOStatus ga_channel_write_all(GAChannel *c, const char *buf, size_t size)
{
    GIOStatus status = G_IO_STATUS_NORMAL;;
    size_t count;

    while (size) {
        status = ga_channel_write(c, buf, size, &count);
        if (status == G_IO_STATUS_NORMAL) {
            size -= count;
            buf += count;
        } else if (status != G_IO_STATUS_AGAIN) {
            break;
        }
    }

    return status;
}

/*
GIOStatus ga_channel_write_all(GAChannel *c, const char *buf, int size,
                                gsize *count)
{
    GIOStatus status = G_IO_STATUS_NORMAL;
    int written = 0, ret;

    while (written != size) {
g_debug("calling write()...");
        //ret = write(c->fd, buf, size);
        ret = 0;
g_debug("returning from write()...");
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
*/
