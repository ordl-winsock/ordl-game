/*
 * forge/net.h — High-performance networking for real-time MMORPG
 *
 * Features:
 *   - Raw TCP and UDP sockets (no libcurl, no boost.asio)
 *   - Non-blocking I/O with epoll (Linux) / kqueue (BSD/macOS) / select (fallback)
 *   - Message framing for stream protocols
 *   - Connection pool with heartbeat / timeout
 *   - Lock-free send/receive queues per connection
 *   - Async event loop (single thread, non-blocking)
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_NET_H
#define FORGE_NET_H

#include "forge/core.h"
#include "forge/time.h"
#include "forge/memory.h"
#include "forge/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

#define FGE_NET_MAX_CONNECTIONS   4096
#define FGE_NET_MAX_EVENTS        1024
#define FGE_NET_RECV_BUF_SIZE     FGE_KIB(64)
#define FGE_NET_SEND_BUF_SIZE     FGE_KIB(64)
#define FGE_NET_MAX_MSG_SIZE      FGE_KIB(16)
#define FGE_NET_DEFAULT_PORT      7777
#define FGE_NET_HEARTBEAT_INTERVAL_SEC 15.0
#define FGE_NET_TIMEOUT_SEC       60.0

/* -------------------------------------------------------------------------- */
/* Address                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    union {
        struct sockaddr_in  v4;
        struct sockaddr_in6 v6;
        struct sockaddr     addr;
    };
    socklen_t len;
} fge_net_addr_t;

bool fge_net_addr_from_string(fge_net_addr_t *out, const char *host, uint16_t port);
bool fge_net_addr_from_sockaddr(fge_net_addr_t *out, const struct sockaddr *sa, socklen_t len);
const char *fge_net_addr_to_string(const fge_net_addr_t *addr, char *buf, size_t buf_size);

/* -------------------------------------------------------------------------- */
/* Message framing — length-prefixed binary messages                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t len;      /* payload length, network byte order */
    uint32_t seq;      /* sequence number for reliability */
    uint16_t type;     /* message type */
    uint16_t flags;    /* compression, encryption, etc. */
} fge_net_msg_header_t;

#define FGE_NET_FLAG_COMPRESSED (1u << 0)
#define FGE_NET_FLAG_ENCRYPTED  (1u << 1)
#define FGE_NET_FLAG_RELIABLE   (1u << 2)
#define FGE_NET_FLAG_ACK        (1u << 3)

typedef struct {
    fge_net_msg_header_t header;
    uint8_t *payload;
} fge_net_msg_t;

/* -------------------------------------------------------------------------- */
/* Buffer — dynamic byte buffer for socket I/O                                */
/* -------------------------------------------------------------------------- */

/* Buffer — ring buffer for socket I/O (zero-copy, no compaction) */
typedef struct {
    uint8_t *data;
    size_t size;      /* capacity */
    size_t head;      /* monotonic write position */
    size_t tail;      /* monotonic read position */
} fge_net_buffer_t;

bool fge_net_buffer_init(fge_net_buffer_t *b, size_t initial_size);
void fge_net_buffer_free(fge_net_buffer_t *b);
void fge_net_buffer_reset(fge_net_buffer_t *b);
bool fge_net_buffer_ensure(fge_net_buffer_t *b, size_t need);
size_t fge_net_buffer_readable(const fge_net_buffer_t *b);
size_t fge_net_buffer_writable(const fge_net_buffer_t *b);
uint8_t *fge_net_buffer_write_ptr(fge_net_buffer_t *b);
void fge_net_buffer_advance_write(fge_net_buffer_t *b, size_t n);
uint8_t *fge_net_buffer_read_ptr(const fge_net_buffer_t *b);
void fge_net_buffer_advance_read(fge_net_buffer_t *b, size_t n);
bool fge_net_buffer_push(fge_net_buffer_t *b, const uint8_t *data, size_t len);
/* Read up to len bytes into out, handling wrap. Returns bytes read. */
size_t fge_net_buffer_peek(const fge_net_buffer_t *b, uint8_t *out, size_t len);

