/*
 * Dedicated threads for virtio-net I/O processing
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth   <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <sys/epoll.h>
#include "hw/virtio-net.h"
#include "hw/dataplane/virtio-net.h"
#include "net/net.h"
#include "net/tap.h"
#include "net/checksum.h"
#include "qemu/iov.h"
#include "event-poll.h"
#include "qemu/thread.h"
#include "vring.h"
#include "trace.h"

//#define DEBUG_VIRTIO_NET_DATAPLANE 1
//#define DEBUG_VIRTIO_NET_DATAPLANE_VERBOSE 1

#ifdef DEBUG_VIRTIO_NET_DATAPLANE
#define dprintf(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define dprintf(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_VIRTIO_NET_DATAPLANE_VERBOSE
#define ddprintf(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define ddprintf(fmt, ...) \
    do { } while (0)
#endif

#define TX_BUFFERING
#define TX_SEND_BUF_SZ (128 << 10)

typedef struct VirtIONetDataPlaneState {
    QemuThread thread;
    EventPoll event_poll;            /* event poller */
    EventNotifier *guest_notifier;   /* irq */
    EventNotifier fd_notifier;      /* tap notifier */
    EventHandler notify_handler;     /* virtqueue notify handler */
    EventHandler fd_handler;         /* tap fd notify handler */
    VirtQueue *vq;
    Vring vring;                     /* virtqueue vring */

    /* TX */
    uint32_t timeout;
    uint32_t previous_timeout;
    uint32_t previous_batch;
#ifdef TX_BUFFERING
    uint8_t *sendbuf;
    size_t sendbuf_size;
    size_t sendbuf_offset;
#endif
} VirtIONetDataPlaneState; 

struct VirtIONetDataPlane {
    VirtIODevice *vdev;
    bool started;
    QEMUBH *start_bh;
    int fd;                          /* tap file descriptor */
    VirtIONetDataPlaneState rx;
    VirtIONetDataPlaneState tx;
    bool has_vnet_hdr;
    bool mergeable_rx_bufs;
};

enum {
    VIRTIO_NET_VRING_MAX = 256,
};

/* Raise an interrupt to signal guest, if necessary */
static void notify_guest_tx(VirtIONetDataPlane *s)
{
    ddprintf("tx notify");
    if (!vring_should_notify(s->vdev, &s->tx.vring)) {
        ddprintf("tx notify suppressed");
        return;
    }

    event_notifier_set(s->tx.guest_notifier);
}

/* Raise an interrupt to signal guest, if necessary */
static void notify_guest_rx(VirtIONetDataPlane *s)
{
    ddprintf("rx notify");
    if (!vring_should_notify(s->vdev, &s->rx.vring)) {
        ddprintf("rx notify suppressed");
        return;
    }

    event_notifier_set(s->rx.guest_notifier);
}

#if 0
#endif

#if 0
#ifdef DEBUG_VIRTIO_NET_DATAPLANE
static void print_header(struct virtio_net_hdr_mrg_rxbuf *hdr2, bool rx)
{
    struct virtio_net_hdr *hdr = &hdr2->hdr;
    dprintf("== %s header ==", rx ? "rx" : "tx");
    dprintf("hdr->flags: %x", hdr->flags);
    dprintf("hdr->gso_type: %x", hdr->gso_type);
    dprintf("hdr->gso_size: %x", hdr->gso_size);
    dprintf("hdr->csum_start: %x", hdr->csum_start);
    dprintf("hdr->csum_offset: %x", hdr->csum_offset);
    dprintf("hdr->num_buffers: %x", hdr2->num_buffers);
}
#else
#define print_header(...)
#endif
#endif

#ifdef TX_BUFFERING

static int sum_iov_len(const struct iovec *iov, int iovcnt)
{
    int sum = 0, i;

    for (i = 0; i < iovcnt; i++) {
        sum += iov[i].iov_len;
    }
    return sum;
}

static ssize_t sendbuf_flush(VirtIONetDataPlane *s)
{
    size_t count = s->tx.sendbuf_offset;
    size_t written = 0;
    ssize_t ret;

    while (written < count) {
        do {
            ret = write(s->fd, s->tx.sendbuf + written, count - written);
        } while (ret == -1 && (errno == EAGAIN || errno == EINTR));
        if (ret == -1) {
            error_report("tap write error: %s", strerror(errno));
            break;
        }
        written += ret;
    }

    s->tx.sendbuf_offset = 0;
    return written;
}

