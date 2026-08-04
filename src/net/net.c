/*
 * src/net/net.c — FORGE networking system
 *
 * Features:
 *   - TCP server with epoll (Linux), raw POSIX
 *   - UDP socket support
 *   - Pre-allocated connection pool with free-list
 *   - Length-prefixed message framing
 *   - Non-blocking I/O
 *   - Event-driven callback architecture
 *
 * Pure C23, zero external dependencies. Linux only.
 */

#define _GNU_SOURCE

#include "forge/net.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/epoll.h>

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

#define FGE_NET_UDP_SENTINEL ((void *)1)

static void emit_event(fge_net_context_t *ctx, fge_net_event_type_t type,
                       fge_connection_t *conn, const fge_net_addr_t *addr,
                       const fge_net_msg_t *msg, fge_result_t error)
{
    if (!ctx || !ctx->event_cb) return;

    fge_net_event_t event = {0};
    event.type = type;
    event.conn = conn;
    event.error = error;
    if (addr) event.addr = *addr;
    if (msg)  event.msg  = *msg;

    ctx->event_cb(&event, ctx->event_userdata);
}

static void conn_pool_free(fge_net_context_t *ctx, fge_connection_t *conn);

static void conn_reset(fge_connection_t *conn)
{
    if (!conn) return;
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->state = FGE_CONN_DISCONNECTED;
    fge_net_buffer_free(&conn->recv_buf);
    fge_net_buffer_free(&conn->send_buf);
    conn->bytes_received = 0;
    conn->bytes_sent = 0;
    conn->msgs_received = 0;
    conn->msgs_sent = 0;
    conn->user_data = NULL;
    conn->next_active = NULL;
    conn->prev_active = NULL;
}

static void active_list_add(fge_net_context_t *ctx, fge_connection_t *conn)
{
    conn->next_active = ctx->active_list;
    conn->prev_active = NULL;
    if (ctx->active_list) ctx->active_list->prev_active = conn;
    ctx->active_list = conn;
    ctx->active_count++;
}

static void active_list_remove(fge_net_context_t *ctx, fge_connection_t *conn)
{
    if (conn->prev_active) conn->prev_active->next_active = conn->next_active;
    else ctx->active_list = conn->next_active;
    if (conn->next_active) conn->next_active->prev_active = conn->prev_active;
    conn->next_active = NULL;
    conn->prev_active = NULL;
    if (ctx->active_count > 0) ctx->active_count--;
}

static fge_connection_t *conn_pool_alloc(fge_net_context_t *ctx)
{
    if (!ctx) return NULL;
    fge_spinlock_lock(&ctx->conn_lock);
    fge_connection_t *conn = ctx->free_list;
    if (conn) {
        ctx->free_list = conn->next_free;
        conn->next_free = NULL;
    }
    fge_spinlock_unlock(&ctx->conn_lock);
    if (!conn) return NULL;

    conn->id = ctx->next_conn_id++;
    conn->fd = -1;
    conn->state = FGE_CONN_CONNECTING;
    conn->bytes_received = 0;
    conn->bytes_sent = 0;
    conn->msgs_received = 0;
    conn->msgs_sent = 0;
    conn->user_data = NULL;
    active_list_add(ctx, conn);
    return conn;
}

static void conn_pool_free(fge_net_context_t *ctx, fge_connection_t *conn)
{
    if (!ctx || !conn) return;
    active_list_remove(ctx, conn);
    conn_reset(conn);
    fge_spinlock_lock(&ctx->conn_lock);
    conn->next_free = ctx->free_list;
    ctx->free_list = conn;
    fge_spinlock_unlock(&ctx->conn_lock);
}

