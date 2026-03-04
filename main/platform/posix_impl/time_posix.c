#include "platform/time.h"

#include <time.h>
#include <unistd.h>

uint64_t mimi_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

void mimi_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000U);
}

