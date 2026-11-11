#include "../include/logger.h"
#include "../include/config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int mklog(struct conf_types* conf, struct log_data* logd, int log_stat, char* mod, char* cont) {
    if (!conf->is_logging) return 1;
    memset(logd, 0, sizeof(*logd));
    snprintf(logd->mod, sizeof(logd->mod), "%s", mod);
    snprintf(logd->cont, sizeof(logd->cont), "%s", cont);
    snprintf(logd->lp, sizeof(logd->lp), "%s", conf->logs_path);
    logd->log_stat = log_stat;
    time_t cur = time(NULL);
    struct tm tinfo;
    if (localtime_r(&cur, &tinfo) == NULL) return -1;
    strftime(logd->datetime, sizeof(logd->datetime), "%Y-%m-%d_%H:%M:%S", &tinfo);
    return 0;
}

int flog(struct log_data* logd) {
    static const char* stats[] = {"SUCCESS", "INFO", "WARNING", "FAILURE", "ERROR"};
    const char* stat = logd->log_stat >= 0 && logd->log_stat <= 4 ? stats[logd->log_stat] : "NONE";
    pthread_mutex_lock(&log_lock);
    FILE* lf = fopen(logd->lp, "a");
    if (lf == NULL) {
        pthread_mutex_unlock(&log_lock);
        fprintf(stderr, "Failed to open log file at %s\n", logd->lp);
        return -1;
    }
    int r = fprintf(lf, "%s [%s] - %s: %s\n", logd->mod, stat, logd->datetime, logd->cont);
    fclose(lf);
    pthread_mutex_unlock(&log_lock);
    return r < 0 ? -1 : 0;
}