static void epoll_mod_events(fge_net_context_t *ctx, fge_connection_t *conn, uint32_t events)
{
    if (!ctx || !conn || conn->fd < 0) return;
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.ptr = conn;
    (void)epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

static void enable_epollout(fge_net_context_t *ctx, fge_connection_t *conn)
{
    epoll_mod_events(ctx, conn, EPOLLIN | EPOLLOUT);
}

static void disable_epollout(fge_net_context_t *ctx, fge_connection_t *conn)
{
    epoll_mod_events(ctx, conn, EPOLLIN);
}

static void parse_messages(fge_net_context_t *ctx, fge_connection_t *conn);
static void handle_accept(fge_net_context_t *ctx);
static void handle_read(fge_net_context_t *ctx, fge_connection_t *conn);
static void handle_write(fge_net_context_t *ctx, fge_connection_t *conn);
static void handle_udp(fge_net_context_t *ctx);
static void check_timeouts(fge_net_context_t *ctx);

/* -------------------------------------------------------------------------- */
/* Address                                                                    */
/* -------------------------------------------------------------------------- */

bool fge_net_addr_from_string(fge_net_addr_t *out, const char *host, uint16_t port)
{
    if (!out || !host) return false;
    memset(out, 0, sizeof(*out));

    char port_str[6];
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return false;

    if (res->ai_addrlen > (socklen_t)sizeof(out->addr)) {
        freeaddrinfo(res);
        return false;
    }

    memcpy(&out->addr, res->ai_addr, res->ai_addrlen);
    out->len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

bool fge_net_addr_from_sockaddr(fge_net_addr_t *out, const struct sockaddr *sa, socklen_t len)
{
    if (!out || !sa || len == 0 || len > (socklen_t)sizeof(out->addr)) return false;
    memcpy(&out->addr, sa, len);
    out->len = len;
    return true;
}

const char *fge_net_addr_to_string(const fge_net_addr_t *addr, char *buf, size_t buf_size)
{
    if (!addr || !buf || buf_size == 0) return "?";

    char ip_str[INET6_ADDRSTRLEN] = {0};
    const char *result = NULL;
    uint16_t port = 0;

    if (addr->addr.sa_family == AF_INET) {
        result = inet_ntop(AF_INET, &addr->v4.sin_addr, ip_str, sizeof(ip_str));
        port = ntohs(addr->v4.sin_port);
        if (result)
            (void)snprintf(buf, buf_size, "%s:%u", ip_str, (unsigned)port);
    } else if (addr->addr.sa_family == AF_INET6) {
        result = inet_ntop(AF_INET6, &addr->v6.sin6_addr, ip_str, sizeof(ip_str));
        port = ntohs(addr->v6.sin6_port);
        if (result)
            (void)snprintf(buf, buf_size, "[%s]:%u", ip_str, (unsigned)port);
    } else {
        (void)snprintf(buf, buf_size, "?");
        return buf;
    }

    if (!result) {
        (void)snprintf(buf, buf_size, "?");
    }
    return buf;
}

/* -------------------------------------------------------------------------- */
/* Buffer                                                                     */
/* -------------------------------------------------------------------------- */

bool fge_net_buffer_init(fge_net_buffer_t *b, size_t initial_size)
{
    if (!b) return false;
    if (initial_size == 0) initial_size = 256;
    b->data = (uint8_t *)FGE_MALLOC(initial_size);
    if (!b->data) return false;
    b->size = initial_size;
    b->head = 0;
    b->tail = 0;
    return true;
}

void fge_net_buffer_free(fge_net_buffer_t *b)
{
    if (!b) return;
    FGE_FREE(b->data);
    b->data = NULL;
    b->size = 0;
    b->head = 0;
    b->tail = 0;
}

void fge_net_buffer_reset(fge_net_buffer_t *b)
{
    if (b) b->head = b->tail = 0;
}

bool fge_net_buffer_ensure(fge_net_buffer_t *b, size_t need)
{
    if (!b) return false;
    size_t used = b->head - b->tail;
    if (b->size - used >= need) return true;

    size_t new_size = b->size ? b->size : 256;
    while (new_size - used < need) new_size *= 2;

    uint8_t *nd = (uint8_t *)FGE_MALLOC(new_size);
    if (!nd) return false;
    /* Copy logical data to new buffer starting at 0 */
    size_t tail_off = b->tail % b->size;
    size_t first = b->size - tail_off;
    if (first > used) first = used;
    memcpy(nd, b->data + tail_off, first);
    if (used > first) memcpy(nd + first, b->data, used - first);
    FGE_FREE(b->data);
    b->data = nd;
    b->size = new_size;
    b->head = used;
    b->tail = 0;
    return true;
}

size_t fge_net_buffer_readable(const fge_net_buffer_t *b)
{
    return b ? b->head - b->tail : 0;
}

size_t fge_net_buffer_writable(const fge_net_buffer_t *b)
{
    return b ? b->size - (b->head - b->tail) : 0;
}

uint8_t *fge_net_buffer_write_ptr(fge_net_buffer_t *b)
{
    return b ? b->data + (b->head % b->size) : NULL;
}

void fge_net_buffer_advance_write(fge_net_buffer_t *b, size_t n)
{
    if (!b) return;
    size_t avail = b->size - (b->head - b->tail);
    if (n > avail) n = avail;
    b->head += n;
}

uint8_t *fge_net_buffer_read_ptr(const fge_net_buffer_t *b)
{
    return b ? b->data + (b->tail % b->size) : NULL;
}

void fge_net_buffer_advance_read(fge_net_buffer_t *b, size_t n)
{
    if (!b) return;
    size_t avail = b->head - b->tail;
    if (n > avail) n = avail;
    b->tail += n;
}

bool fge_net_buffer_push(fge_net_buffer_t *b, const uint8_t *data, size_t len)
{
    if (!b || !data || len == 0) return false;
    if (!fge_net_buffer_ensure(b, len)) return false;
    size_t head_off = b->head % b->size;
    size_t first = b->size - head_off;
    if (first > len) first = len;
    memcpy(b->data + head_off, data, first);
    if (len > first) memcpy(b->data, data + first, len - first);
    b->head += len;
    return true;
}

size_t fge_net_buffer_peek(const fge_net_buffer_t *b, uint8_t *out, size_t len)
{
    if (!b || !out) return 0;
    size_t readable = b->head - b->tail;
    if (len > readable) len = readable;
    size_t tail_off = b->tail % b->size;
    size_t first = b->size - tail_off;
    if (first > len) first = len;
    memcpy(out, b->data + tail_off, first);
    if (len > first) memcpy(out + first, b->data, len - first);
    return len;
}

/* -------------------------------------------------------------------------- */
/* Connection state string                                                    */
/* -------------------------------------------------------------------------- */

const char *fge_conn_state_str(fge_conn_state_t s)
{
    switch (s) {
        case FGE_CONN_DISCONNECTED: return "DISCONNECTED";
        case FGE_CONN_CONNECTING:   return "CONNECTING";
        case FGE_CONN_CONNECTED:    return "CONNECTED";
        case FGE_CONN_CLOSING:      return "CLOSING";
        default:                    return "?";
    }
}

/* -------------------------------------------------------------------------- */
/* Context lifecycle                                                          */
/* -------------------------------------------------------------------------- */

fge_net_context_t *fge_net_create(fge_net_event_cb_t cb, void *userdata)
{
    fge_net_context_t *ctx = (fge_net_context_t *)FGE_CALLOC(1, sizeof(fge_net_context_t));
    if (!ctx) return NULL;

    ctx->event_cb = cb;
    ctx->event_userdata = userdata;
    ctx->listen_fd = -1;
    ctx->udp_fd = -1;
    ctx->epoll_fd = -1;
    ctx->running = false;
    fge_spinlock_init(&ctx->conn_lock);
    fge_clock_init(&ctx->clock);
    ctx->now = fge_clock_elapsed_sec(&ctx->clock);

    for (int32_t i = FGE_NET_MAX_CONNECTIONS - 1; i >= 0; i--) {
        ctx->connections[i].fd = -1;
        ctx->connections[i].state = FGE_CONN_DISCONNECTED;
        ctx->connections[i].next_free = ctx->free_list;
        ctx->free_list = &ctx->connections[i];
    }

    return ctx;
}

void fge_net_destroy(fge_net_context_t *ctx)
{
    if (!ctx) return;

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->udp_fd >= 0) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
    }

    for (uint32_t i = 0; i < FGE_NET_MAX_CONNECTIONS; i++) {
        fge_connection_t *conn = &ctx->connections[i];
        if (conn->fd >= 0) {
            close(conn->fd);
            conn->fd = -1;
        }
        fge_net_buffer_free(&conn->recv_buf);
        fge_net_buffer_free(&conn->send_buf);
    }

    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }

    FGE_FREE(ctx);
}

