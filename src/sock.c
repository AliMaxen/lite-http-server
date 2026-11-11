#define _GNU_SOURCE
#include "../include/sock.h"
#include "../include/config.h"
#include "../include/logger.h"
#include "../include/http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <time.h>

struct worker_queue {
    int *fds;
    unsigned int cap, head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
};

static struct worker_queue queue;
static struct conf_types server_conf;

static void server_log(int status, const char* mod, const char* message) {
    struct log_data logd;
    if (!mklog(&server_conf, &logd, status, (char*)mod, (char*)message)) flog(&logd);
}

static int send_all(int fd, const void* data, size_t len) {
    const char* p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int buffer_append(char* out, size_t cap, size_t* used, const char* value, size_t len) {
    if (len > cap - *used) return -1;
    memcpy(out + *used, value, len);
    *used += len;
    return 0;
}

static void send_error_method(int fd, int code, const char* reason, const char* message, int head_only) {
    char log[128];
    snprintf(log, sizeof(log), "Responded %d %s", code, reason);
    server_log(code >= 400 ? LOG_STAT_WARNING : LOG_STAT_INFO, "HTTP", log);
    char head[512];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, reason, strlen(message));
    if (n > 0) send_all(fd, head, (size_t)n);
    if (!head_only) send_all(fd, message, strlen(message));
}

static void send_error(int fd, int code, const char* reason, const char* message) {
    send_error_method(fd, code, reason, message, 0);
}

static const char* mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css")) return "text/css";
    if (!strcasecmp(ext, ".js")) return "application/javascript";
    if (!strcasecmp(ext, ".json")) return "application/json";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif")) return "image/gif";
    if (!strcasecmp(ext, ".svg")) return "image/svg+xml";
    if (!strcasecmp(ext, ".pdf")) return "application/pdf";
    if (!strcasecmp(ext, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static int path_inside(const char* root, const char* path) {
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/') return path[0] == '/';
    return strncmp(root, path, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static size_t text_append(char* out, size_t cap, size_t used, const char* value) {
    size_t n = strlen(value);
    if (used >= cap || n >= cap - used) return cap;
    memcpy(out + used, value, n);
    return used + n;
}

static size_t html_append(char* out, size_t cap, size_t used, const char* value) {
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        const char* escaped = NULL;
        switch (*p) {
            case '&': escaped = "&amp;"; break;
            case '<': escaped = "&lt;"; break;
            case '>': escaped = "&gt;"; break;
            case '"': escaped = "&quot;"; break;
            case '\'': escaped = "&#39;"; break;
            default: break;
        }
        size_t n = escaped ? strlen(escaped) : 1;
        if (used + n >= cap) return used;
        if (escaped) memcpy(out + used, escaped, n);
        else out[used] = (char)*p;
        used += n;
    }
    return used;
}

static size_t url_append(char* out, size_t cap, size_t used, const char* value) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        unsigned char c = *p;
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) {
            if (used + 1 >= cap) return used;
            out[used++] = (char)c;
        } else {
            if (used + 3 >= cap) return used;
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
        }
    }
    return used;
}

static void serve_directory(int fd, const char* path, const char* url, int head_only) {
    DIR* dir = opendir(path);
    if (!dir) { send_error_method(fd, 403, "Forbidden", "Access denied.\n", head_only); return; }
    char body[32768];
    size_t used = (size_t)snprintf(body, sizeof(body), "<!doctype html><html><body><h1>Directory index</h1><ul>");
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && used + 256 < sizeof(body)) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        used = text_append(body, sizeof(body), used, "<li><a href=\"");
        if (used >= sizeof(body) - 1) break;
        used = url_append(body, sizeof(body), used, url);
        if (used >= sizeof(body) - 1) break;
        if (url[strlen(url)-1] != '/') {
            if (used + 1 >= sizeof(body)) break;
            body[used++] = '/';
        }
        used = url_append(body, sizeof(body), used, ent->d_name);
        if (used >= sizeof(body) - 1) break;
        used = text_append(body, sizeof(body), used, "\">");
        used = html_append(body, sizeof(body), used, ent->d_name);
        if (used >= sizeof(body) - 1) break;
        used = text_append(body, sizeof(body), used, "</a></li>");
        if (used >= sizeof(body) - 1) break;
    }
    closedir(dir);
    used = text_append(body, sizeof(body), used, "</ul></body></html>");
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", used);
    send_all(fd, head, (size_t)n);
    if (!head_only) send_all(fd, body, used);
    server_log(LOG_STAT_INFO, "HTTP", "Served directory index");
}

