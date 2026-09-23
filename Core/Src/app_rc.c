#include "app_rc.h"
#include "main.h"
#include "stm32f1xx_hal_uart.h"
#include "string.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include "app_control.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#define MAX_leng 64
static volatile char directIntBuf[MAX_leng];
static volatile char proBuf[MAX_leng];//目的

/* 用于 HAL_UART_Receive_IT 的接收字节变量 */
static uint8_t rcRxByte;
static uint8_t  ucActic =0;//这是标志说明它可以活动接受,如0 说明是directIntBu可动
static uint8_t ucready =1;//说明接受完成在处理  不可用他接受如0 说明是directIntBu不可动，在处理
static uint16_t intBufCursor = 0;

static void str_to_lower(char *str) {
    while (*str) {
        if (*str >= 'A' && *str <= 'Z') {
            *str += 32;  // ASCII 中 'a' - 'A' = 32
        }
        str++;
    }
}
static QueueHandle_t xMessage =NULL;

void App_RC_Init(void){
    xMessage=xQueueCreate(20,sizeof(char));
    HAL_UART_Receive_IT(&huart3,&rcRxByte,1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart){
    if (huart == &huart3) {
        BaseType_t xhwoken = pdFALSE;
        xQueueSendFromISR(xMessage,&rcRxByte,&xhwoken);
        portYIELD_FROM_ISR(xhwoken);
        HAL_UART_Receive_IT(&huart3,&rcRxByte,1);
    }
}

/* 遥控接收任务：从队列逐字节取出，攒成一行后解析
   注意 curBuf 必须在每次循环里用 ucActic 重新选，
   不能用上一次循环残留的局部变量（auto 变量每次循环都是新的） */
void APP_rc_rxTask(void *argument){
    (void)argument;
    for (;;) {
        uint8_t ucdat ;
        xQueueReceive(xMessage,&ucdat,portMAX_DELAY);

        char* curBuf = (ucActic == 0) ? directIntBuf : proBuf;

        if (ucdat!='\n') {
            if (intBufCursor < (MAX_leng-1)) {
                curBuf[intBufCursor] = ucdat;
                intBufCursor++;
            }else {
                intBufCursor = 0;   // 一行超长，简单归零
            }
        }else {
            curBuf[intBufCursor] = '\0';   // 收完一行补结尾
            intBufCursor = 0;
            ucActic = ucActic ^1;   // 切换写入 buffer
            ucready = ucready^1;    // 标记刚写完的 buffer 就绪
            App_RC_Proc();          // 只在收完一行后解析一次
        }
    }
}

void App_RC_Proc(void){
        char * Pdatas;
        if (ucready == 0) {
             Pdatas = directIntBuf;
        }
        else {
            Pdatas = proBuf;
        }
        str_to_lower( Pdatas);
        if(strncmp(Pdatas, "move ", 5)==0) {
            int turnSpeed, moveSpeed;
            if (sscanf(Pdatas, "move %d %d", &turnSpeed, &moveSpeed) == 2) {
                App_Contro_setMoveSpeed(-moveSpeed *0.01f*0.7f);
                App_Contro_setTurnSpeed(-turnSpeed*0.01f*15.0f);
            }
        }
}