/* -------------------------------------------------------------------------- */
/* TCP server                                                                 */
/* -------------------------------------------------------------------------- */

bool fge_net_listen(fge_net_context_t *ctx, uint16_t port)
{
    if (!ctx) return false;
    if (ctx->listen_fd >= 0) return false;

    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool is_v6 = true;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        is_v6 = false;
        if (fd < 0) return false;
    }

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (is_v6) {
        int v6only = 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        struct sockaddr_in6 addr6 = {0};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6addr_any;

        if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
            close(fd);
            fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return false;
            is_v6 = false;
        }
    }

    if (!is_v6) {
        struct sockaddr_in addr4 = {0};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            close(fd);
            return false;
        }
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return false;
    }

    if (!fge_net_set_nonblocking(fd)) {
        close(fd);
        return false;
    }

    ctx->listen_fd = fd;
    ctx->listen_port = port;

    if (ctx->epoll_fd < 0) {
        ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (ctx->epoll_fd < 0) {
            close(fd);
            ctx->listen_fd = -1;
            return false;
        }
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* NULL sentinel = listener */
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        ctx->listen_fd = -1;
        return false;
    }

    FGE_INFO(FGE_LOG_CAT_NETWORK, "Listening on port %u", (unsigned)port);
    return true;
}

/* -------------------------------------------------------------------------- */
/* TCP client                                                                 */
/* -------------------------------------------------------------------------- */

