#ifndef sock_h
#define sock_h

struct conf_types;

// This function brings up the TCP socket on the specified configurations
int sock_up(struct conf_types* cf);

#endif
