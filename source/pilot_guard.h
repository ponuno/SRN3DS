#pragma once
#include <stdint.h>
void n3ds_pilot_tick(void);
void n3ds_pilot_tick_pc(uint32_t pc);
/* 0 disables the basic-block budget (unlimited); see the pilot harness. */
void n3ds_pilot_set_steps(uint64_t steps);