static ssize_t sendbuf_avail_bytes(VirtIONetDataPlane *s)
{
    return s->tx.sendbuf_size - s->tx.sendbuf_offset;
}

static ssize_t sendbuf_append(VirtIONetDataPlane *s, const struct iovec *iov, int iovcnt)
{
    ssize_t ret;
    VirtIONetDataPlaneState *tx = &s->tx;

    /* for simplicity we only buffer complete descriptor chains */
    if (sendbuf_avail_bytes(s) < sum_iov_len(iov, iovcnt)) {
        return -EAGAIN;
    }

    ret = iov_to_buf(iov, iovcnt, 0, tx->sendbuf + tx->sendbuf_offset,
                     tx->sendbuf_size - tx->sendbuf_offset);
    s->tx.sendbuf_offset += ret;
    return ret;
}

#endif

static int32_t handle_tx_flush(VirtIONetDataPlane *s, int *count)
{
    int32_t bytes_written = 0, ret;
    int ret_errno = 0;
    struct iovec iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *end = &iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *iov = iovec;
    int head, i = 0;
    unsigned int out_num = 0, in_num = 0;

    ddprintf("handle_tx_flush");
    trace_virtio_net_data_plane_tx_flush(s);

    while (1) {
        iov = iovec;
        head = vring_pop(s->vdev, &s->tx.vring, iov, end, &out_num, &in_num);
        if (head < 0 || out_num <= 0) {
            /* no more input buffers */
            break;
        }
        do {
#ifdef TX_BUFFERING
            ret = sendbuf_append(s, iov, out_num);
            if (ret == -EAGAIN) {
                ret = sendbuf_flush(s);
                assert(ret > 0);
                ret = sendbuf_append(s, iov, out_num);
                assert(ret > 0);
            }
#else
            trace_virtio_net_data_plane_tx_write(s, s->fd, out_num);
            ret = writev(s->fd, iov, out_num);
            ret_errno = errno;
            trace_virtio_net_data_plane_tx_write_complete(s, s->fd, ret);
#endif
        } while (ret == -1 && (ret_errno == EINTR || ret_errno == EAGAIN));
        if (ret == -1) {
            error_report("tap writev error: %s", strerror(errno));
            break;
        }
        ddprintf("wrote %d bytes", ret);
        //struct virtio_net_hdr *hdr = iov->iov_base;
        //print_header(iov->iov_base, false);
        bytes_written += ret;
#if 0
        int sum = sum_iov_len(iov, out_num);
        if (ret < sum) {
            /* TODO: fix this, obviously */
            error_report("incomplete write, data lost (%d < %d)", ret, sum);
        }
#endif
        vring_push(&s->tx.vring, head, ret);
        //iov += out_num + in_num;
        i++;
    }

    if (i > 0) {
        notify_guest_tx(s);
#ifdef TX_BUFFERING
        ret = sendbuf_flush(s);
        assert(ret >= 0);
#endif
    }

    *count = i;
    ddprintf("flushed %d bytes", bytes_written);
    trace_virtio_net_data_plane_tx_flush_complete(s, *count, bytes_written);

    return bytes_written;
}

static void handle_tx_kick(EventHandler *handler)
{
    VirtIONetDataPlaneState *tx = container_of(handler, VirtIONetDataPlaneState,
                                               notify_handler);
    VirtIONetDataPlane *s = container_of(tx, VirtIONetDataPlane, tx);

    int count;
    int32_t bytes_written;
    int tx_spin_count = 0;

    ddprintf("handle_tx_kick");

restart:
    vring_disable_notification(s->vdev, &s->tx.vring);

//#define TX_SPIN_COUNT_MAX 10000
#define TX_SPIN_COUNT_MAX -1 //inf
    while (TX_SPIN_COUNT_MAX == -1 || (tx_spin_count < TX_SPIN_COUNT_MAX)) {
        bytes_written = handle_tx_flush(s, &count);
        if (bytes_written <= 0) {
            tx_spin_count++;
        } else {
            tx_spin_count = 0;
        }
    }

    vring_enable_notification(s->vdev, &s->tx.vring);
    /* catch anything we missed while notifications suppressed.
     * restart loop if we get more
     */ 
    bytes_written = handle_tx_flush(s, &count);
    if (bytes_written > 0) {
        tx_spin_count = 0;
        goto restart;
    }
}

static void set_header(VirtIONetDataPlane *s, struct iovec *iov,
                       int iovcnt, size_t hdr_len)
{
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)iov[0].iov_base;

    hdr->flags = 0;
    hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

    iov[0].iov_base += hdr_len;
    iov[0].iov_len -= hdr_len;
}

