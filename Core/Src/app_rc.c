#include "app_rc.h"
#include "main.h"
#include "stm32f1xx_hal_uart.h"
#include "string.h"
#include <stdint.h>
#include "app_control.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "app_usart2_dma.h"   /* 调试时把收到的蓝牙原始行打印到 USART2 */
#define RC_DEBUG_PRINT 1      /* 1=打印收到的每一行；确认蓝牙通了以后改成 0 */

#define MAX_leng 64
/* 与老工程 CARTEST 一致：逐字节中断接收，累计到 '\n' 视为一行结束。
 * 老工程在同一块板子上验证可用；DMA+空闲中断那套在当前工程没有收到数据。 */
static volatile char directIntBuf[MAX_leng];   /* 正在拼的行 */
static volatile char proBuf[MAX_leng];         /* 拼完的行，交给任务 */
static uint16_t intBufCursor = 0;
static SemaphoreHandle_t xLineReady = NULL;    /* 二值信号量：通知解析任务有完整一行 */

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
    /* 与老工程 CARTEST（标准库版）一致：只开 RXNE 中断，中断里直接读 DR。
     * 不用 HAL_UART_Receive_IT：HAL 带接收状态机，一旦发生 ORE 溢出，
     * HAL_UART_IRQHandler 会调用 UART_EndRxTransfer() 把 RXNE/PE/ERR 中断
     * 全部关掉，而且不会自动重新武装（我们也没实现 HAL_UART_ErrorCallback），
     * 接收就此永久停摆。直接读 DR 没有状态机，ORE 只丢一个字节，接收继续。 */
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
}

/* 只在 USART3_IRQHandler 里调用（中断上下文），对应老工程的 USART3_IRQHandler。
   USART3 中断优先级在 main.c 里统一设为 5，
   满足 >= configMAX_SYSCALL_INTERRUPT_PRIORITY(5) 才能调 FromISR 接口。 */
volatile uint16_t testrc =0;
void App_RC_ISR(void)
{
    /* 先读 SR 再读 DR：这一对读操作同时清掉 RXNE 和 ORE（溢出）标志 */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE) == RESET) return;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t DATA = (uint8_t)(huart3.Instance->DR & 0xFFU);

    if ((DATA != '\n') && (DATA != '\r'))
    {
        if (intBufCursor < (MAX_leng - 1U))
        {
            directIntBuf[intBufCursor] = (char)DATA;
            intBufCursor++;
        }
        else
        {
            intBufCursor = 0;   /* 超长丢弃，重新开始拼 */
        }
    }
    else if (intBufCursor > 0U)
    {
        directIntBuf[intBufCursor] = '\0';
        /* 整行拷到 proBuf（覆盖上一行），通知任务 */
        for (uint16_t i = 0U; i <= intBufCursor; i++)
        {
            proBuf[i] = directIntBuf[i];
        }
        intBufCursor = 0;

        xSemaphoreGiveFromISR(xLineReady, &xHigherPriorityTaskWoken);
    }

    /* 不需要重新武装：RXNEIE 一直保持使能，下一个字节到了会再次进中断 */
    testrc++;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

volatile uint16_t  testRcrEecip =0;
/* 遥控接收任务：等一行就绪，解析 */
void APP_rc_rxTask(void *argument){
    (void)argument;
    for (;;) {
        xSemaphoreTake(xLineReady, portMAX_DELAY);
        testRcrEecip++;
        App_RC_Proc();
    }
}

/* 不用 sscanf：newlib 的 scanf 一族在 -O0 下要几百字节栈，叠加 vsnprintf
   会把 1KB 的 rc 任务栈撑爆（爆栈钩子关中断死循环 + IWDG 复位）。
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
/* 同理不用 vsnprintf（%s 在 -O0 下同样吃栈），手动拼 "RC<- [<line>]\n" */
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
        /* 临界区拷贝：ISR 下一行到达时会覆盖 proBuf，先整体搬走再解析，
           避免读到"半新半旧"的数据 */
        taskENTER_CRITICAL();
        strncpy(line, (const char*)proBuf, MAX_leng - 1);
        line[MAX_leng - 1] = '\0';
        taskEXIT_CRITICAL();

        str_to_lower(line);
#if RC_DEBUG_PRINT
        /* 收到任何一行都打印出来：如果这里一直没输出，说明蓝牙/接线/波特率有问题，
           而不是解析问题 */
        rc_debug_print(line);
#endif
        if(strncmp(line, "move ", 5)==0) {
            int turnSpeed, moveSpeed;
            const char *p = line + 5;
            if (rc_parse_int(&p, &turnSpeed) && rc_parse_int(&p, &moveSpeed)) {
                /* 和参考工程一致：满量程 ±100 对应 0.7 m/s / 15 rad/s。
                   先限幅，避免一条异常报文把目标值顶到天上 */
                if (turnSpeed >  100) turnSpeed =  100;
                if (turnSpeed < -100) turnSpeed = -100;
                if (moveSpeed >  100) moveSpeed =  100;
                if (moveSpeed < -100) moveSpeed = -100;

                App_Contro_setMoveSpeed(-moveSpeed *0.01f*0.7f);
                App_Contro_setTurnSpeed(-turnSpeed*0.01f*15.0f);
            }
        }
}