static void serve_file(int fd, struct rq_http* rq) {
    if (strcmp(rq->method, "GET") && strcmp(rq->method, "HEAD")) {
        send_error_method(fd, 405, "Method Not Allowed", "Only GET and HEAD are supported.\n", !strcmp(rq->method, "HEAD"));
        return;
    }
    char* query = strchr(rq->target, '?');
    if (query) *query = '\0';
    if (rq->target[0] != '/' || strstr(rq->target, "\\") || strstr(rq->target, "..")) {
        send_error_method(fd, 400, "Bad Request", "Invalid request path.\n", !strcmp(rq->method, "HEAD"));
        return;
    }
    char root[PATH_MAX], requested[PATH_MAX], resolved[PATH_MAX];
    if (!realpath(server_conf.document_root, root)) {
        send_error_method(fd, 500, "Internal Server Error", "Document root is unavailable.\n", !strcmp(rq->method, "HEAD"));
        return;
    }
    int n = snprintf(requested, sizeof(requested), "%s%s", root, rq->target);
    if (n < 0 || (size_t)n >= sizeof(requested) || !realpath(requested, resolved) || !path_inside(root, resolved)) {
        send_error_method(fd, 404, "Not Found", "Resource not found.\n", !strcmp(rq->method, "HEAD"));
        return;
    }
    struct stat st;
    if (stat(resolved, &st) == -1) { send_error_method(fd, 404, "Not Found", "Resource not found.\n", !strcmp(rq->method, "HEAD")); return; }
    if (S_ISDIR(st.st_mode)) {
        if (!server_conf.directory_serve) {
            send_error_method(fd, 403, "Forbidden", "Directory access is disabled.\n", !strcmp(rq->method, "HEAD"));
            return;
        }
        size_t l = strlen(resolved);
        if (l + 11 < sizeof(requested)) {
            snprintf(requested, sizeof(requested), "%s/index.html", resolved);
            char index_path[PATH_MAX];
            if (realpath(requested, index_path) && path_inside(root, index_path) &&
                    stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) strcpy(resolved, index_path);
            else if (server_conf.directory_listing) {
                serve_directory(fd, resolved, rq->target, !strcmp(rq->method, "HEAD"));
                return;
            } else {
                send_error_method(fd, 403, "Forbidden", "Directory listing is disabled.\n", !strcmp(rq->method, "HEAD"));
                return;
            }
        } else { send_error_method(fd, 404, "Not Found", "Resource not found.\n", !strcmp(rq->method, "HEAD")); return; }
    }
    if (!S_ISREG(st.st_mode)) { send_error_method(fd, 403, "Forbidden", "Access denied.\n", !strcmp(rq->method, "HEAD")); return; }
    int file = open(resolved, O_RDONLY | O_CLOEXEC);
    if (file == -1) { send_error_method(fd, 403, "Forbidden", "Access denied.\n", !strcmp(rq->method, "HEAD")); return; }
    char head[512];
    n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
            mime_type(resolved), (long long)st.st_size);
    if (n > 0) send_all(fd, head, (size_t)n);
    server_log(LOG_STAT_INFO, "HTTP", "Served static resource");
    if (strcmp(rq->method, "HEAD")) {
        off_t off = 0;
        while (off < st.st_size) {
            ssize_t sent = sendfile(fd, file, &off, 1024 * 1024);
            if (sent < 0) { if (errno == EINTR) continue; break; }
            if (sent == 0) break;
        }
    }
    close(file);
}
static void proxy_request(int fd, struct rq_http* rq) {
    struct addrinfo hints = {0}, *res = NULL, *it;
    char port[8];
    snprintf(port, sizeof(port), "%u", server_conf.proxy_port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(server_conf.proxy_host, port, &hints, &res) != 0) {
        send_error(fd, 502, "Bad Gateway", "Cannot resolve upstream server.\n");
        return;
    }
    int upstream = -1;
    for (it = res; it; it = it->ai_next) {
        upstream = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (upstream >= 0 && connect(upstream, it->ai_addr, it->ai_addrlen) == 0) break;
        if (upstream >= 0) close(upstream);
        upstream = -1;
    }
    freeaddrinfo(res);
    if (upstream < 0) { send_error(fd, 502, "Bad Gateway", "Cannot connect to upstream server.\n"); return; }
    struct timeval tout = { .tv_sec = (int)server_conf.request_timeout, .tv_usec = 0 };
    setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, &tout, sizeof(tout));
    setsockopt(upstream, SOL_SOCKET, SO_SNDTIMEO, &tout, sizeof(tout));
    size_t head_cap = rq->header_len + 64, head_used = 0;
    char* head = malloc(head_cap);
    if (head == NULL) { close(upstream); return; }
    int head_error = 0;
    head_error |= buffer_append(head, head_cap, &head_used, rq->method, strlen(rq->method));
    head_error |= buffer_append(head, head_cap, &head_used, " ", 1);
    head_error |= buffer_append(head, head_cap, &head_used, rq->target, strlen(rq->target));
    head_error |= buffer_append(head, head_cap, &head_used, " ", 1);
    head_error |= buffer_append(head, head_cap, &head_used, rq->version, strlen(rq->version));
    head_error |= buffer_append(head, head_cap, &head_used, "\r\n", 2);
    for (int i = 0; i < rq->nheader && !head_error; i++) {
        if (!strcasecmp(rq->headers[i].name, "Connection") ||
                !strcasecmp(rq->headers[i].name, "Proxy-Connection") ||
                !strcasecmp(rq->headers[i].name, "Keep-Alive") ||
                !strcasecmp(rq->headers[i].name, "Upgrade") ||
                !strcasecmp(rq->headers[i].name, "TE") ||
                !strcasecmp(rq->headers[i].name, "Trailer") ||
                !strcasecmp(rq->headers[i].name, "Expect")) continue;
        head_error |= buffer_append(head, head_cap, &head_used, rq->headers[i].name, strlen(rq->headers[i].name));
        head_error |= buffer_append(head, head_cap, &head_used, ": ", 2);
        head_error |= buffer_append(head, head_cap, &head_used, rq->headers[i].value, strlen(rq->headers[i].value));
        head_error |= buffer_append(head, head_cap, &head_used, "\r\n", 2);
    }
    head_error |= buffer_append(head, head_cap, &head_used, "Connection: close\r\n\r\n", 21);
    if (head_error || send_all(upstream, head, head_used) == -1) {
        free(head);
        close(upstream);
        return;
    }
    free(head);
    size_t wire_body_len = rq->body_type == 1 ? (size_t)rq->cont_len :
            rq->body_type == 2 ? rq->chunk_pos - rq->header_len : 0;
    if (send_all(upstream, rq->raw + rq->header_len, wire_body_len) == -1) {
        close(upstream);
        return;
    }
    char buff[16384];
    while (1) {
        ssize_t n = recv(upstream, buff, sizeof(buff), 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        if (send_all(fd, buff, (size_t)n) == -1) break;
    }
    close(upstream);
}

