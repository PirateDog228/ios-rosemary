/*
 * rosemary-inspectord: small read-only TCP relay for Rosemary Inspector.
 *
 * The daemon exposes only three fixed protocol operations. It does not run
 * commands, open arbitrary paths, or accept file uploads. Connections are
 * restricted to the USB-ECM host address 192.168.7.1 by default.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#else
/* Permit syntax checking on Linux; the real binary is built with the iOS SDK. */
static int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    errno = ENOTSUP;
    return -1;
}
#endif

#define RI_VERSION              1u
#define RI_OP_INFO              1u
#define RI_OP_READ_LOG          2u
#define RI_MAX_LOG_READ         (32u * 1024u)
#define RI_DEFAULT_BIND         "192.168.7.2"
#define RI_DEFAULT_PEER         "192.168.7.1"
#define RI_DEFAULT_PORT         9271
#define RI_WIRE_REQUEST_SIZE    20u
#define RI_WIRE_RESPONSE_SIZE   40u
#define RI_SYSCTL_READLOG       "kern.rosemary.inspector.readlog"
#define RI_SYSCTL_LOG_POSITION  "kern.rosemary.inspector.log_position"
#define RI_SYSCTL_LOG_CAPACITY  "kern.rosemary.inspector.log_capacity"
#define RI_SYSCTL_PANIC_ACTIVE  "kern.rosemary.inspector.panic_active"
#define RI_SYSCTL_VERSION       "kern.rosemary.inspector.protocol_version"

#define RI_KLOG_F_OVERRUN       0x00000001u
#define RI_KLOG_F_MORE          0x00000002u

struct kernel_read_request {
    uint64_t cursor;
    uint32_t max_bytes;
    uint32_t reserved;
};

struct kernel_read_reply {
    uint64_t cursor;
    uint64_t next_cursor;
    uint64_t total;
    uint64_t oldest;
    uint32_t bytes;
    uint32_t flags;
    uint8_t data[];
};

static uint16_t
read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t
read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t
read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void
write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void
write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void
write_be64(uint8_t *p, uint64_t v)
{
    write_be32(p, (uint32_t)(v >> 32));
    write_be32(p + 4, (uint32_t)v);
}

static int
read_full(int fd, void *buffer, size_t length)
{
    uint8_t *p = (uint8_t *)buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = recv(fd, p + done, length - done, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 1;
}

static int
write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *p = (const uint8_t *)buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = send(fd, p + done, length - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static uint64_t
read_u64_sysctl(const char *name)
{
    uint64_t value = 0;
    size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, NULL, 0) != 0 || length != sizeof(value))
        return 0;
    return value;
}

static uint32_t
read_u32_sysctl(const char *name)
{
    uint32_t value = 0;
    size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, NULL, 0) != 0 || length != sizeof(value))
        return 0;
    return value;
}

static void
json_escape(char *out, size_t out_size, const char *in)
{
    size_t used = 0;
    if (out_size == 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p && used + 2 < out_size; ++p) {
        if (*p == '"' || *p == '\\') {
            out[used++] = '\\';
            out[used++] = (char)*p;
        } else if (*p >= 0x20) {
            out[used++] = (char)*p;
        } else if (*p == '\n' && used + 6 < out_size) {
            out[used++] = '\\'; out[used++] = 'n';
        }
    }
    out[used] = '\0';
}

static size_t
make_info_json(char *out, size_t capacity)
{
    uint64_t log_total = read_u64_sysctl(RI_SYSCTL_LOG_POSITION);
    uint32_t log_capacity = read_u32_sysctl(RI_SYSCTL_LOG_CAPACITY);
    uint32_t panic_active = read_u32_sysctl(RI_SYSCTL_PANIC_ACTIVE);
    uint32_t protocol_version = read_u32_sysctl(RI_SYSCTL_VERSION);
    uint64_t oldest = log_total > log_capacity ? log_total - log_capacity : 0;
    uint64_t memory_bytes = 0;
    struct timeval boot_time;
    char kernel_version[512] = "unknown";
    size_t length;

    length = sizeof(memory_bytes);
    (void)sysctlbyname("hw.memsize", &memory_bytes, &length, NULL, 0);
    memset(&boot_time, 0, sizeof(boot_time));
    length = sizeof(boot_time);
    (void)sysctlbyname("kern.boottime", &boot_time, &length, NULL, 0);
    length = sizeof(kernel_version) - 1;
    if (sysctlbyname("kern.version", kernel_version, &length, NULL, 0) == 0) {
        if (length >= sizeof(kernel_version)) length = sizeof(kernel_version) - 1;
        kernel_version[length] = '\0';
    }

    char escaped_version[1024];
    json_escape(escaped_version, sizeof(escaped_version), kernel_version);
    int n = snprintf(out, capacity,
        "{\"agent\":\"rosemary-inspectord\",\"agent_version\":\"0.1.0\","
        "\"protocol_version\":%u,\"device\":\"rosemary\","
        "\"log_capacity\":%u,\"log_total\":%llu,\"log_oldest\":%llu,"
        "\"panic_active\":%s,\"memory_bytes\":%llu,\"boot_time_sec\":%lld,"
        "\"kernel_version\":\"%s\"}",
        protocol_version, log_capacity,
        (unsigned long long)log_total, (unsigned long long)oldest,
        panic_active ? "true" : "false", (unsigned long long)memory_bytes,
        (long long)boot_time.tv_sec, escaped_version);
    if (n < 0) return 0;
    return (size_t)n < capacity ? (size_t)n : capacity;
}

