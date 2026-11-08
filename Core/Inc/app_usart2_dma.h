#ifndef __APP_USART2_DMA_H
#define __APP_USART2_DMA_H
#include "FreeRTOS.h"
#include "task.h"
#include "memory_monitor.h"
#include <stdarg.h>

/**
 * @brief 初始化 USART2 的 DMA 发送框架（动态内存 + 零拷贝）
 * @note  必须在 FreeRTOS 调度器启动前（vTaskStartScheduler 之前）调用
 */
void App_USART2_Init(void);

/**
 * @brief 格式化打印（线程安全，非阻塞业务任务）
 * @param fmt  格式化字符串（支持 %d, %s, %.2f 等标准格式）
 * @param ...  可变参数列表
 * @note  内部使用 vsnprintf 双重调用精确计算长度，只申请刚好够用的内存
 *        队列满时最多阻塞 100ms，超时直接丢弃（保护喂狗任务不被卡死）
 */
void App_USART2_Printf(const char *fmt, ...);

/**
 * @brief 原样发送一个字符串（不做格式化，栈开销很小）
 * @param s 以 '\0' 结尾的字符串
 * @note  给 App_RC_Proc 这类"栈空间紧张"的任务用：内部只做 strlen + memcpy，
 *        不调用 vsnprintf；队列满时最多等 50ms 就丢弃，不会卡死调用任务。
 */
void App_USART2_SendRaw(const char *s);
extern TaskHandle_t usart2; 
#endif