static void handle_client(int fd) {
    struct timeval tout = { .tv_sec = (int)server_conf.request_timeout, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tout, sizeof(tout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tout, sizeof(tout));
    if (server_conf.recv_buffer_size) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &server_conf.recv_buffer_size, sizeof(int));
    if (server_conf.send_buffer_size) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &server_conf.send_buffer_size, sizeof(int));

    struct rq_http rq = {0};
    rq.max_body_size = (size_t)server_conf.max_body_size;
    rq.max_request_size = (size_t)server_conf.max_request_size;
    rq.max_header_size = server_conf.max_header_size;
    rq.max_headers = server_conf.max_headers;
    rq.max_chunk_line_size = server_conf.max_chunk_line_size;
    rq.max_trailer_size = server_conf.max_trailer_size;
    char buff[16384];
    int result = HTTP_PARSER_BUFF_PENDING;
    int expect_checked = 0, expect_rejected = 0;
    while (result == HTTP_PARSER_BUFF_PENDING) {
        ssize_t n = recv(fd, buff, sizeof(buff), 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        result = http_parser(&rq, buff, (size_t)n);
        if (rq.headers_done && !expect_checked) {
            expect_checked = 1;
            for (int i = 0; i < rq.nheader; i++) {
                if (!strcasecmp(rq.headers[i].name, "Expect")) {
                    if (strcasecmp(rq.headers[i].value, "100-continue") == 0) {
                        if (result == HTTP_PARSER_BUFF_PENDING) send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
                    } else {
                        expect_rejected = 1;
                    }
                    break;
                }
            }
            if (expect_rejected) break;
        }
    }
    if (expect_rejected) {
        send_error(fd, 417, "Expectation Failed", "Unsupported expectation.\n");
    } else if (result == PARSER_SUCCESS) {
        char log[512];
        snprintf(log, sizeof(log), "Request %s %s from client", rq.method, rq.target);
        server_log(LOG_STAT_INFO, "HTTP", log);
        if (server_conf.reverse_proxy) proxy_request(fd, &rq);
        else serve_file(fd, &rq);
    } else if (result == PARSER_TOO_LARGE) {
        send_error(fd, 413, "Payload Too Large", "Request exceeds configured limits.\n");
        server_log(LOG_STAT_WARNING, "HTTP", "Rejected request exceeding configured limits");
    } else if (result == PARSER_UNSUPPORTED_PROTOCOL || result == PARSER_BAD_REQUEST || result == PARSER_EXCEEDING_HEADER_COUNT_MAX) {
        send_error(fd, 400, "Bad Request", "Malformed or unsupported HTTP request.\n");
        server_log(LOG_STAT_WARNING, "HTTP", "Rejected malformed HTTP request");
    }
    http_parser_free(&rq);
    close(fd);
}

