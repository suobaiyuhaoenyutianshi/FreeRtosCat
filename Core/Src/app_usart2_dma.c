// ======================== 头文件包含 ========================
#include "app_usart2_dma.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "stm32f103xb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
TaskHandle_t usart2;
// ======================== 引用外部硬件句柄 ========================
extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef hdma_usart2_tx;
// ======================== 可调配置宏 ========================
#define TX_QUEUE_LEN       5
#define MAX_PRINTF_LEN     256

// ======================== 静态全局变量 ========================
static QueueHandle_t xTxQueue;
static SemaphoreHandle_t xTxDmaSem;

// ======================== DMA 发送完成中断回调 ========================
volatile uint32_t tx_cplt_count = 0;
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART2) {
        tx_cplt_count++;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (xTxDmaSem != NULL) {
         xSemaphoreGiveFromISR(xTxDmaSem, &xHigherPriorityTaskWoken);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
// ======================== 守门员任务 ========================
static void vUSART2_TxGateKeeperTask(void *pvParameters)
{
    (void)pvParameters;
    char *tx_buffer;
    for (;;)
    {
        // 1. 等待队列数据
        if (xQueueReceive(xTxQueue, &tx_buffer, portMAX_DELAY) != pdPASS)
            continue;

        // 2. 启动 DMA 发送
        if (HAL_UART_Transmit_DMA(&huart2, (uint8_t*)tx_buffer, strlen(tx_buffer)) != HAL_OK)
        {
            // 发送启动失败，释放内存并继续
            vPortFree(tx_buffer);
            continue;
        }

        // 3. 等待发送完成信号量（最多等待5秒）
        if (xSemaphoreTake(xTxDmaSem, pdMS_TO_TICKS(5000)) != pdPASS)
        {
            // 超时，可以打印错误（但不要用阻塞发送，以免影响）
            vPortFree(tx_buffer);
            continue;
        }

        // 4. 发送成功，释放内存
        vPortFree(tx_buffer);
    }
}

// ======================== 对外打印接口 ========================
void App_USART2_Printf(const char *fmt, ...)
{
    va_list args;
    char *buf = NULL;
    int need_len;

    // 1. 计算长度
    va_start(args, fmt);
    need_len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (need_len <= 0 || need_len > MAX_PRINTF_LEN)
        return;

    // 2. 分配内存
    buf = (char *)pvPortMalloc(need_len + 1);
    if (buf == NULL)
        return;

    // 3. 格式化写入
    va_start(args, fmt);
    vsnprintf(buf, need_len + 1, fmt, args);
    va_end(args);

    // 4. 入队
    if (xQueueSend(xTxQueue, &buf, portMAX_DELAY) != pdPASS)
    {
        vPortFree(buf);
    }
}

// ======================== 初始化函数 ========================
void App_USART2_Init(void)
{
    // 1. 创建发送队列
    xTxQueue = xQueueCreate(TX_QUEUE_LEN, sizeof(char *));
    configASSERT(xTxQueue != NULL);

    // 2. 创建二进制信号量（初始为空）
    xTxDmaSem = xSemaphoreCreateBinary();
    configASSERT(xTxDmaSem != NULL);

    // 3. 创建守门员任务
    configASSERT(xTaskCreate(vUSART2_TxGateKeeperTask, "UART2_TX", 256, NULL, 2, &usart2) == pdPASS);

    // 4. 使能 DMA 中断（此时信号量和队列已就绪）
    HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);
}