fge_connection_t *fge_net_connect(fge_net_context_t *ctx, const char *host, uint16_t port)
{
    if (!ctx) return NULL;

    fge_net_addr_t addr = {0};
    if (!fge_net_addr_from_string(&addr, host, port)) {
        FGE_WARN(FGE_LOG_CAT_NETWORK, "Failed to resolve host: %s", host);
        return NULL;
    }

    int fd = socket(addr.addr.sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;

    if (!fge_net_set_nonblocking(fd)) {
        close(fd);
        return NULL;
    }

    fge_connection_t *conn = conn_pool_alloc(ctx);
    if (!conn) {
        close(fd);
        return NULL;
    }

    conn->fd = fd;
    conn->remote_addr = addr;
    conn->state = FGE_CONN_CONNECTING;
    conn->connect_time = ctx->now;

    if (connect(fd, &addr.addr, addr.len) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            conn->fd = -1;
            conn_pool_free(ctx, conn);
            return NULL;
        }
    }

    if (ctx->epoll_fd < 0) {
        ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (ctx->epoll_fd < 0) {
            close(fd);
            conn->fd = -1;
            conn_pool_free(ctx, conn);
            return NULL;
        }
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = conn;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        conn->fd = -1;
        conn_pool_free(ctx, conn);
        return NULL;
    }

    return conn;
}

/* -------------------------------------------------------------------------- */
/* UDP socket                                                                 */
/* -------------------------------------------------------------------------- */

bool fge_net_bind_udp(fge_net_context_t *ctx, uint16_t port)
{
    if (!ctx) return false;
    if (ctx->udp_fd >= 0) return false;

    int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool is_v6 = true;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        is_v6 = false;
        if (fd < 0) return false;
    }

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (is_v6) {
        int v6only = 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        struct sockaddr_in6 addr6 = {0};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6addr_any;

        if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
            close(fd);
            fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return false;
            is_v6 = false;
        }
    }

    if (!is_v6) {
        struct sockaddr_in addr4 = {0};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            close(fd);
            return false;
        }
    }

    if (!fge_net_set_nonblocking(fd)) {
        close(fd);
        return false;
    }

    ctx->udp_fd = fd;

    if (ctx->epoll_fd < 0) {
        ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (ctx->epoll_fd < 0) {
            close(fd);
            ctx->udp_fd = -1;
            return false;
        }
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = FGE_NET_UDP_SENTINEL;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        ctx->udp_fd = -1;
        return false;
    }

    FGE_INFO(FGE_LOG_CAT_NET_UDP, "UDP bound on port %u", (unsigned)port);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Close connection                                                           */