static void* worker(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&queue.lock);
        while (queue.count == 0) pthread_cond_wait(&queue.not_empty, &queue.lock);
        int fd = queue.fds[queue.head];
        queue.head = (queue.head + 1) % queue.cap;
        queue.count--;
        pthread_cond_signal(&queue.not_full);
        pthread_mutex_unlock(&queue.lock);
        handle_client(fd);
    }
    return NULL;
}

int sock_up(struct conf_types* cf) {
    server_conf = *cf;
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) { server_log(LOG_STAT_FAILURE, "SER", "Failed to create listening socket"); return -1; }
    int option = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1) {
        server_log(LOG_STAT_FAILURE, "SER", "Failed to set SO_REUSEADDR"); close(sock_fd); return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cf->tcpls_port);
    if (inet_pton(AF_INET, cf->tcpls_host, &addr.sin_addr) != 1) {
        server_log(LOG_STAT_FAILURE, "SER", "Invalid IPv4 listen address"); close(sock_fd); return -1;
    }
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        char msg[256]; snprintf(msg, sizeof(msg), "Failed to bind %s:%u: %s", cf->tcpls_host, cf->tcpls_port, strerror(errno));
        server_log(LOG_STAT_FAILURE, "SER", msg); close(sock_fd); return -1;
    }
    if (listen(sock_fd, (int)cf->listen_backlog) == -1) {
        server_log(LOG_STAT_FAILURE, "SER", "Failed to listen on socket"); close(sock_fd); return -1;
    }

    queue.cap = cf->max_queue;
    queue.fds = calloc(queue.cap, sizeof(int));
    if (!queue.fds) { close(sock_fd); return -1; }
    pthread_mutex_init(&queue.lock, NULL);
    pthread_cond_init(&queue.not_empty, NULL);
    pthread_cond_init(&queue.not_full, NULL);
    pthread_t* workers = calloc(cf->max_workers, sizeof(pthread_t));
    if (!workers) { free(queue.fds); close(sock_fd); return -1; }
    for (unsigned int i = 0; i < cf->max_workers; i++) {
        if (pthread_create(&workers[i], NULL, worker, NULL) != 0) {
            server_log(LOG_STAT_FAILURE, "SER", "Failed to create worker thread");
            free(workers); free(queue.fds); close(sock_fd); return -1;
        }
        pthread_detach(workers[i]);
    }
    free(workers);
    char msg[256]; snprintf(msg, sizeof(msg), "Listening on %s:%u with %u workers and queue size %u",
            cf->tcpls_host, cf->tcpls_port, cf->max_workers, cf->max_queue);
    server_log(LOG_STAT_SUCCESS, "SER", msg);

    while (1) {
        struct sockaddr_in client;
        socklen_t size = sizeof(client);
        int cl_fd = accept(sock_fd, (struct sockaddr*)&client, &size);
        if (cl_fd == -1) { if (errno == EINTR) continue; server_log(LOG_STAT_WARNING, "SER", "Accept failed"); continue; }
        char ip[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        pthread_mutex_lock(&queue.lock);
        while (queue.count == queue.cap) pthread_cond_wait(&queue.not_full, &queue.lock);
        queue.fds[queue.tail] = cl_fd;
        queue.tail = (queue.tail + 1) % queue.cap;
        queue.count++;
        pthread_cond_signal(&queue.not_empty);
        pthread_mutex_unlock(&queue.lock);
        snprintf(msg, sizeof(msg), "Accepted connection from %s", ip);
        server_log(LOG_STAT_INFO, "SER", msg);
    }
    close(sock_fd);
    return 0;
}
