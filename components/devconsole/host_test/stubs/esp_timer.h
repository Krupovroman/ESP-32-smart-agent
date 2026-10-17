#pragma once
#include <stdint.h>
// Simulated clock: reads that find no input advance it by their timeout.
int64_t esp_timer_get_time(void);
