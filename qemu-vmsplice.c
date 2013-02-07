#include <glib.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/mman.h>
#include "qapi/error.h"
#include "qemu/sockets.h"

//#define DEBUG_VMSPLICE

#ifdef DEBUG_VMSPLICE
#define dprintf(...) \
do { \
    g_printerr(__VA_ARGS__); \
    g_printerr("\n"); \
} while (0)
#else
#define dprintf(...)
#endif

static void report_duration(const char *name, struct timeval t1, struct timeval t2)
{
    double usec = (t2.tv_sec - t1.tv_sec) * 1000 * 1000;
    usec += (t2.tv_usec - t1.tv_usec);
    g_printerr("test: %s\nduration: %f seconds\n", name, usec / 1000 / 1000);
}

#define PAGE_SIZE (1<<12)
#define PAGE_MASK (PAGE_SIZE - 1)

static void do_in(int fd, uint8_t *buf, size_t buf_size, ssize_t *bytes_read, uint64_t *checksum)
{
    size_t count = 0;
    int ret;
    uint64_t checksum_tmp = 0;
    size_t i = 0;
    size_t page_offset = 0;

    while (1) {
        ret = read(fd, buf, buf_size);
        if (ret == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            g_error("read(): %s", strerror(errno));
        }
        if (ret == 0) {
            dprintf("got a 0 read, returning...");
            break;
        }
        page_offset = count & PAGE_MASK;
        if (page_offset) {
            if (ret > (PAGE_SIZE - page_offset)) {
                checksum_tmp += buf[PAGE_SIZE - page_offset];
            }
        } else {
            checksum_tmp += buf[0];
        }
        for (i = 0; i < (ret - (PAGE_SIZE - page_offset)) / PAGE_SIZE; i++) {
            checksum_tmp += buf[(PAGE_SIZE - page_offset) + i * PAGE_SIZE];
        }
        count += ret;
        dprintf("total bytes read (socket): %zu", count);
    }
    dprintf("total bytes read (socket): %zu", count);
    *bytes_read = count;
    *checksum = checksum_tmp;
}

static void do_out(int fd, void *ptr, size_t len)
{
    ssize_t ret, count = 0;

    while (len > 0) {
        ret = write(fd, ptr, len);
        if (ret == -1) {
            if (errno == EAGAIN) {
                g_printerr("eagain");
                continue;
            }
            g_error("write(): %s", strerror(errno));
        }
        g_printerr("wrote %zd bytes\n", ret);
        count += ret;
        ptr += ret;
        len -= ret;
    }

    dprintf("total bytes written (socket): %zu", count);
}

static void do_in_vmsplice(int fd, uint8_t *buf, size_t buf_size, ssize_t *bytes_read, uint64_t *checksum)
{
    struct iovec iov = { 0 };
    ssize_t ret, count = 0;
    uint64_t checksum_tmp = 0;

    iov.iov_base = buf;
    iov.iov_len = buf_size;

    dprintf("vmsplicing in data");
    while (1) {
        size_t i = 0;
        ret = vmsplice(fd, &iov, 1, 0);
        if (ret == -1) {
            if (errno != EAGAIN && errno != EINTR) {
                g_error("vmplice() error: %s", strerror(errno));
            }
            dprintf("ret == -1");
        } else {
            if (ret == 0) {
                dprintf("vmsplice() returned 0, returning");
                break;
            }
            count += ret;
            for (i = 0; i < ret / 4096; i++) {
                checksum_tmp += buf[i*4096];
            }
        }
    }
    dprintf("total bytes read (vmsplice): %zu", count);
    *bytes_read = count;
    *checksum = checksum_tmp;
}

