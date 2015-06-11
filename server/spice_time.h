#ifndef H_SPICE_TIME
#define H_SPICE_TIME

#include <time.h>

#define MILLI_SECOND 1000LL
#define NANO_SECOND 1000000000LL
#define NANO_MS (NANO_SECOND / MILLI_SECOND)

static inline uint64_t nano_now(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return NANO_SECOND * time.tv_sec + time.tv_nsec;
}


static inline uint64_t milli_now(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return MILLI_SECOND * time.tv_sec + time.tv_nsec / NANO_MS;
}

#endif
