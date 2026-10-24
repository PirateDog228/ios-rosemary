#pragma once
#include <stdint.h>
void m2_run(uint64_t fb, uint64_t fb_size, const uint64_t *cpu_mpidrs, int ncpu);
void m2_irq(void);
void secondary_main(uint64_t idx);
