#include "../include/http_parser.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>


static int token_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static char* copy_part(const char* s, size_t n) {
    char* p = malloc(n + 1);
    if (p == NULL) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int find_header_end(const char* s, size_t n, size_t* end) {
    if (n < 4) return 0;
    for (size_t i = 3; i < n; i++) {
        if (s[i-3] == '\r' && s[i-2] == '\n' && s[i-1] == '\r' && s[i] == '\n') {
            *end = i + 1;
            return 1;
        }
    }
    return 0;
}

static int parse_headers(struct rq_http* rq) {
    for (size_t i = 0; i < rq->header_len; i++) {
        if (rq->raw[i] == '\n' && (i == 0 || rq->raw[i-1] != '\r')) return PARSER_BAD_REQUEST;
        if (rq->raw[i] == '\r' && (i + 1 >= rq->header_len || rq->raw[i+1] != '\n')) return PARSER_BAD_REQUEST;
    }
    char* head = copy_part(rq->raw, rq->header_len - 2);
    if (head == NULL) return PARSER_BAD_REQUEST;
    char* save = NULL;
    char* line = strtok_r(head, "\r\n", &save);
    if (line == NULL) { free(head); return PARSER_BAD_REQUEST; }

    char* a = strchr(line, ' ');
    if (a == NULL) { free(head); return PARSER_BAD_REQUEST; }
    *a++ = '\0';
    while (*a == ' ') a++;
    char* b = strchr(a, ' ');
    if (b == NULL) { free(head); return PARSER_BAD_REQUEST; }
    *b++ = '\0';
    while (*b == ' ') b++;
    if (strchr(b, ' ') != NULL || strcmp(b, HTTP_1_1) != 0) {
        free(head);
        return PARSER_UNSUPPORTED_PROTOCOL;
    }
    for (const unsigned char* c = (const unsigned char*)line; *c; c++) {
        if (!token_char(*c)) { free(head); return PARSER_BAD_REQUEST; }
    }
    for (const unsigned char* c = (const unsigned char*)a; *c; c++) {
        if (*c <= 32 || *c == 127) { free(head); return PARSER_BAD_REQUEST; }
    }
    rq->method = strdup(line);
    rq->target = strdup(a);
    rq->version = strdup(b);
    if (!rq->method || !rq->target || !rq->version) { free(head); return PARSER_BAD_REQUEST; }
    rq->is_sup_http = 0;

    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (*line == '\0') continue;
        if (rq->nheader >= (int)(rq->max_headers ? rq->max_headers : MAX_HEADERS) || rq->nheader >= MAX_HEADERS) {
            free(head);
            return PARSER_EXCEEDING_HEADER_COUNT_MAX;
        }
        char* colon = strchr(line, ':');
        if (colon == NULL || colon == line) { free(head); return PARSER_BAD_REQUEST; }
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') colon++;
        char* e = colon + strlen(colon);
        while (e > colon && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        for (char* c = line; *c; c++) {
            if (!token_char((unsigned char)*c)) { free(head); return PARSER_BAD_REQUEST; }
        }
        rq->headers[rq->nheader].name = strdup(line);
        rq->headers[rq->nheader].value = strdup(colon);
        if (!rq->headers[rq->nheader].name || !rq->headers[rq->nheader].value) {
            free(rq->headers[rq->nheader].name);
            free(rq->headers[rq->nheader].value);
            rq->headers[rq->nheader].name = NULL;
            rq->headers[rq->nheader].value = NULL;
            free(head);
            return PARSER_BAD_REQUEST;
        }
        rq->nheader++;
    }
    free(head);

    int has_cl = 0, has_te = 0, host_count = 0;
    unsigned long long content_len = 0;
    for (int i = 0; i < rq->nheader; i++) {
        char* name = rq->headers[i].name;
        char* value = rq->headers[i].value;
        if (!strcasecmp(name, "Host")) {
            host_count++;
            if (*value == '\0') return PARSER_BAD_REQUEST;
        }
        for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
            if ((*c < 32 && *c != '\t') || *c == 127) return PARSER_BAD_REQUEST;
        }
        if (!strcasecmp(name, HTTP_CONTENT_LENGTH_CONSTANT)) {
            for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
                if (!isdigit(*c)) return PARSER_BAD_REQUEST;
            }
            char* end = NULL;
            errno = 0;
            unsigned long long n = strtoull(value, &end, 10);
            if (errno || end == value || *end || (has_cl && n != content_len)) return PARSER_BAD_REQUEST;
            has_cl = 1;
            content_len = n;
        } else if (!strcasecmp(name, HTTP_TRANSFER_ENCODING_CONSTANT)) {
            if (strcasecmp(value, "chunked") != 0 || has_te) return PARSER_BAD_REQUEST;
            has_te = 1;
        }
    }
    if (host_count != 1 || (has_cl && has_te)) return PARSER_BAD_REQUEST;
    if (has_te) rq->body_type = 2;
    else if (has_cl) {
        if (content_len > rq->max_body_size) return PARSER_TOO_LARGE;
        rq->body_type = 1;
        rq->cont_len = (ssize_t)content_len;
    }
    rq->headers_done = 1;
    return PARSER_SUCCESS;
}

