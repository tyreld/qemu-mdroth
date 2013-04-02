/*
 * Dedicated thread for virtio-net I/O processing
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

#ifndef HW_DATAPLANE_VIRTIO_NET_H
#define HW_DATAPLANE_VIRTIO_NET_H

#include "hw/virtio.h"
#include "net/net.h"

typedef struct VirtIONetDataPlane VirtIONetDataPlane;

#ifdef CONFIG_VIRTIO_NET_DATA_PLANE
VirtIONetDataPlane *virtio_net_data_plane_create(VirtIODevice *vdev, int fd,
                                                 bool has_vnet_hdr);
void virtio_net_data_plane_destroy(VirtIONetDataPlane *s);
void virtio_net_data_plane_start(VirtIONetDataPlane *s);
void virtio_net_data_plane_stop(VirtIONetDataPlane *s);
void virtio_net_data_plane_drain(VirtIONetDataPlane *s);
void virtio_net_data_plane_set_mrg_rx_bufs(VirtIONetDataPlane *s, int mergeable_rx_bufs);
#else
static inline VirtIONetDataPlane *virtio_net_data_plane_create(
        VirtIODevice *vdev)
{
    return NULL;
}

static inline void virtio_net_data_plane_destroy(VirtIONetDataPlane *s) {}
static inline void virtio_net_data_plane_start(VirtIONetDataPlane *s) {}
static inline void virtio_net_data_plane_stop(VirtIONetDataPlane *s) {}
static inline void virtio_net_data_plane_drain(VirtIONetDataPlane *s) {}
static inline void virtio_net_data_plane_set_mrg_rx_bufs(VirtIONetDataPlane *s, int mergeable_rx_bufs) {}
#endif

#endif /* HW_DATAPLANE_VIRTIO_NET_H */
