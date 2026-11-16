#include "app_rc.h"
#include "main.h"
#include "stm32f1xx_hal_uart.h"
#include "string.h"
#include <stdint.h>
#include "app_control.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "app_usart2_dma.h"   /* 调试时把收到的蓝牙原始帧打印到 USART2 */
#define RC_DEBUG_PRINT 1      /* 1=打印收到的每一帧；确认蓝牙通了以后改成 0 */

#define MAX_leng 64

/* DMA + 空闲中断 + 双缓冲：
 *   s_dma_buf[0]/[1] 由 DMA1_Channel3 交替写入；
 *   s_ready_buf 只由 CPU 读写——ISR 把刚收完的帧拷进来，任务从这里取。
 * 这样即使任务拷贝期间 ISR 又完成一帧并重装 DMA，DMA 也只会写到 s_dma_buf，
 * 不会踩到任务正在读的 s_ready_buf（临界区只能挡 CPU 中断，挡不住 DMA 写内存）。
 * 出错（FE/NE/ORE）由 HAL_UART_ErrorCallback 丢弃当前帧并重新武装。 */
static uint8_t  s_dma_buf[2][MAX_leng];
static volatile char s_ready_buf[MAX_leng];
static uint8_t  s_dma_idx = 0;
static SemaphoreHandle_t xLineReady = NULL;    /* 二值信号量：通知解析任务有完整一帧 */

volatile uint16_t testrc = 0;        /* 诊断：收到的字节数（按帧累加） */
volatile uint16_t testRcErr = 0;     /* 诊断：FE/NE/ORE 触发重新武装的次数 */
volatile uint16_t testRcrEecip = 0;  /* 诊断：交给任务的帧数 */

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
    s_dma_idx = 0;
    /* 启动 DMA + 空闲中断接收。返回值必须检查：启动瞬间若有挂起错误，
       HAL 会返回 HAL_ERROR 并把 ReceptionType 复位成 STANDARD，
       之后永远不会进 HAL_UARTEx_RxEventCallback。 */
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng) != HAL_OK)
    {
        s_dma_idx ^= 1U;
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng);
    }
}

/* 空闲(或收满)回调：整帧已落入 s_dma_buf[s_dma_idx]，拷到成品缓冲后换缓冲重新武装。 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart != &huart3) return;

    /* DMA_NORMAL 下“半传输”事件不是帧结束（只收到 32 字节），直接忽略 */
    if (HAL_UARTEx_GetRxEventType(huart) == HAL_UART_RXEVENT_HT) return;
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (Size > (MAX_leng - 1U)) Size = (MAX_leng - 1U);   /* 收满 64 时截到 63 */

    /* 拷到成品缓冲（DMA 不会写这块内存） */
    for (uint16_t i = 0U; i < Size; i++)
    {
        s_ready_buf[i] = (char)s_dma_buf[s_dma_idx][i];
    }
    /* 去掉行尾的 \r / \n */
    while ((Size > 0U) &&
           ((s_ready_buf[Size - 1U] == '\r') || (s_ready_buf[Size - 1U] == '\n')))
    {
        Size--;
    }
    s_ready_buf[Size] = '\0';

    /* 换另一块 DMA 缓冲重新武装；这一步失败就丢弃本帧，不在这里重试 */
    s_dma_idx ^= 1U;
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng);

    testrc += Size;

    if (Size > 0U)
    {
        xSemaphoreGiveFromISR(xLineReady, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* DMA 模式下 HAL 把 FE/NE/ORE 任何一个都当致命错误：UART_EndRxTransfer 会关掉
   IDLEIE/EIE 并中止 DMA，然后调用这里。当前帧直接丢弃、重新武装即可，
   丢一帧对遥控没有影响（下一次空闲又会有新帧）。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart3) return;
     // 1. 必须清标志，防止中断风暴
    __HAL_UART_CLEAR_PEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);

    // 2. 检查状态，确保底层已经释放了DMA和接收锁
    if (huart->RxState == HAL_UART_STATE_READY) 
    {
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng);
    }
    testRcErr++;
    
}

/* 遥控接收任务：等一帧就绪，解析 */
void APP_rc_rxTask(void *argument){
    (void)argument;
    for (;;) {
        xSemaphoreTake(xLineReady, portMAX_DELAY);
        testRcrEecip++;
        App_RC_Proc();
    }
}

/* 不用 sscanf：newlib 的 scanf 一族在 -O0 下要几百字节栈，叠加 vsnprintf
   会把 rc 任务栈撑爆（爆栈钩子关中断死循环 + IWDG 复位）。
   这里手写"跳过空格 + 可选符号 + 十进制整数"的解析，栈开销几十字节。 */
static int rc_parse_int(const char **pp, int *out)
{
    const char *p = *pp;
    while (*p == ' ') p++;

    int sign = 1;
    if (*p == '-' || *p == '+') {
        if (*p == '-') sign = -1;
        p++;
    }
    if (*p < '0' || *p > '9') return 0;

    int val = 0;
    while (*p >= '0' && *p <= '9') {
        if (val < 100000) val = val * 10 + (int)(*p - '0');  /* 防 int 溢出 */
        p++;
    }

    *out = sign * val;
    *pp = p;
    return 1;
}

#if RC_DEBUG_PRINT
/* 同理不用 vsnprintf（%s 在 -O0 下同样吃栈），手动拼 "RC<- [<frame>]\n" */
static void rc_debug_print(const char *line)
{
    char tmp[MAX_leng + 10];
    uint16_t n = (uint16_t)strlen(line);
    if (n > (MAX_leng - 1U)) n = (MAX_leng - 1U);

    memcpy(tmp, "RC<- [", 6);
    memcpy(&tmp[6], line, n);
    tmp[6 + n] = ']';
    tmp[7 + n] = '\n';
    tmp[8 + n] = '\0';

    App_USART2_SendRaw(tmp);
}
#endif

void App_RC_Proc(void){
        char line[MAX_leng];
        /* 临界区拷贝：ISR 下一帧到达时会覆盖 s_ready_buf，先整体搬走再解析，
           避免读到"半新半旧"的数据（s_ready_buf 只有 CPU 读写，临界区足够） */
        taskENTER_CRITICAL();
        strncpy(line, (const char*)s_ready_buf, MAX_leng - 1);
        line[MAX_leng - 1] = '\0';
        taskEXIT_CRITICAL();

        str_to_lower(line);
#if RC_DEBUG_PRINT
        /* 收到任何一帧都打印出来：如果这里一直没输出，说明蓝牙/接线/波特率有问题，
           而不是解析问题 */
        rc_debug_print(line);
#endif
        if(strncmp(line, "move ", 5)==0) {
            int turnSpeed, moveSpeed;
            const char *p = line + 5;
            if (rc_parse_int(&p, &turnSpeed) && rc_parse_int(&p, &moveSpeed)) {
                
                if (turnSpeed >  100) turnSpeed =  100;
                if (turnSpeed < -100) turnSpeed = -100;
                if (moveSpeed >  100) moveSpeed =  100;
                if (moveSpeed < -100) moveSpeed = -100;

                App_Contro_setMoveSpeed(-moveSpeed *0.01f*0.7f);
                App_Contro_setTurnSpeed(-turnSpeed*0.01f*15.0f);
            }
        }
}
