#include "../include/config.h"
#include "../include/http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

static void conf_defaults(struct conf_types* c) {
    memset(c, 0, sizeof(*c));
    c->tcpls_port = TCPLS_PORT_DEFAULT;
    c->is_logging = 1;
    c->max_workers = 8;
    c->max_queue = 128;
    c->max_headers = 64;
    c->max_header_size = 16384;
    c->max_body_size = 16 * 1024 * 1024;
    c->max_request_size = 0;
    c->request_timeout = 15;
    c->listen_backlog = 128;
    c->max_chunk_line_size = 128;
    c->max_trailer_size = 65536;
    c->reverse_proxy = 0;
    c->directory_serve = 1;
    c->directory_listing = 0;
    c->send_buffer_size = 0;
    c->recv_buffer_size = 0;
    strcpy(c->tcpls_host, TCPLS_HOST_DEFAULT);
    strcpy(c->logs_path, LOGS_PATH_DEFAULT);
    strcpy(c->document_root, "./www");
    strcpy(c->proxy_host, "127.0.0.1");
    c->proxy_port = 8081;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int parse_conf(char* config_file_path, struct conf_types* server_conf) {
    conf_defaults(server_conf);
    FILE* config_file = fopen(config_file_path, "r");
    if (config_file == NULL) return PARSE_ERROR_FILE_NOT_EXIST;

    char line[1024];
    int nline = 0, result = 0;
    while (fgets(line, sizeof(line), config_file) != NULL) {
        nline++;
        char* p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        char* eq = strchr(p, '=');
        if (eq == NULL) { result = nline; break; }
        *eq++ = '\0';
        char* key = trim(p);
        char* value = trim(eq);
        if (*key == '\0' || *value == '\0') { result = nline; break; }

        char* end = NULL;
        errno = 0;
        unsigned long long num = strtoull(value, &end, 10);
        int numeric = errno == 0 && end != value && *end == '\0';

        if (!strcmp(key, VAR_TCPLS_HOST)) {
            if (strlen(value) >= sizeof(server_conf->tcpls_host)) result = nline;
            else strcpy(server_conf->tcpls_host, value);
        } else if (!strcmp(key, VAR_TCPLS_PORT)) {
            if (!numeric || num < 1 || num > 65535) result = nline;
            else server_conf->tcpls_port = (unsigned short)num;
        } else if (!strcmp(key, VAR_LOGS_PATH)) {
            if (strlen(value) >= sizeof(server_conf->logs_path)) result = nline;
            else strcpy(server_conf->logs_path, value);
        } else if (!strcmp(key, VAR_ALLOW_LOG)) {
            if (!numeric || num > 1) result = nline;
            else server_conf->is_logging = (unsigned int)num;
        } else if (!strcmp(key, "max-workers")) {
            if (!numeric || num < 1 || num > 1024) result = nline;
            else server_conf->max_workers = (unsigned int)num;
        } else if (!strcmp(key, "max-queue")) {
            if (!numeric || num < 1 || num > 65536) result = nline;
            else server_conf->max_queue = (unsigned int)num;
        } else if (!strcmp(key, "max-headers")) {
            if (!numeric || num < 1 || num > MAX_HEADERS) result = nline;
            else server_conf->max_headers = (unsigned int)num;
        } else if (!strcmp(key, "max-header-size")) {
            if (!numeric || num < 1024 || num > 1048576) result = nline;
            else server_conf->max_header_size = (unsigned int)num;
        } else if (!strcmp(key, "max-body-size")) {
            if (!numeric || num < 1 || num > 1073741824ULL) result = nline;
            else server_conf->max_body_size = num;
        } else if (!strcmp(key, "max-request-size")) {
            if (!numeric || num > 2147483648ULL) result = nline;
            else server_conf->max_request_size = num;
        } else if (!strcmp(key, "request-timeout")) {
            if (!numeric || num < 1 || num > 3600) result = nline;
            else server_conf->request_timeout = (unsigned int)num;
        } else if (!strcmp(key, "listen-backlog")) {
            if (!numeric || num < 1 || num > 65535) result = nline;
            else server_conf->listen_backlog = (unsigned int)num;
        } else if (!strcmp(key, "max-chunk-line-size")) {
            if (!numeric || num < 1 || num > 4096) result = nline;
            else server_conf->max_chunk_line_size = (unsigned int)num;
        } else if (!strcmp(key, "max-trailer-size")) {
            if (!numeric || num < 2 || num > 1048576) result = nline;
            else server_conf->max_trailer_size = (unsigned int)num;
        } else if (!strcmp(key, "reverse-proxy")) {
            if (!numeric || num > 1) result = nline;
            else server_conf->reverse_proxy = (unsigned int)num;
        } else if (!strcmp(key, "directory-serve")) {
            if (!numeric || num > 1) result = nline;
            else server_conf->directory_serve = (unsigned int)num;
        } else if (!strcmp(key, "directory-listing")) {
            if (!numeric || num > 1) result = nline;
            else server_conf->directory_listing = (unsigned int)num;
        } else if (!strcmp(key, "document-root")) {
            if (strlen(value) >= sizeof(server_conf->document_root)) result = nline;
            else strcpy(server_conf->document_root, value);
        } else if (!strcmp(key, "proxy-host")) {
            if (strlen(value) >= sizeof(server_conf->proxy_host)) result = nline;
            else strcpy(server_conf->proxy_host, value);
        } else if (!strcmp(key, "proxy-port")) {
            if (!numeric || num < 1 || num > 65535) result = nline;
            else server_conf->proxy_port = (unsigned short)num;
        } else if (!strcmp(key, "send-buffer-size")) {
            if (!numeric || num > 16777216) result = nline;
            else server_conf->send_buffer_size = (unsigned int)num;
        } else if (!strcmp(key, "recv-buffer-size")) {
            if (!numeric || num > 16777216) result = nline;
            else server_conf->recv_buffer_size = (unsigned int)num;
        } else {
            result = nline;
        }
        if (result) break;
    }
    if (ferror(config_file) && result == 0) result = nline ? nline : 1;
    fclose(config_file);
    return result;
}