/* -------------------------------------------------------------------------- */

void fge_net_close(fge_net_context_t *ctx, fge_connection_t *conn)
{
    if (!ctx || !conn) return;
    if (conn->fd < 0) return;

    (void)epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    conn->fd = -1;

    if (conn->state != FGE_CONN_DISCONNECTED) {
        conn->state = FGE_CONN_DISCONNECTED;
        emit_event(ctx, FGE_NET_EVENT_DISCONNECT, conn, NULL, NULL, FGE_OK);
    }

    fge_net_buffer_free(&conn->recv_buf);
    fge_net_buffer_free(&conn->send_buf);
    conn_pool_free(ctx, conn);
}

/* -------------------------------------------------------------------------- */
/* Send — framed TCP message                                                  */
/* -------------------------------------------------------------------------- */

bool fge_net_send(fge_net_context_t *ctx, fge_connection_t *conn, uint16_t msg_type,
                  const uint8_t *payload, uint32_t payload_len)
{
    if (!ctx || !conn) return false;
    if (conn->state != FGE_CONN_CONNECTED && conn->state != FGE_CONN_CONNECTING)
        return false;
    if (payload_len > FGE_NET_MAX_MSG_SIZE) return false;

    uint32_t net_len   = htonl(payload_len);
    uint16_t net_type  = htons(msg_type);
    uint16_t net_flags = htons(0);

    if (!fge_net_buffer_ensure(&conn->send_buf, 8 + payload_len)) return false;

    uint8_t *wp = fge_net_buffer_write_ptr(&conn->send_buf);
    memcpy(wp + 0, &net_len,   4);
    memcpy(wp + 4, &net_type,  2);
    memcpy(wp + 6, &net_flags, 2);
    if (payload_len > 0) memcpy(wp + 8, payload, payload_len);
    fge_net_buffer_advance_write(&conn->send_buf, 8 + payload_len);
    conn->msgs_sent++;

    /* Single best-effort write; EPOLLOUT will re-arm if more remains */
    size_t readable = fge_net_buffer_readable(&conn->send_buf);
    if (readable > 0) {
        size_t tail_off = conn->send_buf.tail % conn->send_buf.size;
        size_t contiguous = conn->send_buf.size - tail_off;
        if (contiguous > readable) contiguous = readable;
        uint8_t *rp = conn->send_buf.data + tail_off;
        ssize_t n = write(conn->fd, rp, contiguous);
        if (n > 0) {
            fge_net_buffer_advance_read(&conn->send_buf, (size_t)n);
            conn->bytes_sent += (uint64_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* fall through to EPOLLOUT arming */
            } else if (errno != EINTR) {
                emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_IO);
                fge_net_close(ctx, conn);
                return false;
            }
        }
    }

    /* If data remains, arm EPOLLOUT for event-driven flush */
    if (fge_net_buffer_readable(&conn->send_buf) > 0) {
        enable_epollout(ctx, conn);
    } else {
        fge_net_buffer_reset(&conn->send_buf);
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* Send UDP                                                                   */
/* -------------------------------------------------------------------------- */

bool fge_net_send_udp(fge_net_context_t *ctx, const fge_net_addr_t *addr,
                      uint16_t msg_type, const uint8_t *payload, uint32_t payload_len)
{
    if (!ctx || !addr || ctx->udp_fd < 0) return false;
    if (payload_len > FGE_NET_MAX_MSG_SIZE) return false;

    uint8_t buf[FGE_NET_MAX_MSG_SIZE + 8];
    uint32_t net_len   = htonl(payload_len);
    uint16_t net_type  = htons(msg_type);
    uint16_t net_flags = htons(0);

    memcpy(buf + 0, &net_len,   4);
    memcpy(buf + 4, &net_type,  2);
    memcpy(buf + 6, &net_flags, 2);
    if (payload_len > 0) memcpy(buf + 8, payload, payload_len);

    ssize_t n = sendto(ctx->udp_fd, buf, 8 + payload_len, 0,
                       &addr->addr, addr->len);
    return n == (ssize_t)(8 + payload_len);
}

/* -------------------------------------------------------------------------- */
/* Event loop                                                                 */
/* -------------------------------------------------------------------------- */

static void parse_messages(fge_net_context_t *ctx, fge_connection_t *conn)
{
    uint8_t header_buf[8];
    while (1) {
        size_t readable = fge_net_buffer_readable(&conn->recv_buf);
        if (readable < 8) break; /* need full header */

        /* Peek header (may wrap in ring buffer) */
        fge_net_buffer_peek(&conn->recv_buf, header_buf, 8);
        uint32_t payload_len = ((uint32_t)header_buf[0] << 24) |
                               ((uint32_t)header_buf[1] << 16) |
                               ((uint32_t)header_buf[2] << 8)  |
                               ((uint32_t)header_buf[3]);
        uint16_t msg_type = ((uint16_t)header_buf[4] << 8) | (uint16_t)header_buf[5];
        uint16_t flags    = ((uint16_t)header_buf[6] << 8) | (uint16_t)header_buf[7];

        if (payload_len > FGE_NET_MAX_MSG_SIZE) {
            FGE_WARN(FGE_LOG_CAT_NET_PROTOCOL, "Oversized message from conn %u", conn->id);
            emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_PROTOCOL);
            fge_net_close(ctx, conn);
            return;
        }

        if (readable < 8 + payload_len) break; /* incomplete payload */

        /* For small messages, copy payload out of ring. For large, we could
         * expose a scatter/gather interface. For now, copy to stack buffer. */
        uint8_t payload_buf[FGE_NET_MAX_MSG_SIZE];
        if (payload_len > 0) {
            fge_net_buffer_peek(&conn->recv_buf, payload_buf, 8 + payload_len);
        }

        fge_net_msg_header_t header = {
            .len   = payload_len,
            .seq   = 0,
            .type  = msg_type,
            .flags = flags
        };
        fge_net_msg_t msg = {
            .header  = header,
            .payload = payload_len > 0 ? payload_buf + 8 : NULL
        };

        conn->msgs_received++;
        emit_event(ctx, FGE_NET_EVENT_DATA, conn, NULL, &msg, FGE_OK);
        fge_net_buffer_advance_read(&conn->recv_buf, 8 + payload_len);
    }
}

