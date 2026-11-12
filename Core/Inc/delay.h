#ifndef DELAY_H
#define DELAY_H
#include"main.h"
#include <stdint.h>
void Delay_Init(void);
void DelayUs(uint32_t us);

/* ---- 时间戳统一用 DWT tick 域 ----
 * GetTick() 返回原始 CYCCNT：1 tick = 1/SystemCoreClock 秒，32 位模计数器，
 * 72MHz 下约 59.65s 回绕一次。时间差必须先在 uint32 里相减、再换算:
 *     uint32_t d    = GetTick() - t0;   // mod 2^32，回绕自动正确
 *     float    dt_s = TICKS_TO_SEC(d);
 * 不要把时间戳先转成 uint64/浮点再相减——模会变成 2^64，回绕处得到巨量值。 */
uint32_t GetTick(void);
#define TICKS_TO_SEC(ticks) ((float)(ticks) * (1.0f / (float)SystemCoreClock))
#endif // DELAY_H