#if 0
static uint64_t get_delta(struct timeval t1, struct timeval t2)
{
    uint64_t usec = (t2.tv_sec - t1.tv_sec) * 1000 * 1000 + (t2.tv_usec - t1.tv_usec);
    return usec;
}
#endif

//#define VIRTIO_RX_BUF_SIZE (sizeof(struct virtio_net_hdr_mrg_rxbuf) + (64 << 10))
#define VIRTIO_RX_BUF_SIZE (4096 + 65536)
#define STAT_INTERVAL 10000

static void handle_rx_mrg_rxbuf(VirtIONetDataPlaneState *rx)
{
    VirtIONetDataPlane *s = container_of(rx, VirtIONetDataPlane, rx);
    struct iovec iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *end = &iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *iov = iovec;
    struct virtio_net_hdr_mrg_rxbuf *hdr = NULL;
    int head[VIRTIO_NET_VRING_MAX];
    int len[VIRTIO_NET_VRING_MAX];
    static unsigned int out_num = 0, in_num = 0;
    uint8_t buf[VIRTIO_RX_BUF_SIZE];
    int i;

    vring_disable_notification(s->vdev, &s->rx.vring);
    while (1) {
        ssize_t count, offset, guest_offset, ret;

        count = read(s->fd, buf, VIRTIO_RX_BUF_SIZE);
        if (count == -1) {
            if (errno != EAGAIN) {
                dprintf("tap read error: %s", strerror(errno));
            }
            continue;
        }

        i = 0;
        offset = 0;

        while (count > offset) {
            iov = iovec;
            head[i] = vring_pop(s->vdev, &s->rx.vring, iov, end, &out_num, &in_num);
            if (head[i] < 0) {
                continue;
            }
            iov += out_num;
            if (i == 0) {
                hdr = iov->iov_base;
                memcpy(hdr, buf, sizeof(struct virtio_net_hdr));
                guest_offset = offset = sizeof(struct virtio_net_hdr_mrg_rxbuf);
            } else {
                guest_offset = 0;
            }
            ret = iov_from_buf(iov, in_num, guest_offset,
                               buf + offset, count - offset);
            len[i] = guest_offset + ret;
            //len[i] = offset + ret;
            offset += ret;
            dprintf("count: %zd, offset: %zd, i: %d, len[i]: %d, head[i]: %d",
                    count, offset, i, len[i], head[i]);
            i++;
        }
        assert(hdr);
        assert(i > 0);
        stw_p(&hdr->num_buffers, i);
        //dprintf("pushing %d buffers", hdr->num_buffers);
        vring_push_multiple(&s->rx.vring, head, len, i);
        notify_guest_rx(s);
    }
    vring_enable_notification(s->vdev, &s->rx.vring);
}

