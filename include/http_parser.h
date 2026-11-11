#ifndef http_parser_h
#define http_parser_h

#define MAX_HEADERS 128
#define HTTP_1_1 "HTTP/1.1"
#define HTTP_CONTENT_LENGTH_CONSTANT "Content-Length"
#define HTTP_TRANSFER_ENCODING_CONSTANT "Transfer-Encoding"

#define PARSER_SUCCESS 0
#define PARSER_UNSUPPORTED_PROTOCOL -1
#define HTTP_PARSER_BUFF_PENDING 1
#define PARSER_PROTOCOL_SUPPORT_UNDEFINED 2
#define PARSER_EMPTY_BUFFER 3
#define PARSER_EXCEEDING_HEADER_COUNT_MAX 4
#define PARSER_NO_BODY 5
#define PARSER_HAS_BODY 6
#define PARSER_TRANSFER_ENCODING_BODY_SIZE 7
#define PARSER_BAD_REQUEST 8
#define PARSER_TOO_LARGE 9

#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>

struct http_header {
    char* name;
    char* value;
};

struct rq_http {
    char *method;
    char *target;
    char *version;
    int is_sup_http;
    struct http_header headers[MAX_HEADERS];
    int nheader;
    ssize_t cont_len;
    char *body;
    size_t body_len;
    int body_type;
    char *raw;
    size_t raw_len, raw_capacity, header_len;
    size_t max_body_size, max_header_size, max_trailer_size, max_request_size;
    unsigned int max_headers, max_chunk_line_size;
    int headers_done, complete;
    size_t chunk_pos, chunk_remaining, chunk_capacity, trailer_size;
    int chunk_state;
};

int http_parser(struct rq_http *rq, char* buff, size_t buffs);
void http_parser_free(struct rq_http *rq);

#endif
