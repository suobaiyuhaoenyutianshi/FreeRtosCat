/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* HardFault 取证要用到 SCB 寄存器和 FreeRTOS 的"当前任务"接口。
 * 放在 USER CODE 里，CubeMX 重新生成时不会被删。 */
#include "core_cm3.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_rc.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 取异常现场：测 EXC_RETURN 的 bit2 选 MSP/PSP，把帧指针(r0)和 EXC_RETURN(r1)
 * 作为参数尾调用 hardfault_dump。硬故障/总线/用法/内存管理 四个处理器共用。
 * hardfault_dump 里 while(1) 永不返回，所以用 b(尾调用)而不是 bl。 */
#define HF_DUMP_FRAME()                       \
    __asm volatile (                          \
        "tst lr, #4       \n"                 \
        "ite eq           \n"                 \
        "mrseq r0, msp    \n"                 \
        "mrsne r0, psp    \n"                 \
        "mov r1, lr       \n"                 \
        "b hardfault_dump \n"                 \
    )
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ============================ HardFault 现场抓取 ============================
 * Cortex-M3 进异常时硬件已把 8 个寄存器压进"当时正在用的栈"，顺序固定：
 *   stack[0]=R0 [1]=R1 [2]=R2 [3]=R3 [4]=R12 [5]=LR [6]=PC [7]=xPSR
 *
 * 调试时按这个顺序看 hf_ctx（Watch 里展开一个结构体即可）：
 *   pc         出错的那条指令 → addr2line / 反汇编窗口换成"文件:行号"
 *   lr         出错时的返回地址 → 从哪个函数调进来的
 *   r0~r3      函数前 4 个参数 → 非法读写的坏地址常躺在里面
 *   psr        低 9 位是异常号(IPSR)：0=线程 3=HardFault 16+n=第 n 个 IRQ
 *   exc_return 0xFFFFFFFD=崩在任务里(PSP)；0xFFFFFFF9=崩在中断里(MSP)
 *   hfsr       FORCED=1 说明是别的 fault 升级上来的
 *   cfsr       细分原因：PRECISERR / IMPRECISERR / UNALIGNED / DIVBYZERO / IBUSERR
 *   bfar       出错访问的数据地址（BFARVALID=1 时有效，0xE000ED38）
 *   mmfar      MemManage 坏地址（0xE000ED34，CFSR 低 8 位非 0 时看）
 *   sp         出事时用的栈指针，事后走栈/bt 要用
 *   cur_task   当前任务句柄；task_name 是任务名（空=调度器还没起来）
 * 注意：这些值在 RAM 里，暂停后用 Watch 看即可，不要在 printf 里读（可能再次崩）。
 * ========================================================================== */
typedef struct {
    uint32_t r0, r1, r2, r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
    uint32_t exc_return;
    uint32_t hfsr;
    uint32_t cfsr;
    uint32_t bfar;
    uint32_t mmfar;
    uint32_t sp;
    uint32_t cur_task;
    const char *task_name;
} hf_ctx_t;

volatile hf_ctx_t hf_ctx;      /* 现场唯一容器，Watch 这一个就够了 */

void hardfault_dump(uint32_t *stack, uint32_t exc_return)
{
    hf_ctx.r0     = stack[0];
    hf_ctx.r1     = stack[1];
    hf_ctx.r2     = stack[2];
    hf_ctx.r3     = stack[3];
    hf_ctx.r12    = stack[4];
    hf_ctx.lr     = stack[5];
    hf_ctx.pc     = stack[6];
    hf_ctx.psr    = stack[7];
    hf_ctx.sp     = (uint32_t)stack;

    hf_ctx.exc_return = exc_return;
    hf_ctx.hfsr   = SCB->HFSR;
    hf_ctx.cfsr   = SCB->CFSR;
    hf_ctx.bfar   = SCB->BFAR;
    hf_ctx.mmfar  = SCB->MMFAR;

    /* 记下崩在哪个任务：调度器没起来时句柄为 NULL，跳过，避免二次崩溃 */
    hf_ctx.cur_task = (uint32_t)xTaskGetCurrentTaskHandle();
    if (hf_ctx.cur_task != 0U)
    {
        hf_ctx.task_name = pcTaskGetName((TaskHandle_t)hf_ctx.cur_task);
    }
    else
    {
        hf_ctx.task_name = NULL;
    }

    while (1);   /* 停在这里，Watch hf_ctx 即可一次看完全部现场 */
}
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4;
extern DMA_HandleTypeDef hdma_usart2_tx;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern TIM_HandleTypeDef htim2;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M3 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  HF_DUMP_FRAME();   /* 抓现场：按 EXC_RETURN 选 MSP/PSP，尾调用 hardfault_dump */
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  HF_DUMP_FRAME();   /* 默认未使能时会升级成 HardFault；一旦使能，这里也抓现场 */
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  HF_DUMP_FRAME();   /* 默认未使能时会升级成 HardFault；一旦使能，这里也抓现场 */
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  HF_DUMP_FRAME();   /* 默认未使能时会升级成 HardFault；一旦使能，这里也抓现场 */
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */


/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */


/******************************************************************************/
/* STM32F1xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f1xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles EXTI line3 interrupt.
  */
void EXTI3_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI3_IRQn 0 */

  /* USER CODE END EXTI3_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3);
  /* USER CODE BEGIN EXTI3_IRQn 1 */

  /* USER CODE END EXTI3_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel3 global interrupt.
  */
void DMA1_Channel3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel3_IRQn 0 */

  /* USER CODE END DMA1_Channel3_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
  /* USER CODE BEGIN DMA1_Channel3_IRQn 1 */

  /* USER CODE END DMA1_Channel3_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel7 global interrupt.
  */
void DMA1_Channel7_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel7_IRQn 0 */

  /* USER CODE END DMA1_Channel7_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart2_tx);
  /* USER CODE BEGIN DMA1_Channel7_IRQn 1 */

  /* USER CODE END DMA1_Channel7_IRQn 1 */
}

/**
  * @brief This function handles ADC1 and ADC2 global interrupts.
  */
void ADC1_2_IRQHandler(void)
{
  /* USER CODE BEGIN ADC1_2_IRQn 0 */

  /* USER CODE END ADC1_2_IRQn 0 */
  HAL_ADC_IRQHandler(&hadc1);
  /* USER CODE BEGIN ADC1_2_IRQn 1 */

  /* USER CODE END ADC1_2_IRQn 1 */
}

/**
  * @brief This function handles TIM1 break interrupt.
  */
void TIM1_BRK_IRQHandler(void)
{
  /* USER CODE BEGIN TIM1_BRK_IRQn 0 */

  /* USER CODE END TIM1_BRK_IRQn 0 */
  HAL_TIM_IRQHandler(&htim1);
  /* USER CODE BEGIN TIM1_BRK_IRQn 1 */

  /* USER CODE END TIM1_BRK_IRQn 1 */
}

/**
  * @brief This function handles TIM1 update interrupt.
  */
void TIM1_UP_IRQHandler(void)
{
  /* USER CODE BEGIN TIM1_UP_IRQn 0 */

  /* USER CODE END TIM1_UP_IRQn 0 */
  HAL_TIM_IRQHandler(&htim1);
  /* USER CODE BEGIN TIM1_UP_IRQn 1 */

  /* USER CODE END TIM1_UP_IRQn 1 */
}

/**
  * @brief This function handles TIM1 trigger and commutation interrupts.
  */
void TIM1_TRG_COM_IRQHandler(void)
{
  /* USER CODE BEGIN TIM1_TRG_COM_IRQn 0 */

  /* USER CODE END TIM1_TRG_COM_IRQn 0 */
  HAL_TIM_IRQHandler(&htim1);
  /* USER CODE BEGIN TIM1_TRG_COM_IRQn 1 */

  /* USER CODE END TIM1_TRG_COM_IRQn 1 */
}

/**
  * @brief This function handles TIM1 capture compare interrupt.
  */
void TIM1_CC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM1_CC_IRQn 0 */

  /* USER CODE END TIM1_CC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim1);
  /* USER CODE BEGIN TIM1_CC_IRQn 1 */

  /* USER CODE END TIM1_CC_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles TIM3 global interrupt.
  */
void TIM3_IRQHandler(void)
{
  /* USER CODE BEGIN TIM3_IRQn 0 */

  /* USER CODE END TIM3_IRQn 0 */
  HAL_TIM_IRQHandler(&htim3);
  /* USER CODE BEGIN TIM3_IRQn 1 */

  /* USER CODE END TIM3_IRQn 1 */
}

/**
  * @brief This function handles TIM4 global interrupt.
  */
void TIM4_IRQHandler(void)
{
  /* USER CODE BEGIN TIM4_IRQn 0 */

  /* USER CODE END TIM4_IRQn 0 */
  HAL_TIM_IRQHandler(&htim4);
  /* USER CODE BEGIN TIM4_IRQn 1 */

  /* USER CODE END TIM4_IRQn 1 */
}

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
  /* USER CODE BEGIN USART2_IRQn 0 */

  /* USER CODE END USART2_IRQn 0 */
  HAL_UART_IRQHandler(&huart2);
  /* USER CODE BEGIN USART2_IRQn 1 */

  /* USER CODE END USART2_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */
  /* 蓝牙接收：直接读 DR，绕过 HAL 的接收状态机（原因见 app_rc.c 的 App_RC_Init）。
     USART3 上只有这一个中断源，处理完直接返回，不让 HAL_UART_IRQHandler 再碰
     状态机——否则它一旦看到 ORE，会执行 UART_EndRxTransfer() 把 RXNEIE 关掉，
     接收永久停摆。以后若要给 USART3 加中断发送，需要在这里补 TC 处理。 */
  App_RC_ISR();
  return;
  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */

  /* USER CODE END USART3_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[15:10] interrupts.
  */
void EXTI15_10_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI15_10_IRQn 0 */

  /* USER CODE END EXTI15_10_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_14);
  /* USER CODE BEGIN EXTI15_10_IRQn 1 */

  /* USER CODE END EXTI15_10_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