static void do_out_vmsplice(int fd_pipe[2], int fd, void *ptr, size_t len, bool need_pipe)
{
    struct iovec iov[1];
    ssize_t count = 0;
    ssize_t spliced_count = 0;
    ssize_t ret;

    iov[0].iov_base = ptr;
    iov[0].iov_len = MIN(len, 1<<30); 
    while (count < len) {
        /* no effect ??? */
        ret = vmsplice(fd_pipe[1], iov, 1, SPLICE_F_GIFT);
        //ret = vmsplice(fd_pipe[1], iov, 1, 0);
        if (ret == -1) {
            if (errno == EAGAIN) {
                g_printerr("eagain\n");
                continue;
            }
            g_error("vmsplice() error: %s", strerror(errno));
        } else {
            count += ret;
            iov[0].iov_base += ret;
            iov[0].iov_len = MIN(len - count, 1<<30);
            dprintf("flipped %d bytes to kernel", (int)ret);
        }

        if (fd != -1) {
            /* this was for testing splice()'ing a pipe to a unix socket. 
             * don't need it atm */
            assert(0);
            ssize_t remaining = count - spliced_count;
            if (count < len) {
                ret = splice(fd_pipe[0], NULL, fd, NULL, remaining, SPLICE_F_MORE | SPLICE_F_NONBLOCK);
            } else {
                ret = splice(fd_pipe[0], NULL, fd, NULL, remaining, SPLICE_F_NONBLOCK);
            }
            if (ret == -1) {
                g_error("error: %s", strerror(errno));
            }
            spliced_count += ret;
            dprintf("spliced through %d bytes", (int)ret);
        }
    }

    while (fd != -1 && spliced_count != count) {
        /* this was for testing splice()'ing a pipe to a unix socket. 
         * don't need it atm */
        assert(0);
        ssize_t remaining = count - spliced_count;
        if (count < len) {
            ret = splice(fd_pipe[0], NULL, fd, NULL, remaining, SPLICE_F_MORE | SPLICE_F_NONBLOCK);
        } else {
            ret = splice(fd_pipe[0], NULL, fd, NULL, remaining, SPLICE_F_NONBLOCK);
        }
        if (ret == -1) {
            g_error("error: %s", strerror(errno));
        }
        spliced_count += ret;
        dprintf("spliced through %d bytes", (int)ret);
    }

    dprintf("total bytes written (vmsplice): %zu", count);
}

static void do_alloc(void **ptr, size_t len)
{
    int i;
    uint8_t *buf;

    int ret;
    ret = posix_memalign(ptr, 4096, len);
    if (ret != 0) {
        g_error("posix_memalign(): %s", strerror(ret));
    }
#if 0
    *ptr = g_malloc0(len);
#endif
    buf = *ptr;
    for (i = 0; i < len; i++) {
        buf[i] = 'g';
    }
}

static void do_unmap(void **ptr, size_t len)
{
#if 0
    int ret = munmap(*ptr, len);
    if (ret == -1) {
        g_error("do_unmap(): %s", strerror(errno));
    }
    g_free(*ptr);
#endif
}

static int get_fd(int fd_sock)
{
    int fd = -1, ret;

    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    struct iovec iov = { 0 };
    char data[64], control[1024];

    iov.iov_base = data;
    iov.iov_len = sizeof(data) - 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    //ret = recvmsg(fd_sock, &msg, MSG_PEEK);
    ret = recvmsg(fd_sock, &msg, MSG_PEEK);
    if (ret == -1) {
        g_error("recvmsg(): %s", strerror(errno));
    }

    data[ret] = 0;

    cmsg = CMSG_FIRSTHDR(&msg);
    while (cmsg != NULL) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            fd = *(int *)CMSG_DATA(cmsg);
        } else {
            dprintf("cmsg not SCM_RIGHTS");
        }
        cmsg = CMSG_NXTHDR(&msg, cmsg);
    }

    return fd;
}

static void send_fd(int fd_sock, int fd)
{
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    struct iovec iov = { 0 };
    char control[1024], data[1024];
    int ret;

    strcpy(data, "hello from send_fd!");
    strcpy(control, "what on earth");
    iov.iov_base = data;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    *((int *)CMSG_DATA(cmsg)) = fd;

    msg.msg_controllen = cmsg->cmsg_len;

    ret = sendmsg(fd_sock, &msg, 0);
    if (ret == -1) {
        g_error("sendmsg(): %s", strerror(errno));
    }
}

static int listen_sock(const char *path)
{
    Error *err = NULL;
    int fd = unix_listen(path, NULL, strlen(path), &err);

    if (err != NULL) {
        g_error("%s", error_get_pretty(err));
    }
    return fd;
}

static int accept_sock(int fd_sock)
{
    int fd_client;
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);

    fd_client = qemu_accept(fd_sock, (struct sockaddr *)&addr, &addrlen);
    if (fd_client == -1) {
        g_error("qemu_accept() error");
    } else {
        g_printerr("client connected\n");
    }
    return fd_client;
}