static void handle_rx(EventHandler *handler)
{
    VirtIONetDataPlaneState *rx = container_of(handler, VirtIONetDataPlaneState,
                                               fd_handler);
    VirtIONetDataPlane *s = container_of(rx, VirtIONetDataPlane, rx);
    ssize_t ret;
    struct iovec iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *end = &iovec[VIRTIO_NET_VRING_MAX];
    struct iovec *iov = iovec;
    static int head = -1;
    static unsigned int out_num = 0, in_num = 0;
    bool sent = false;
    static bool deferred = false;
    int rx_spin_count = 0;
    int rx_notify_coalesced = 0;

    ddprintf("handle_rx");

    dprintf("s->mergeable_rx_bufs: %d", s->mergeable_rx_bufs);
    if (s->mergeable_rx_bufs) {
        dprintf("marker 0");
        return handle_rx_mrg_rxbuf(rx);
    }

    vring_disable_notification(s->vdev, &s->rx.vring);
    for (;;) {
        iov = iovec;
        if (!deferred) {
            head = vring_pop(s->vdev, &s->rx.vring, iov, end, &out_num, &in_num);
        }
        if (head < 0) {
            if (sent) {
                notify_guest_rx(s);
                sent = false;
            }
            deferred = false;
            continue;
#if 0
            /* no more input buffers, stop polling and wait for kick */
            dprintf("no more buffers, waiting for kick (head; %d)", head);
            vring_enable_notification(s->vdev, &s->rx.vring);
            /* one last check for buffers that may have been added while
             * notifications were suppressed
             */
            head = vring_pop(s->vdev, &s->rx.vring, iov, end, &out_num, &in_num);
            dprintf("head: %d, in_num: %d, out_num: %d", head, in_num, out_num);
            if (head < 0) {
                /* ignore fd events until we get a kick, we can't do anything
                 * until we get more buffers */
                event_poll_mod(&s->rx.event_poll, &s->rx.fd_handler,
                               &s->rx.fd_notifier, handle_rx, 0);
                break;
            }
            dprintf("got more buffers on final check, re-suppressing notifications");
            vring_disable_notification(s->vdev, &s->rx.vring);
#endif
        }

        iov += out_num;
        if (!s->has_vnet_hdr) {
            assert(0); /* untested */
            set_header(s, iov, in_num, sizeof(struct virtio_net_hdr));
        }

        do {
            ret = readv(s->fd, iov, in_num);
#ifdef DEBUG_VIRTIO_NET_DATAPLANE
            int max_size = sum_iov_len(iov, in_num);
            if (ret != -1) {
                dprintf("max size read: %d, ret: %zd", max_size, ret);
            }
#endif
        } while (ret == -1 && errno == EINTR);
        if (ret == -1) {
            if (errno != EAGAIN) {
                dprintf("tap read error: %s", strerror(errno));
            }
            /* wait for more packets, leave notifications suppressed in the
             * meantime since we can't do anything about them
             */
            deferred = true;

//#define RX_SPIN_MAX 1000
#define RX_SPIN_MAX -1 //inf
            if (RX_SPIN_MAX != -1) {
                if (rx_spin_count > RX_SPIN_MAX) {
                    rx_spin_count = 0;
                    break;
                }
                rx_spin_count++;
            }
            if (sent) {
                notify_guest_rx(s);
                sent = false;
            }
            continue;
        }
        if (ret == 0) {
            dprintf("read 0 bytes");
        }
        ddprintf("read %d bytes", (int)ret);
#if 0
        if (s->has_vnet_hdr) {
            work_around_broken_dhclient(iov, ret);
        }
#endif
        if (!s->has_vnet_hdr) {
            vring_push(&s->rx.vring, head, ret + sizeof(struct virtio_net_hdr));
        } else {
            vring_push(&s->rx.vring, head, ret);
        }
        sent = true;
        deferred = false;
//#define RX_NOTIFY_COALESCE_MAX 10
#define RX_NOTIFY_COALESCE_MAX 0
        if (rx_notify_coalesced > RX_NOTIFY_COALESCE_MAX) {
            rx_notify_coalesced = 0;
            notify_guest_rx(s);
            sent = false;
        } else {
            rx_notify_coalesced++;
        }
    }
    if (sent) {
        notify_guest_rx(s);
    }
}

/* basically unused currently */
static void handle_rx_kick(EventHandler *handler)
{
    VirtIONetDataPlaneState *rx = container_of(handler, VirtIONetDataPlaneState,
                                               notify_handler);
    VirtIONetDataPlane *s = container_of(rx, VirtIONetDataPlane, rx);

    /* NOTE: handle_rx() will re-remove event from loop if we run
     * out of buffers again
     */
    dprintf("handle_rx_kick");
    event_poll_mod(&s->rx.event_poll, &s->rx.fd_handler,
                   &s->rx.fd_notifier, handle_rx, EPOLLIN);
}

static void *data_plane_thread_tx(void *opaque)
{
    VirtIONetDataPlane *s = opaque;
    dprintf("running tx event loop");
    vring_enable_notification(s->vdev, &s->tx.vring);
    do {
        event_poll(&s->tx.event_poll);
    } while (s->started);
    return NULL;
}

static void *data_plane_thread_rx(void *opaque)
{
    VirtIONetDataPlane *s = opaque;
    dprintf("running rx event loop");
    vring_enable_notification(s->vdev, &s->rx.vring);
    do {
        event_poll(&s->rx.event_poll);
    } while (s->started);
    return NULL;
}

static void start_data_plane_bh(void *opaque)
{
    VirtIONetDataPlane *s = opaque;
#if 0
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
#endif

    dprintf("starting threads");

    qemu_bh_delete(s->start_bh);
    s->start_bh = NULL;
    qemu_thread_create(&s->rx.thread, data_plane_thread_rx,
                       s, QEMU_THREAD_JOINABLE);
    qemu_thread_create(&s->tx.thread, data_plane_thread_tx,
                       s, QEMU_THREAD_JOINABLE);

    /* yes this is terrible. just for testing since cpu migration is making
     * benchmarks inconsistent  */
#if 0
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(s->rx.thread.thread, sizeof(cpuset), &cpuset);

    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(s->tx.thread.thread, sizeof(cpuset), &cpuset);
#endif
}

