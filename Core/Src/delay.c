#include "delay.h"
#include "stm32f103xb.h"
#include "system_stm32f1xx.h"
#include <stdint.h>

// 初始化 DWT 计数器（必须在 main 里先调用一次）
void Delay_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
//us级延迟
void DelayUs(uint32_t us){
 uint32_t start = DWT->CYCCNT;
 uint32_t ticks = us * (SystemCoreClock / 1000000);
 // 减法比较：CYCCNT 回绕(约59.6s@72MHz)也正确，避免加法 end 溢出死循环
 while ((DWT->CYCCNT - start) < ticks);
}
//获取系统上电以来的微秒数
uint64_t GetUs(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000000);
}