#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/config.h"
#include "../include/sock.h"
#include "../include/logger.h"

int main(int args, char** argv) {
    struct conf_types* conf = calloc(1, sizeof(struct conf_types));
    if (conf == NULL) return 1;
    char* conf_path = args > 1 ? argv[1] : CONFIG_FILE_PATH_DEFUALT;

    int parse_res = parse_conf(conf_path, conf);
    if (parse_res == PARSE_ERROR_FILE_NOT_EXIST) {
        fprintf(stderr, "Configuration file at %s does not exist. Using default configurations.\n", conf_path);
    } else if (parse_res > 0) {
        fprintf(stderr, "Configuration file %s has a parse error at line %d. Using defaults for invalid values.\n", conf_path, parse_res);
        free(conf);
        conf = calloc(1, sizeof(struct conf_types));
        if (conf == NULL) return 1;
        parse_conf("", conf);
    }

    printf("Server hostname: %s, Server port: %u, Workers: %u, Reverse Proxy: %u, Logging: %u\n",
            conf->tcpls_host, conf->tcpls_port, conf->max_workers, conf->reverse_proxy, conf->is_logging);
    int result = sock_up(conf);
    free(conf);
    return result == 0 ? 0 : 1;
}
