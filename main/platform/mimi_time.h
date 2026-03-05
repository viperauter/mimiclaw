#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t mimi_time_ms(void);
void mimi_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

