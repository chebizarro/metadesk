#ifndef MD_HOST_LIFECYCLE_H
#define MD_HOST_LIFECYCLE_H
#include <stdbool.h>
/* Exit 78 requires operator intervention; 1 is restartable; signals/stop = 0. */
#define MD_HOST_EXIT_CONFIG 78
int md_host_lifecycle_init(bool managed);
bool md_host_lifecycle_stopping(void);
void md_host_lifecycle_event(const char *state, int status);
int md_host_lifecycle_finish(int status);
#ifdef _WIN32
int md_host_lifecycle_stop(void);
#endif
#endif
