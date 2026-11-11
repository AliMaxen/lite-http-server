#ifndef logger_h
#define logger_h

#define LOG_STAT_SUCCESS 0
#define LOG_STAT_INFO 1
#define LOG_STAT_WARNING 2
#define LOG_STAT_FAILURE 3
#define LOG_STAT_ERROR 4


struct conf_types;

struct log_data {
    int log_stat;
    char datetime[20]; // 2026-10-10_05:02:10
    char mod[8];
    char cont[300];
    char lp[256];
};


// Assigns values to a log_data struct, along with the appropriate format of datetime of current
// time.
int mklog(struct conf_types* conf, struct log_data *logd, int log_stat, char* mod, char* cont);


// The function that would log the log_data information in a pre-defined word order into the log
// file specified in conf_types
int flog(struct log_data* logd);

#endif

