#ifndef H_SPICE_TIME
#define H_SPICE_TIME

#include <time.h>

#define NANO_SECOND 1000000000LL
#define NANO_MS 1000000LL

static inline uint64_t nano_now(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return NANO_SECOND * time.tv_sec + time.tv_nsec;
}

#endif