static int decode_chunked(struct rq_http* rq) {
    if (rq->body == NULL) {
        rq->chunk_capacity = rq->max_body_size < 4096 ? rq->max_body_size + 1 : 4096;
        if (rq->chunk_capacity == 0) rq->chunk_capacity = 1;
        rq->body = malloc(rq->chunk_capacity);
        if (rq->body == NULL) return PARSER_BAD_REQUEST;
        rq->chunk_pos = rq->header_len;
    }

    while (1) {
        if (rq->chunk_state == 0) {
            size_t line_start = rq->chunk_pos;
            size_t p = line_start;
            while (p + 1 < rq->raw_len && !(rq->raw[p] == '\r' && rq->raw[p+1] == '\n')) p++;
            if (p + 1 >= rq->raw_len) return HTTP_PARSER_BUFF_PENDING;
            size_t line_len = p - line_start;
            if (line_len == 0 || line_len > (rq->max_chunk_line_size ? rq->max_chunk_line_size : 128)) return PARSER_BAD_REQUEST;
            char size_line[129];
            memcpy(size_line, rq->raw + line_start, line_len);
            size_line[line_len] = '\0';
            char* ext = strchr(size_line, ';');
            if (ext) *ext = '\0';
            if (*size_line == '\0') return PARSER_BAD_REQUEST;
            for (char* c = size_line; *c; c++) {
                if (!isxdigit((unsigned char)*c)) return PARSER_BAD_REQUEST;
            }
            char* end = NULL;
            errno = 0;
            unsigned long long chunk_size = strtoull(size_line, &end, 16);
            if (errno || end == size_line || *end || chunk_size > SIZE_MAX) return PARSER_BAD_REQUEST;
            rq->chunk_pos = p + 2;
            if (chunk_size == 0) rq->chunk_state = 3;
            else {
                if (chunk_size > rq->max_body_size - rq->body_len) return PARSER_TOO_LARGE;
                rq->chunk_remaining = (size_t)chunk_size;
                rq->chunk_state = 1;
            }
        } else if (rq->chunk_state == 1) {
            size_t available = rq->raw_len - rq->chunk_pos;
            if (available == 0) return HTTP_PARSER_BUFF_PENDING;
            size_t take = available < rq->chunk_remaining ? available : rq->chunk_remaining;
            size_t needed = rq->body_len + take + 1;
            if (needed > rq->chunk_capacity) {
                size_t capacity = rq->chunk_capacity;
                while (capacity < needed) {
                    size_t grown = capacity > (rq->max_body_size / 2) ? rq->max_body_size + 1 : capacity * 2;
                    if (grown <= capacity) return PARSER_TOO_LARGE;
                    capacity = grown;
                }
                char* next = realloc(rq->body, capacity);
                if (next == NULL) return PARSER_BAD_REQUEST;
                rq->body = next;
                rq->chunk_capacity = capacity;
            }
            memcpy(rq->body + rq->body_len, rq->raw + rq->chunk_pos, take);
            rq->body_len += take;
            rq->chunk_pos += take;
            rq->chunk_remaining -= take;
            if (rq->chunk_remaining) return HTTP_PARSER_BUFF_PENDING;
            rq->chunk_state = 2;
        } else if (rq->chunk_state == 2) {
            if (rq->raw_len - rq->chunk_pos < 2) return HTTP_PARSER_BUFF_PENDING;
            if (rq->raw[rq->chunk_pos] != '\r' || rq->raw[rq->chunk_pos+1] != '\n') return PARSER_BAD_REQUEST;
            rq->chunk_pos += 2;
            rq->chunk_state = 0;
        } else if (rq->chunk_state == 3) {
            size_t p = rq->chunk_pos;
            while (p + 1 < rq->raw_len && !(rq->raw[p] == '\r' && rq->raw[p+1] == '\n')) p++;
            if (p + 1 >= rq->raw_len) return HTTP_PARSER_BUFF_PENDING;
            rq->trailer_size += p - rq->chunk_pos + 2;
            if (rq->trailer_size > (rq->max_trailer_size ? rq->max_trailer_size : 65536)) return PARSER_TOO_LARGE;
            if (p == rq->chunk_pos) {
                rq->chunk_pos = p + 2;
                if (rq->body_len + 1 > rq->chunk_capacity) {
                    char* next = realloc(rq->body, rq->body_len + 1);
                    if (next == NULL) return PARSER_BAD_REQUEST;
                    rq->body = next;
                    rq->chunk_capacity = rq->body_len + 1;
                }
                rq->body[rq->body_len] = '\0';
                rq->complete = 1;
                return PARSER_SUCCESS;
            }
            if (memchr(rq->raw + rq->chunk_pos, ':', p - rq->chunk_pos) == NULL) return PARSER_BAD_REQUEST;
            rq->chunk_pos = p + 2;
        }
    }
}