static void handle_accept(fge_net_context_t *ctx)
{
    while (1) {
        fge_net_addr_t addr = {0};
        socklen_t addrlen = sizeof(addr.addr);
        int fd = accept(ctx->listen_fd, &addr.addr, &addrlen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            FGE_WARN(FGE_LOG_CAT_NETWORK, "accept failed: %s", strerror(errno));
            continue;
        }
        addr.len = addrlen;

        fge_connection_t *conn = conn_pool_alloc(ctx);
        if (!conn) {
            FGE_WARN(FGE_LOG_CAT_NETWORK, "Connection pool exhausted");
            close(fd);
            continue;
        }

        conn->fd = fd;
        conn->remote_addr = addr;
        conn->state = FGE_CONN_CONNECTED;
        conn->connect_time = ctx->now;
        conn->last_recv_time = ctx->now;

        if (!fge_net_set_nonblocking(fd)) {
            close(fd);
            conn->fd = -1;
            conn_pool_free(ctx, conn);
            continue;
        }
        (void)fge_net_set_nodelay(fd);

        struct epoll_event ev = {0};
        ev.events = EPOLLIN;
        ev.data.ptr = conn;
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            FGE_WARN(FGE_LOG_CAT_NETWORK, "epoll_ctl ADD failed: %s", strerror(errno));
            close(fd);
            conn->fd = -1;
            conn_pool_free(ctx, conn);
            continue;
        }

        FGE_DEBUG(FGE_LOG_CAT_NETWORK, "Accepted connection %u from %s",
                  conn->id,
                  fge_net_addr_to_string(&addr,
                                         (char[INET6_ADDRSTRLEN + 8]){0},
                                         INET6_ADDRSTRLEN + 8));
        emit_event(ctx, FGE_NET_EVENT_ACCEPT, conn, &addr, NULL, FGE_OK);
    }
}

