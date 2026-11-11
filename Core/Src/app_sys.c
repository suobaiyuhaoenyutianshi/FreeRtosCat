
#include "main.h"

/*
 * 中断优先级统一编排
 * ---------------------------------------------------------------------------
 * 必须在所有 MX_*_Init() 之后调用（那些函数里会自己设一遍优先级）。
 * 放在独立文件里，CubeMX 重新生成 main.c 也不会影响这里。
 *
 * 规则（Cortex-M3 + FreeRTOS）：
 *   1. 数字越小优先级越高；
 *   2. ISR 里要调用 xxxFromISR() 的，优先级数字必须
 *      >= configMAX_SYSCALL_INTERRUPT_PRIORITY（本工程 = 5）。
 *      取 5 = "允许调 FreeRTOS API 的最高优先级"。
 *   3. HAL 的 1ms 时基（TIM2）不能被高频 ISR 饿死，所以它取 5；
 *      编码器 EXTI 是高频边沿中断，降到 6，排在 TIM2 之后。
 * ---------------------------------------------------------------------------
 */
void App_NvicPriority_Init(void)
{
    /* --- 会调用 FreeRTOS FromISR 接口的：取 5 --- */
    HAL_NVIC_SetPriority(TIM2_IRQn,   5U, 0U);   /* HAL 1ms timebase，回调里会 vTaskNotifyGiveFromISR */
    HAL_NVIC_SetPriority(USART3_IRQn, 5U, 0U);   /* 蓝牙逐字节接收，回调里会 xSemaphoreGiveFromISR */

    /* --- 不调用 RTOS 接口的高频中断：降到 6，避免抢占上面两个 --- */
    HAL_NVIC_SetPriority(EXTI3_IRQn,       6U, 0U);   /* 编码器 A 相（右） */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn,   6U, 0U);   /* 编码器 A 相（左） */
    HAL_NVIC_SetPriority(ADC1_2_IRQn,      6U, 0U);   /* 电池采样 */
    HAL_NVIC_SetPriority(TIM1_BRK_IRQn,    6U, 0U);
    HAL_NVIC_SetPriority(TIM1_UP_IRQn,     6U, 0U);
    HAL_NVIC_SetPriority(TIM1_TRG_COM_IRQn, 6U, 0U);
    HAL_NVIC_SetPriority(TIM1_CC_IRQn,     6U, 0U);
    HAL_NVIC_SetPriority(TIM3_IRQn,        6U, 0U);
    HAL_NVIC_SetPriority(TIM4_IRQn,        6U, 0U);
}
