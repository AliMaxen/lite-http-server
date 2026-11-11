#ifndef config_h
#define config_h

#define TCPLS_PORT_DEFAULT 8080
#define TCPLS_HOST_DEFAULT "127.0.0.1"
#define LOGS_PATH_DEFAULT "liteserver.log"
#define CONFIG_FILE_PATH_DEFUALT "tests/liteserver.conf"
#define PARSE_ERROR_FILE_NOT_EXIST -1

#define VAR_COUNT 20
#define VAR_TCPLS_PORT "lite-server-port"
#define VAR_TCPLS_HOST "lite-server-host"
#define VAR_LOGS_PATH "logging-path"
#define VAR_ALLOW_LOG "allow-logging"

#define IS_NUMBER(ch) (ch >= '0' && ch <= '9')

struct conf_types {
    unsigned short tcpls_port, is_logging;
    char tcpls_host[64], logs_path[256];
    unsigned int max_workers, max_queue, max_headers, max_header_size;
    unsigned long long max_body_size, max_request_size;
    unsigned int request_timeout, listen_backlog, max_chunk_line_size, max_trailer_size;
    unsigned int reverse_proxy, directory_serve, directory_listing, send_buffer_size, recv_buffer_size;
    char document_root[512], proxy_host[128];
    unsigned short proxy_port;
};

int parse_conf(char* config_file_path, struct conf_types* server_conf);

#endif