static void handle_read(fge_net_context_t *ctx, fge_connection_t *conn)
{
    while (1) {
        if (!fge_net_buffer_ensure(&conn->recv_buf, FGE_NET_RECV_BUF_SIZE / 4)) {
            emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_NOMEM);
            fge_net_close(ctx, conn);
            return;
        }

        /* Compute contiguous writable region (may wrap) */
        size_t head_off = conn->recv_buf.head % conn->recv_buf.size;
        size_t tail_off = conn->recv_buf.tail % conn->recv_buf.size;
        size_t used = conn->recv_buf.head - conn->recv_buf.tail;
        size_t avail;
        if (used == 0) {
            avail = conn->recv_buf.size - head_off;
        } else if (head_off >= tail_off) {
            avail = conn->recv_buf.size - head_off;
        } else {
            avail = tail_off - head_off;
        }
        if (avail == 0) break; /* buffer full */

        uint8_t *wp = conn->recv_buf.data + head_off;
        ssize_t n = read(conn->fd, wp, avail);

        if (n > 0) {
            fge_net_buffer_advance_write(&conn->recv_buf, (size_t)n);
            conn->bytes_received += (uint64_t)n;
            conn->last_recv_time = ctx->now;
        } else if (n == 0) {
            emit_event(ctx, FGE_NET_EVENT_DISCONNECT, conn, NULL, NULL, FGE_OK);
            fge_net_close(ctx, conn);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_IO);
            fge_net_close(ctx, conn);
            return;
        }
    }

    parse_messages(ctx, conn);
}

static void handle_write(fge_net_context_t *ctx, fge_connection_t *conn)
{
    if (conn->state == FGE_CONN_CONNECTING) {
        int soerr = 0;
        socklen_t soerrlen = sizeof(soerr);
        if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrlen) < 0 || soerr != 0) {
            emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_NETWORK);
            fge_net_close(ctx, conn);
            return;
        }
        conn->state = FGE_CONN_CONNECTED;
        (void)fge_net_set_nodelay(conn->fd);
        emit_event(ctx, FGE_NET_EVENT_CONNECT, conn, NULL, NULL, FGE_OK);
    }

    /* Write contiguous regions from ring buffer */
    while (fge_net_buffer_readable(&conn->send_buf) > 0) {
        size_t tail_off = conn->send_buf.tail % conn->send_buf.size;
        size_t readable = fge_net_buffer_readable(&conn->send_buf);
        size_t contiguous = conn->send_buf.size - tail_off;
        if (contiguous > readable) contiguous = readable;

        uint8_t *rp = conn->send_buf.data + tail_off;
        ssize_t n = write(conn->fd, rp, contiguous);
        if (n > 0) {
            fge_net_buffer_advance_read(&conn->send_buf, (size_t)n);
            conn->bytes_sent += (uint64_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_IO);
            fge_net_close(ctx, conn);
            return;
        }
    }

    if (fge_net_buffer_readable(&conn->send_buf) == 0) {
        fge_net_buffer_reset(&conn->send_buf);
        disable_epollout(ctx, conn);
        if (conn->state == FGE_CONN_CLOSING) {
            fge_net_close(ctx, conn);
        }
    }
}