static int connect_sock(const char *path)
{
    Error *err = NULL;
    int fd;

    if (path[0] == '/') {
        fd = unix_connect(path, &err);
    } else {
        fd = inet_connect(path, &err);
    }

    if (err != NULL) {
        g_error("%s", error_get_pretty(err));
    }

    return fd;
}

static void do_server(const char *sock_path, size_t buf_size, int inherited_pipe)
{
    int ret;
    uint8_t *buf = NULL;
    uint8_t buf2[1024];
    int fd_pipe[2];
    int fd_sock, fd_client;

    if (!buf_size) {
        buf_size = 1 << 20;
    }

    g_printerr("using %zd sized buffer to store\n", buf_size);
    fd_sock = listen_sock(sock_path);
    do_alloc((void **)&buf, buf_size);

    while (1) {
        ssize_t bytes_read = 0;
        uint64_t checksum = 0;

        g_printerr("--------\nwaiting for connection\n");
        fd_client = accept_sock(fd_sock);
        if (inherited_pipe != -1) {
            fd_pipe[0] = inherited_pipe;
            inherited_pipe = -1;
        } else {
            fd_pipe[0] = get_fd(fd_client);
        }

        if (fd_pipe[0] != -1) {
            if (inherited_pipe != -1) {
                g_printerr("got pipe via fork(), gonna use it\n");
            } else {
                g_printerr("got pipe via SCM_RIGHTS, gonna use it\n");
            }
            do_in_vmsplice(fd_pipe[0], buf, buf_size, &bytes_read, &checksum);
            close(fd_pipe[0]);
        } else {
            g_printerr("no pipe, using socket\n");
            do_in(fd_client, buf, buf_size, &bytes_read, &checksum);
        }

        g_printerr("completed. bytes read %zd\n", bytes_read);
        g_printerr("checksum: %lu\n", checksum);

        /* print out remainder of socket buf */
        ret = read(fd_client, buf2, 1024);
        if (ret == -1) {
            dprintf("read(): %s", strerror(errno));
        } else {
            buf[ret] = '\0';
            dprintf("data remaining in socket (%d bytes): %s", ret, buf);
        }

        close(fd_client);
        g_printerr("client finished\n");
    }
    do_unmap((void **)&buf, buf_size);
    close(fd_sock);
}

//#define PIPESZ 1048576
//#define PIPESZ 16384
//#define PIPESZ (64 << 10)

static void do_client(const char *sock_path, size_t buf_size, bool use_pipe, int inherited_pipe)
{
    size_t i = 0;
    uint8_t *buf;
    uint64_t sum;
    int fd_sock;
    int fd_pipe[2];
    void *ptr;
    struct timeval t1, t2;

    if (!buf_size) {
        buf_size = 512 << 20;
    }

    g_printerr("allocating memory: %zu\n", buf_size);
    do_alloc(&ptr, buf_size);
    g_printerr("allocated.\n");

    buf = ptr;
    sum = 0;
    for (i = 0; i < buf_size / 4096; i++) {
        sum += buf[i*4096];
    }
    g_printerr("checksum: %lu\n", sum);

    fd_sock = connect_sock(sock_path);

    if (use_pipe) {
        /* vmsplice -> pipe via SCM_RIGHTS */
        if (inherited_pipe != -1) {
            fd_pipe[1] = inherited_pipe;
        } else if (pipe(fd_pipe) != 0) {
            g_error("pipe(): %s", strerror(errno));
        }
        //const char *msg = "thanks for handling my 'out_pipe' command";
        if (inherited_pipe == -1) {
#ifdef PIPESZ
            if (fcntl(fd_pipe[1], F_SETPIPE_SZ, PIPESZ) == -1) {
                g_error("fcntl(): %s", strerror(errno));
            }
#endif
            send_fd(fd_sock, fd_pipe[0]);
        }
        //write(fd_sock, msg, strlen(msg));
        gettimeofday(&t1, NULL);
        do_out_vmsplice(fd_pipe, -1, ptr, buf_size, false);
        gettimeofday(&t2, NULL);
        if (inherited_pipe != -1) {
            report_duration("vmsplice -> pipe (via fork())", t1, t2);
        } else {
            report_duration("vmsplice -> pipe (SCM_RIGHTS)", t1, t2);
        }
        //close(fd_pipe[0]);
        close(fd_pipe[1]);
    } else {
        /* write -> socket */
        /*
        const char *msg = "thanks for handling my 'out' command";
        write(fd_sock, msg, strlen(msg));
        */
        gettimeofday(&t1, NULL);
        do_out(fd_sock, ptr, buf_size);
        gettimeofday(&t2, NULL);
        report_duration("write -> socket", t1, t2);
    }

    /* DEBUG: iterate back over all pages to see if vmsplice() is always
     * using a COW mapping to pass aligned pages through
     */
    sum = 0;
    for (i = 0; i < buf_size / 4096; i++) {
        sum += buf[i*4096];
    }
    g_printerr("checksum: %lu\n\n", sum);

    do_unmap(&ptr, buf_size);
    close(fd_sock);
}