int http_parser(struct rq_http* rq, char* buff, size_t buffs) {
    if (rq == NULL || (buff == NULL && buffs)) return PARSER_BAD_REQUEST;
    if (rq->complete) return PARSER_SUCCESS;
    if (buffs == 0) return HTTP_PARSER_BUFF_PENDING;
    if (rq->max_body_size == 0) rq->max_body_size = 16 * 1024 * 1024;
    if (rq->max_header_size == 0) rq->max_header_size = 16384;
    if (rq->max_headers == 0) rq->max_headers = MAX_HEADERS;
    if (rq->max_chunk_line_size == 0) rq->max_chunk_line_size = 128;
    if (rq->max_trailer_size == 0) rq->max_trailer_size = 65536;
    size_t raw_limit = rq->max_request_size ? rq->max_request_size :
            rq->max_body_size + rq->max_header_size + rq->max_trailer_size + rq->max_chunk_line_size;
    if (rq->raw_len > raw_limit || buffs > raw_limit - rq->raw_len) return PARSER_TOO_LARGE;
    size_t needed = rq->raw_len + buffs + 1;
    if (needed > rq->raw_capacity) {
        size_t capacity = rq->raw_capacity ? rq->raw_capacity : 16384;
        while (capacity < needed) {
            size_t grown = capacity > (raw_limit + 1) / 2 ? raw_limit + 1 : capacity * 2;
            if (grown <= capacity) return PARSER_TOO_LARGE;
            capacity = grown;
        }
        char* next = realloc(rq->raw, capacity);
        if (next == NULL) return PARSER_BAD_REQUEST;
        rq->raw = next;
        rq->raw_capacity = capacity;
    }
    memcpy(rq->raw + rq->raw_len, buff, buffs);
    rq->raw_len += buffs;
    rq->raw[rq->raw_len] = '\0';

    if (!rq->headers_done) {
        size_t end = 0;
        if (!find_header_end(rq->raw, rq->raw_len, &end)) {
            if (rq->raw_len > rq->max_header_size) return PARSER_TOO_LARGE;
            return HTTP_PARSER_BUFF_PENDING;
        }
        if (end > rq->max_header_size) return PARSER_TOO_LARGE;
        rq->header_len = end;
        int r = parse_headers(rq);
        if (r != PARSER_SUCCESS) return r;
    }
    if (rq->body_type == 2) return decode_chunked(rq);
    if (rq->body_type == 1) {
        size_t need = (size_t)rq->cont_len;
        if (rq->raw_len - rq->header_len < need) return HTTP_PARSER_BUFF_PENDING;
        rq->body = malloc(need + 1);
        if (rq->body == NULL) return PARSER_BAD_REQUEST;
        memcpy(rq->body, rq->raw + rq->header_len, need);
        rq->body[need] = '\0';
        rq->body_len = need;
    }
    rq->complete = 1;
    return PARSER_SUCCESS;
}

void http_parser_free(struct rq_http* rq) {
    if (!rq) return;
    free(rq->method);
    free(rq->target);
    free(rq->version);
    for (int i = 0; i < rq->nheader; i++) {
        free(rq->headers[i].name);
        free(rq->headers[i].value);
    }
    free(rq->body);
    free(rq->raw);
    memset(rq, 0, sizeof(*rq));
}