static void handle_udp(fge_net_context_t *ctx)
{
    uint8_t buf[FGE_NET_MAX_MSG_SIZE + 8];
    fge_net_addr_t addr = {0};
    socklen_t addrlen = sizeof(addr.addr);

    ssize_t n = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0, &addr.addr, &addrlen);
    if (n < 8) return;
    addr.len = addrlen;

    uint32_t payload_len = ((uint32_t)buf[0] << 24) |
                           ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8)  |
                           ((uint32_t)buf[3]);
    uint16_t msg_type = ((uint16_t)buf[4] << 8) | (uint16_t)buf[5];
    uint16_t flags    = ((uint16_t)buf[6] << 8) | (uint16_t)buf[7];

    if (payload_len > FGE_NET_MAX_MSG_SIZE || (size_t)n < 8 + payload_len)
        return;

    fge_net_msg_header_t header = {
        .len   = payload_len,
        .seq   = 0,
        .type  = msg_type,
        .flags = flags
    };
    fge_net_msg_t msg = {
        .header  = header,
        .payload = payload_len > 0 ? buf + 8 : NULL
    };

    emit_event(ctx, FGE_NET_EVENT_DATA, NULL, &addr, &msg, FGE_OK);
}

static void check_timeouts(fge_net_context_t *ctx)
{
    fge_connection_t *conn = ctx->active_list;
    while (conn) {
        fge_connection_t *next = conn->next_active;
        if (conn->state == FGE_CONN_CONNECTED || conn->state == FGE_CONN_CONNECTING) {
            double elapsed = ctx->now - conn->last_recv_time;
            if (elapsed > FGE_NET_TIMEOUT_SEC) {
                FGE_DEBUG(FGE_LOG_CAT_NETWORK, "Connection %u timed out", conn->id);
                emit_event(ctx, FGE_NET_EVENT_DISCONNECT, conn, NULL, NULL, FGE_ERR_TIMEOUT);
                fge_net_close(ctx, conn);
            }
        }
        conn = next;
    }
}

void fge_net_run(fge_net_context_t *ctx)
{
    if (!ctx) return;
    ctx->running = true;
    while (ctx->running) {
        fge_net_poll(ctx, -1);
    }
}

void fge_net_poll(fge_net_context_t *ctx, int timeout_ms)
{
    if (!ctx || ctx->epoll_fd < 0) return;

    ctx->now = fge_clock_elapsed_sec(&ctx->clock);

    struct epoll_event events[FGE_NET_MAX_EVENTS];
    int nfds = epoll_wait(ctx->epoll_fd, events, FGE_NET_MAX_EVENTS, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) return;
        FGE_WARN(FGE_LOG_CAT_NETWORK, "epoll_wait failed: %s", strerror(errno));
        return;
    }

    for (int i = 0; i < nfds; i++) {
        void *ptr = events[i].data.ptr;

        if (ptr == NULL) {
            /* Listener */
            if (events[i].events & EPOLLIN) {
                handle_accept(ctx);
            }
        } else if (ptr == FGE_NET_UDP_SENTINEL) {
            /* UDP */
            if (events[i].events & EPOLLIN) {
                handle_udp(ctx);
            }
        } else {
            fge_connection_t *conn = (fge_connection_t *)ptr;
            if (conn->fd < 0) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (conn->state == FGE_CONN_CONNECTING) {
                    emit_event(ctx, FGE_NET_EVENT_ERROR, conn, NULL, NULL, FGE_ERR_NETWORK);
                } else {
                    emit_event(ctx, FGE_NET_EVENT_DISCONNECT, conn, NULL, NULL, FGE_OK);
                }
                fge_net_close(ctx, conn);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_read(ctx, conn);
                if (conn->fd < 0) continue;
            }

            if (events[i].events & EPOLLOUT) {
                handle_write(ctx, conn);
            }
        }
    }

    check_timeouts(ctx);
}

void fge_net_stop(fge_net_context_t *ctx)
{
    if (ctx) ctx->running = false;
}

/* -------------------------------------------------------------------------- */
/* Socket helpers                                                             */
/* -------------------------------------------------------------------------- */

bool fge_net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool fge_net_set_nodelay(int fd)
{
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}