static int
send_response(int fd, uint16_t status, uint64_t next_cursor, uint64_t total,
    uint64_t oldest, uint32_t flags, const void *payload, uint32_t payload_length)
{
    uint8_t header[RI_WIRE_RESPONSE_SIZE];
    memset(header, 0, sizeof(header));
    memcpy(header, "RISP", 4);
    write_be16(header + 4, RI_VERSION);
    write_be16(header + 6, status);
    write_be64(header + 8, next_cursor);
    write_be64(header + 16, total);
    write_be64(header + 24, oldest);
    write_be32(header + 32, payload_length);
    write_be32(header + 36, flags);
    if (write_full(fd, header, sizeof(header)) != 0) return -1;
    if (payload_length && write_full(fd, payload, payload_length) != 0) return -1;
    return 0;
}

static int
handle_info(int fd)
{
    char json[2048];
    size_t length = make_info_json(json, sizeof(json));
    return send_response(fd, 0, 0, 0, 0, 0, json, (uint32_t)length);
}

static int
handle_log(int fd, uint64_t cursor, uint32_t requested)
{
    uint32_t max_bytes = requested ? requested : 16384u;
    if (max_bytes > RI_MAX_LOG_READ) max_bytes = RI_MAX_LOG_READ;

    size_t capacity = sizeof(struct kernel_read_reply) + max_bytes;
    struct kernel_read_reply *reply = (struct kernel_read_reply *)calloc(1, capacity);
    if (reply == NULL) return -1;
    struct kernel_read_request query = { .cursor = cursor, .max_bytes = max_bytes, .reserved = 0 };
    size_t reply_length = capacity;
    int result = sysctlbyname(RI_SYSCTL_READLOG, reply, &reply_length,
        &query, sizeof(query));
    if (result != 0 || reply_length < sizeof(*reply) ||
        reply->bytes > max_bytes || reply_length < sizeof(*reply) + reply->bytes) {
        char message[160];
        int e = errno;
        int n = snprintf(message, sizeof(message), "readlog sysctl failed: errno=%d", e);
        if (n < 0) n = 0;
        (void)send_response(fd, 1, cursor, 0, 0, 0, message, (uint32_t)n);
        free(reply);
        return 0;
    }
    int sent = send_response(fd, 0, reply->next_cursor, reply->total,
        reply->oldest, reply->flags, reply->data, reply->bytes);
    free(reply);
    return sent;
}

static int
serve_client(int fd)
{
    for (;;) {
        uint8_t request[RI_WIRE_REQUEST_SIZE];
        int rr = read_full(fd, request, sizeof(request));
        if (rr <= 0) return rr;
        if (memcmp(request, "RINQ", 4) != 0 || read_be16(request + 4) != RI_VERSION) {
            static const char bad[] = "unsupported request";
            (void)send_response(fd, 2, 0, 0, 0, 0, bad, (uint32_t)(sizeof(bad) - 1));
            return -1;
        }
        uint16_t operation = read_be16(request + 6);
        uint64_t cursor = read_be64(request + 8);
        uint32_t max_bytes = read_be32(request + 16);
        int result;
        if (operation == RI_OP_INFO) {
            result = handle_info(fd);
        } else if (operation == RI_OP_READ_LOG) {
            result = handle_log(fd, cursor, max_bytes);
        } else {
            static const char bad[] = "unknown operation";
            result = send_response(fd, 3, 0, 0, 0, 0, bad, (uint32_t)(sizeof(bad) - 1));
        }
        if (result != 0) return -1;
    }
}

static void
usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--bind 192.168.7.2] [--peer 192.168.7.1] [--port 9271]\n", program);
}

int
main(int argc, char **argv)
{
    const char *bind_address = RI_DEFAULT_BIND;
    const char *allowed_peer = RI_DEFAULT_PEER;
    unsigned long port = RI_DEFAULT_PORT;
    struct in_addr bind_ip, peer_ip;
    int server;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_address = argv[++i];
        else if (!strcmp(argv[i], "--peer") && i + 1 < argc) allowed_peer = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end = NULL;
            port = strtoul(argv[++i], &end, 10);
            if (!end || *end || port == 0 || port > 65535) { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }

    if (inet_pton(AF_INET, bind_address, &bind_ip) != 1 ||
        inet_pton(AF_INET, allowed_peer, &peer_ip) != 1) {
        fprintf(stderr, "invalid IPv4 address\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "rosemary-inspectord must run as root\n");
        return 1;
    }
    if (read_u32_sysctl(RI_SYSCTL_VERSION) != RI_VERSION) {
        fprintf(stderr, "Rosemary Inspector kernel API v1 is not available; boot the kernel with patch 0181 first\n");
        return 1;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }
    int yes = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    (void)setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
#ifdef __APPLE__
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr = bind_ip;
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind"); close(server); return 1;
    }
    if (listen(server, 4) != 0) {
        perror("listen"); close(server); return 1;
    }

    printf("rosemary-inspectord 0.1.0 listening on %s:%lu (peer %s only)\n",
        bind_address, port, allowed_peer);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        int fd = accept(server, (struct sockaddr *)&client, &client_length);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }
        if (client.sin_addr.s_addr != peer_ip.s_addr) {
            close(fd);
            continue;
        }
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        (void)serve_client(fd);
        close(fd);
    }
}
