#include "mimi_time.h"

#include <sys/time.h>
#include <unistd.h>

uint64_t mimi_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

void mimi_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000U);
}