void virtio_net_data_plane_start(VirtIONetDataPlane *s)
{
    if (s->started) {
        return;
    }

    dprintf("start");
    trace_virtio_net_data_plane_start(s);

    /* take over handling of the tap fd from qemu's main loop */
    /* TODO: may have queued packets here, transition requires forcing a
     * flush somehow (or never running in non-dataplane mode */

    /* enable guest notifiers (irq) */
    if (s->vdev->binding->set_guest_notifiers(s->vdev->binding_opaque,
                                              true) != 0) {
        fprintf(stderr, "virtio-net failed to set guest notifier, "
                "ensure -enable-kvm is set\n");
        exit(1);
    }

    /* RX */

    s->rx.vq = virtio_get_queue(s->vdev, 0);
    s->rx.guest_notifier = virtio_queue_get_guest_notifier(s->rx.vq);

    /* set up vring */
    if (!vring_setup(&s->rx.vring, s->vdev, 0)) {
        fprintf(stderr, "virtio-net failed to configure rx vring");
        return;
    }

    /* set up virtqueue rx notify handler */
    event_poll_init(&s->rx.event_poll);
    if (s->vdev->binding->set_host_notifier(s->vdev->binding_opaque,
                                            0, true) != 0) {
        fprintf(stderr, "virtio-net failed to set host notifier\n");
        exit(1);
    }
    event_poll_add(&s->rx.event_poll, &s->rx.notify_handler,
                   virtio_queue_get_host_notifier(s->rx.vq),
                   handle_rx_kick, true);

    /* set up tap fd notify handler */
    event_notifier_init_fd(&s->rx.fd_notifier, s->fd);
    event_poll_add(&s->rx.event_poll, &s->rx.fd_handler,
                   &s->rx.fd_notifier, handle_rx, false);

    /* TX */

    s->tx.vq = virtio_get_queue(s->vdev, 1);
    s->tx.guest_notifier = virtio_queue_get_guest_notifier(s->tx.vq);

    /* set up vring */
    if (!vring_setup(&s->tx.vring, s->vdev, 1)) {
        fprintf(stderr, "virtio-net failed to configure tx vring");
        return;
    }

    /* set up virtqueue tx notify handler*/
    event_poll_init(&s->tx.event_poll);
    if (s->vdev->binding->set_host_notifier(s->vdev->binding_opaque,
                                            1, true) != 0) {
        fprintf(stderr, "virtio-net failed to set host notifier\n");
        exit(1);
    }
    event_poll_add(&s->tx.event_poll, &s->tx.notify_handler,
                   virtio_queue_get_host_notifier(s->tx.vq),
                   handle_tx_kick, true);
#ifdef TX_BUFFERING
    s->tx.sendbuf = g_malloc(TX_SEND_BUF_SZ);
    s->tx.sendbuf_size = TX_SEND_BUF_SZ;
    s->tx.sendbuf_offset = 0;
#endif

    s->started = true;

    /* Spawn threads in BH so they inherit iothread cpusets */
    s->start_bh = qemu_bh_new(start_data_plane_bh, s);
    qemu_bh_schedule(s->start_bh);
}

void virtio_net_data_plane_stop(VirtIONetDataPlane *s)
{
    dprintf("stop");
    trace_virtio_net_data_plane_stop(s);
    //s->nc_tap->info->poll(s->nc_tap, true);
}

VirtIONetDataPlane *virtio_net_data_plane_create(VirtIODevice *vdev,
                                                 int tap_fd, bool has_vnet_hdr)
{
    VirtIONetDataPlane *s = g_new0(VirtIONetDataPlane, 1);

    dprintf("create");
    s->fd = tap_fd;
    s->vdev = vdev;
    s->has_vnet_hdr = has_vnet_hdr;

    return s;
}

void virtio_net_data_plane_destroy(VirtIONetDataPlane *s)
{
    dprintf("destroy");
    if (!s) {
        return;
    }
    virtio_net_data_plane_stop(s);
    g_free(s);
}


void virtio_net_data_plane_drain(VirtIONetDataPlane *s)
{
    dprintf("drain");
}

void virtio_net_data_plane_set_mrg_rx_bufs(VirtIONetDataPlane *s, int mergeable_rx_bufs)
{
    s->mergeable_rx_bufs = mergeable_rx_bufs;
}
