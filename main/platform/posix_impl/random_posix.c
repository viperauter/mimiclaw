#include "platform/random.h"
#include "platform/mimi_err.h"

#include <fcntl.h>
#include <unistd.h>

uint32_t mimi_random_u32(void)
{
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v)) {
            return v;
        }
    }
    /* Fallback: xorshift-ish */
    static uint32_t s = 0x12345678U;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