/* -------------------------------------------------------------------------- */
/* Connection                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_CONN_DISCONNECTED = 0,
    FGE_CONN_CONNECTING,
    FGE_CONN_CONNECTED,
    FGE_CONN_CLOSING,
} fge_conn_state_t;

typedef struct fge_connection fge_connection_t;

struct fge_connection {
    int fd;
    fge_conn_state_t state;
    fge_net_addr_t local_addr;
    fge_net_addr_t remote_addr;

    /* Buffers */
    fge_net_buffer_t recv_buf;
    fge_net_buffer_t send_buf;

    /* Timing */
    double last_recv_time;
    double last_send_time;
    double connect_time;

    /* Statistics */
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t msgs_received;
    uint64_t msgs_sent;

    /* User data */
    void *user_data;
    uint32_t id;

    /* Pool linkage */
    fge_connection_t *next_free;

    /* Active list linkage (for O(active) iteration) */
    fge_connection_t *next_active;
    fge_connection_t *prev_active;
};

const char *fge_conn_state_str(fge_conn_state_t s);

/* -------------------------------------------------------------------------- */
/* Event types                                                                */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_NET_EVENT_CONNECT,
    FGE_NET_EVENT_DISCONNECT,
    FGE_NET_EVENT_DATA,
    FGE_NET_EVENT_ERROR,
    FGE_NET_EVENT_ACCEPT,
} fge_net_event_type_t;

typedef struct {
    fge_net_event_type_t type;
    fge_connection_t *conn;
    fge_net_addr_t addr;
    fge_net_msg_t msg;
    fge_result_t error;
} fge_net_event_t;

typedef void (*fge_net_event_cb_t)(const fge_net_event_t *event, void *userdata);

/* -------------------------------------------------------------------------- */
/* Network context / event loop                                               */
/* -------------------------------------------------------------------------- */

typedef struct fge_net_context fge_net_context_t;

struct fge_net_context {
    int epoll_fd;               /* Linux epoll, or -1 for select fallback */
    int listen_fd;              /* TCP listener, or -1 */
    int udp_fd;                 /* UDP socket, or -1 */
    uint16_t listen_port;

    /* Connection pool */
    fge_connection_t connections[FGE_NET_MAX_CONNECTIONS];
    fge_connection_t *free_list;
    fge_connection_t *active_list;   /* doubly-linked list of active conns */
    uint32_t active_count;
    uint32_t next_conn_id;
    fge_spinlock_t conn_lock;

    /* Event callback */
    fge_net_event_cb_t event_cb;
    void *event_userdata;

    /* Timing */
    fge_clock_t clock;
    double now;

    /* Running flag */
    bool running;
};

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

[[nodiscard]]
fge_net_context_t *fge_net_create(fge_net_event_cb_t cb, void *userdata);
void fge_net_destroy(fge_net_context_t *ctx);

/* TCP server */
bool fge_net_listen(fge_net_context_t *ctx, uint16_t port);

/* TCP client */
fge_connection_t *fge_net_connect(fge_net_context_t *ctx, const char *host, uint16_t port);

/* UDP socket */
bool fge_net_bind_udp(fge_net_context_t *ctx, uint16_t port);

/* Close connection */
void fge_net_close(fge_net_context_t *ctx, fge_connection_t *conn);

/* Send framed message */
bool fge_net_send(fge_net_context_t *ctx, fge_connection_t *conn, uint16_t msg_type,
                  const uint8_t *payload, uint32_t payload_len);

/* Send unreliable UDP message */
bool fge_net_send_udp(fge_net_context_t *ctx, const fge_net_addr_t *addr,
                      uint16_t msg_type, const uint8_t *payload, uint32_t payload_len);

/* Event loop — single thread, blocks until fge_net_stop() */
void fge_net_run(fge_net_context_t *ctx);

/* Single iteration (for integration with external loops) */
void fge_net_poll(fge_net_context_t *ctx, int timeout_ms);

/* Stop event loop */
void fge_net_stop(fge_net_context_t *ctx);

/* Set socket non-blocking */
bool fge_net_set_nonblocking(int fd);

/* Disable Nagle's algorithm for low latency */
bool fge_net_set_nodelay(int fd);

#endif /* FORGE_NET_H */
