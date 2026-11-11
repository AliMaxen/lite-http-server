#include "../include/http_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int feed_request_len(const char* request, size_t len, size_t split, const char* expected, size_t expected_len) {
    struct rq_http rq = {0};
    rq.max_body_size = 1024;
    rq.max_header_size = 4096;
    rq.max_headers = 64;
    size_t pos = 0;
    int result = HTTP_PARSER_BUFF_PENDING;
    while (pos < len && result == HTTP_PARSER_BUFF_PENDING) {
        size_t take = split < len - pos ? split : len - pos;
        result = http_parser(&rq, (char*)request + pos, take);
        pos += take;
    }
    int ok = result == PARSER_SUCCESS && rq.body_len == expected_len &&
            (expected_len == 0 || memcmp(rq.body, expected, expected_len) == 0);
    if (!ok) fprintf(stderr, "Parser test failed: result=%d body_len=%zu expected=%zu\n", result, rq.body_len, expected_len);
    http_parser_free(&rq);
    return ok;
}

static int reject_request(const char* request, int expected) {
    struct rq_http rq = {0};
    rq.max_body_size = 1024;
    rq.max_header_size = 4096;
    rq.max_headers = 64;
    int result = http_parser(&rq, (char*)request, strlen(request));
    http_parser_free(&rq);
    if (result != expected) fprintf(stderr, "Expected parser error %d, got %d\n", expected, result);
    return result == expected;
}

static int reject_limited_request(const char* request, size_t max_body, size_t max_request, int expected) {
    struct rq_http rq = {0};
    rq.max_body_size = max_body;
    rq.max_request_size = max_request;
    rq.max_header_size = 4096;
    rq.max_headers = 64;
    int result = http_parser(&rq, (char*)request, strlen(request));
    http_parser_free(&rq);
    if (result != expected) fprintf(stderr, "Expected limited parser result %d, got %d\n", expected, result);
    return result == expected;
}

int main(void) {
    const char* cl = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello world";
    const char* te = "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=x\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: yes\r\n\r\n";
    const char* crlf_data = "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n\r\nAB\r\n0\r\n\r\n";
    const char* nul = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\na\0b";
    int ok = 1;
    ok &= feed_request_len(cl, strlen(cl), 1, "hello world", 11);
    ok &= feed_request_len(cl, strlen(cl), 7, "hello world", 11);
    ok &= feed_request_len(te, strlen(te), 1, "Wikipedia", 9);
    ok &= feed_request_len(te, strlen(te), 5, "Wikipedia", 9);
    ok &= feed_request_len(crlf_data, strlen(crlf_data), 1, "\r\nAB", 4);
    ok &= feed_request_len(nul, sizeof("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\n") - 1 + 3, 2, "a\0b", 3);
    ok &= reject_request("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx", PARSER_BAD_REQUEST);
    ok &= reject_request("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nx\r\n0\r\n\r\n", PARSER_BAD_REQUEST);
    ok &= reject_request("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", PARSER_BAD_REQUEST);
    ok &= reject_limited_request("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2048\r\n\r\n", 1024, 0, PARSER_TOO_LARGE);
    ok &= reject_limited_request("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 1024, 16, PARSER_TOO_LARGE);
    return ok ? 0 : 1;
}