int main(int argc, char **argv)
{
    const char *sock_path = "/tmp/vmsplice.sock";
    //const char *sock_path = "127.0.0.1:5150";
    const char *cmd = argv[1];
    size_t len = 0;

    if (argc > 2) {
        len = atol(argv[2]);
    }

    if (strcmp(cmd, "fork") == 0) {
        int pipe_fd[2];
        int ret;
        size_t client_len = atol(argv[3]);

        if (argc <= 3) {
            g_error("must specify size of client buffer");
        }

        pipe(pipe_fd);
#ifdef PIPESZ
        if (fcntl(pipe_fd[1], F_SETPIPE_SZ, PIPESZ) == -1) {
            g_error("fcntl(): %s", strerror(errno));
        }
#endif
        ret = fork();
        if (ret == -1) {
            g_error("fork(): %s", strerror(errno));
        } else if (ret == 0) {
            /* child */
            setsid();
            do_client(sock_path, client_len, true, pipe_fd[1]);
        } else {
            do_server(sock_path, len, pipe_fd[0]);
        }
    }

    if (strncmp(cmd, "in", 2) == 0) {
        do_server(sock_path, len, -1);
    }

    if (strncmp(cmd, "out", 3) == 0) {
        if (strcmp(cmd, "out_pipe") == 0) {
            do_client(sock_path, len, true, -1);
        } else {
            do_client(sock_path, len, false, -1);
        }

    }

    return 0;

    /* simple */
#if 0
    fd_sock = connect_sock(sock_path);
    do_alloc(&ptr, len);
    gettimeofday(&t1, NULL);
    do_out_simple(fd_sock, ptr, len);
    gettimeofday(&t2, NULL);
    report_duration("simple", t1, t2);
    do_unmap(&ptr, len);
    close(fd_sock);

    /* simple */
    fd_sock = connect_sock(sock_path);
    do_alloc(&ptr, len);
    gettimeofday(&t1, NULL);
    do_out_simple(fd_sock, ptr, len);
    gettimeofday(&t2, NULL);
    report_duration("simple", t1, t2);
    do_unmap(&ptr, len);
    close(fd_sock);

    /* vmsplice -> pipe -> socket */
    fd_sock = connect_sock(sock_path);
    if (pipe(fd_pipe) == -1) {
        g_error("pipe(): %s", strerror(errno));
    }
    if (fcntl(fd_pipe[1], F_SETPIPE_SZ, MIN(len, 1048576)) == -1) {
        g_error("fcntl(): %s", strerror(errno));
    }
    do_alloc(&ptr, len);
    gettimeofday(&t1, NULL);
    do_out_vmsplice(fd_pipe, fd_sock, ptr, len, true);
    gettimeofday(&t2, NULL);
    report_duration("vmsplice -> pipe -> socket", t1, t2);
    do_unmap(&ptr, len);
    close(fd_sock);
#endif

#if 0
    /* vmsplice -> pipe */
    fd_pipe[1] = open(pipe_path, O_WRONLY);
    //fd_pipe[0] = open(pipe_path, O_RDONLY);
    g_printerr("len: %lu\n", len);
    do_alloc(&ptr, len);
    gettimeofday(&t1, NULL);
    do_out_vmsplice(fd_pipe, -1, ptr, len, false);
    gettimeofday(&t2, NULL);
    report_duration("vmsplice -> pipe", t1, t2);
    do_unmap(&ptr, len);
    close(fd_pipe[0]);
    close(fd_pipe[1]);
#endif
}
