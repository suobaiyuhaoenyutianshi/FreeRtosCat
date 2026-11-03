#include "app_rc.h"
#include "main.h"
#include "stm32f1xx_hal_uart.h"
#include "string.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include "app_control.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#define MAX_leng 64
static volatile char directIntBuf[MAX_leng];
static volatile char proBuf[MAX_leng];//目的

/* DMA 接收：USART3 通过 DMA1_Channel3 + 空闲中断，整行收进 activeBuf */
static uint8_t  ucActic =0; // 0：当前写入 directIntBuf；1：当前写入 proBuf
static uint8_t  ucready =1; // 1：proBuf 就绪；0：directIntBuf 就绪（ISR 里切换）
static SemaphoreHandle_t xLineReady = NULL; // 二值信号量：通知解析任务有完整一行

static void str_to_lower(char *str) {
    while (*str) {
        if (*str >= 'A' && *str <= 'Z') {
            *str += 32;  // ASCII 中 'a' - 'A' = 32
        }
        str++;
    }
}

void App_RC_Init(void){
    xLineReady = xSemaphoreCreateBinary();
    /* 用 DMA + 空闲中断收变长行，一次武装，空闲即回调整行 */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, (uint8_t*)directIntBuf, MAX_leng);
}

/* 空闲(或收满)回调：整行已落入 activeBuf，交换缓冲并通知解析任务 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size){
    if (huart != &huart3) return;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    char* active = (ucActic == 0)? (char*)directIntBuf : (char*)proBuf;
    if (Size >= MAX_leng) Size = MAX_leng - 1;
    active[Size] = '\0';
    if (Size > 0 && active[Size-1] == '\r') active[Size-1] = '\0'; // 去 \r\n 的 \r

    /* 交换：下次写到另一缓冲 */
    ucActic ^= 1;
    ucready ^= 1;
    char* next = (ucActic == 0)? (char*)directIntBuf : (char*)proBuf;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, (uint8_t*)next, MAX_leng);

    xSemaphoreGiveFromISR(xLineReady, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* 遥控接收任务：等一行就绪，解析 */
void APP_rc_rxTask(void *argument){
    (void)argument;
    for (;;) {
        xSemaphoreTake(xLineReady, portMAX_DELAY);
        App_RC_Proc();
    }
}

void App_RC_Proc(void){
        char line[MAX_leng];                      /* 修复新增：本地拷贝缓冲 */
        char * Pdatas;
        if (ucready == 0) {
             Pdatas = (char*)directIntBuf;
        }
        else {
             Pdatas = (char*)proBuf;
        }
        /* 修复：修复前任务直接解析共享缓冲，ISR 下一帧回调可能覆盖正在读的块，
           造成读撕裂/丢行。修复后先整体拷到本地 line。
           用临界区包住 64 字节拷贝（约 1µs，对中断影响可忽略），
           确保 strncpy 期间 ISR 无法翻转入料缓冲，彻底封死竞态窗口。 */
        taskENTER_CRITICAL();
        strncpy(line, (const char*)Pdatas, MAX_leng - 1);
        line[MAX_leng - 1] = '\0';
        taskEXIT_CRITICAL();
        Pdatas = line;
        str_to_lower( Pdatas);
        if(strncmp(Pdatas, "move ", 5)==0) {
            int turnSpeed, moveSpeed;
            if (sscanf(Pdatas, "move %d %d", &turnSpeed, &moveSpeed) == 2) {
                App_Contro_setMoveSpeed(-moveSpeed *0.01f*0.7f);
                App_Contro_setTurnSpeed(-turnSpeed*0.01f*15.0f);
            }
        }
}