/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "memory_monitor.h"
#include "portmacro.h"
#include "projdefs.h"
#include "task.h"
#include "app_control.h"
#include "app_pwm.h"
#include "app_bat.h"
#include "button.h"
#include "stm32f103xb.h"
#include "stm32f1xx_hal_adc_ex.h"
#include "stm32f1xx_hal_gpio.h"
#include "stm32f1xx_hal_uart.h"
#include <stdint.h>
#include <sys/_types.h>
#include "app_usart2_dma.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#include "app_bat.h"
#include "app_pwm.h"
#include "delay.h"
#include "app_encoder.h"
#include "app_mpu6050.h"
#include "app_rc.h"
#include "button.h"
#include "app_motor.h"
#include "memory.h"
#include "app_usart2_dma.h" 
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

IWDG_HandleTypeDef hiwdg;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart2_tx;
DMA_HandleTypeDef hdma_usart3_rx;

/* USER CODE BEGIN PV */
TaskHandle_t mot;
/* 诊断容器定义（类型在 main.h 的 diag_ctx_t）：
   TIM2 中断次数、中断屏蔽状态、I2C 返回值全部在这里，Watch 里展开 g_diag 即可 */
volatile diag_ctx_t g_diag = {0};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_IWDG_Init(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
  float AAP;
  volatile uint32_t ssda = 0;   /* 调试面包屑：volatile 保证每次都真写内存，调试器里能看到变化 */
  volatile float test_var = 3.14f; 
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
int s =1;
  if ( s> 0) { AAP = 1.0f; }
  
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_TIM1_Init();
  MX_TIM4_Init();
  MX_USART2_UART_Init();
  MX_TIM3_Init();
  MX_USART3_UART_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */
  /* ---- 调试标记：阻塞式直接发串口，绕过 FreeRTOS 队列/DMA 打印链路 ----
     看到 "B1" 说明 MCU 已经跑到这里，且 USART2 硬件通路正常 */
  HAL_UART_Transmit(&huart2, (uint8_t *)"B1\r\n", 4, 50);
#define BOOT_STEP(c)  do { uint8_t _c = (uint8_t)(c); HAL_UART_Transmit(&huart2, &_c, 1, 20); } while (0)
  /* 调试断点时暂停 IWDG 计数，否则 CPU 一停就被复位（仅连接调试器时生效） */
  __HAL_DBGMCU_FREEZE_IWDG();

  /* ---- 中断优先级统一编排（所有 MX_*_Init 之后执行，覆盖它们设的值）----
   * 数字越小优先级越高；ISR 里要调 FreeRTOS FromISR 接口的，数值必须 >= 5。
   *   TIM2(HAL 1ms 时基) = 5：回调里会 vTaskNotifyGiveFromISR
   *   USART3(蓝牙逐字节)  = 5：回调里会 xSemaphoreGiveFromISR
   *   EXTI(编码器)/ADC/TIM1/TIM3/TIM4 = 6：高频中断，不能压过上面两个，否则 tick 被饿死
   */
  HAL_NVIC_SetPriority(TIM2_IRQn,        5U, 0U);
  HAL_NVIC_SetPriority(USART3_IRQn,      5U, 0U);
  HAL_NVIC_SetPriority(EXTI3_IRQn,       6U, 0U);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn,   6U, 0U);
  HAL_NVIC_SetPriority(ADC1_2_IRQn,      6U, 0U);
  HAL_NVIC_SetPriority(TIM1_BRK_IRQn,    6U, 0U);
  HAL_NVIC_SetPriority(TIM1_UP_IRQn,     6U, 0U);
  HAL_NVIC_SetPriority(TIM1_TRG_COM_IRQn,6U, 0U);
  HAL_NVIC_SetPriority(TIM1_CC_IRQn,     6U, 0U);
  HAL_NVIC_SetPriority(TIM3_IRQn,        6U, 0U);
  HAL_NVIC_SetPriority(TIM4_IRQn,        6U, 0U);
  Delay_Init();//别删这个
  BOOT_STEP('1');
  App_PWM_Init();
  BOOT_STEP('2');

  /* ---- 开环电机测试：紧跟 App_PWM_Init，排除后面初始化卡死的影响 ---- */
  // App_PWM_Set_L(0.5f);           /* 50%，架空车轮看方向；15% 可能低于启动转矩 */
  // App_PWM_Set_R(0.5f);
  // while (1)
  // {
  //   ssda++;                      /* 调试器里能涨 = 代码确实跑到这里了 */
  //   HAL_IWDG_Refresh(&hiwdg);    /* 必须喂，否则 0.41s 后复位 */
  //   HAL_Delay(50);
  // }

  APP_batInit();
  BOOT_STEP('3');
  App_Encoder_Init();
  BOOT_STEP('4');
  App_RC_Init();
  BOOT_STEP('5');
  App_MPU6050_Init();
  BOOT_STEP('6');
  /* IWDG 在 MX_IWDG_Init 就已启动，但此时调度器还没起、没任务喂狗；
     初始化里耗时的 HAL_Delay(100) 之后补喂一次，避免初始化期间被复位 */
  HAL_IWDG_Refresh(&hiwdg);
  BOOT_STEP('7');
  App_Control_Init();
  BOOT_STEP('8');
  App_Motor_Init();
  BOOT_STEP('9');
  App_USART2_Init();
  BOOT_STEP('A');

  TaskHandle_t att;TaskHandle_t ctrl;TaskHandle_t rc;
  
  /* 创建控制任务：栈 384 字，优先级 4，周期 5ms。
     事件驱动修复：必须【先创建】控制任务，才能把它的句柄 ctrl 传给姿态任务作通知目标 */
  xTaskCreate(App_Control_Task, "ctrl", 384, NULL, 4, &ctrl);

  /* 创建姿态任务：栈 256 字（= 1KB），优先级 3。
     事件驱动修复：把 ctrl 传给它，姿态任务写完邮箱就 vTaskNotifyGive(ctrl) 通知控制任务 */
  xTaskCreate(App_Attitude_Task, "att", 256, ctrl, 3, &att);

  xTaskCreate( App_Motor_Proc,"motor",128,NULL,5,&mot);
  /* 创建遥控接收任务：栈 512 字（2048B）。
     原 256 字（1KB）不够：App_RC_Proc 里跑 newlib 的 sscanf + vsnprintf，
     实测爆栈（vApplicationStackOverflowHook 计数 0→1），钩子会关中断死循环，
     再被 IWDG 复位 → 表现为"一收蓝牙数据就复位、计数永远停在 23/2"。
     现在解析和打印已改成手写实现，512 字留足余量。 */
  xTaskCreate(APP_rc_rxTask, "rc", 512, NULL, 2, &rc);
  vRegisterTaskForMonitoring(att);
  vRegisterTaskForMonitoring(ctrl);
  vRegisterTaskForMonitoring(rc);
  vRegisterTaskForMonitoring(mot);
  vRegisterTaskForMonitoring(usart2);   /* UART2_TX：之前漏注册，栈降到 128 字后必须观测 */
 
  /* 启动内存监控任务，周期性打印各任务栈高水位 + 堆用量 */
  vStartMemoryMonitor();
  /* 启动调度器。正常情况永远不会返回 */
  HAL_UART_Transmit(&huart2, (uint8_t *)"B2\r\n", 4, 50);   /* 看到 B2 = 所有初始化都过了 */
  vTaskStartScheduler();

/*uint8_t TE[] = "holl";
 uint8_t txData[] = "Hello, UART!\r\n";
    uint16_t dataLen = sizeof(txData) - 1; // 不发送末尾的 '\0'
     HAL_UART_Transmit(&huart2, txData, dataLen, HAL_MAX_DELAY);
HAL_UART_Transmit(&huart2,TE,strlen(TE),HAL_MAX_DELAY);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  App_PWM_Cmd(1);
  /* 走到这里说明 vTaskStartScheduler() 启动失败（多半是 FreeRTOS 堆不够，
     堆在 FreeRTOSConfig.h 的 configTOTAL_HEAP_SIZE）。 */
  HAL_UART_Transmit(&huart2, (uint8_t *)"SCHED-FAIL\r\n", 12, 50);
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_8;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */
    
  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_4;
  hiwdg.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 71;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 99;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 71;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 9999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_ENABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 71;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 99;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */
  HAL_TIM_MspPostInit(&htim4);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 921600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 9600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  /* DMA1_Channel7_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel7_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6
                          |GPIO_PIN_9|GPIO_PIN_10, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pins : PA1 PA4 PA5 PA6
                           PA9 PA10 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6
                          |GPIO_PIN_9|GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB14 PB3 */
  GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB15 PB4 */
  GPIO_InitStruct.Pin = GPIO_PIN_15|GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PA11 */
  GPIO_InitStruct.Pin = GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB5 PB7 */
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ---- 爆栈现场快照 ------------------------------------------------------------
 * 钩子运行在 PendSV / 调度器切换上下文的临界位置，此时：
 *   - 不能 pvPortMalloc（堆可能已被溢出踩坏）
 *   - 不能 xQueueSend(portMAX_DELAY)（调度器正在切换，阻塞行为未定义）
 * 所以这里只做"写全局变量 + 关中断死循环"，调试器直接看下面两个变量即可：
 *   g_ulStackOverflowCount : 爆栈次数（不放断点也能知道有没有进过钩子）
 *   g_cOverflowTaskName    : 爆栈的任务名
 * 断点建议打在本函数第一行（Count++ 那行）。
 * --------------------------------------------------------------------------- */
volatile uint32_t g_ulStackOverflowCount = 0;
volatile char     g_cOverflowTaskName[configMAX_TASK_NAME_LEN] = {0};

/**
  * @brief  任务栈溢出钩子
  * @note   configCHECK_FOR_STACK_OVERFLOW = 2 时必须由用户提供，否则链接报错。
  *         只有任务被切出去时才会检查，深溢出可能先 HardFault 而进不到这里。
  */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  g_ulStackOverflowCount++;
  for (uint32_t i = 0; (i < (configMAX_TASK_NAME_LEN - 1U)) && (pcTaskName[i] != '\0'); i++)
  {
    g_cOverflowTaskName[i] = pcTaskName[i];
  }
  g_cOverflowTaskName[configMAX_TASK_NAME_LEN - 1U] = '\0';

  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

/* ---- 堆分配失败现场 --------------------------------------------------------
 * pvPortMalloc 返回 NULL 时由 heap_4.c 调用（configUSE_MALLOC_FAILED_HOOK=1）。
 * 与爆栈钩子同样原则：只写全局变量 + 关中断死循环，不碰堆/队列/串口。
 * 调试器里看：
 *   g_ulMallocFailedCount    : 失败次数
 *   g_ulMallocFailedFreeHeap : 失败时的剩余堆（区分"彻底没空间"还是"碎片化不够一块"）
 *   g_pvMallocFailedCaller   : 调用 pvPortMalloc 的返回地址，
 *                              用 arm-none-eabi-addr2line -e <elf> 0x<地址> 定位到源码行
 * ------------------------------------------------------------------------- */
volatile uint32_t g_ulMallocFailedCount = 0;
volatile uint32_t g_ulMallocFailedFreeHeap = 0;
volatile uint32_t g_pvMallocFailedCaller = 0;

void vApplicationMallocFailedHook(void)
{
  g_ulMallocFailedFreeHeap = (uint32_t)xPortGetFreeHeapSize();
  g_pvMallocFailedCaller = (uint32_t)__builtin_return_address(0);
  g_ulMallocFailedCount++;

  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM2 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  BaseType_t xhw = pdFALSE;
  if (htim->Instance == TIM2) { g_diag.tim2_irq_count++; }   /* 诊断：TIM2 中断计数 */
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM2)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  if (htim->Instance == TIM2) { 
    if (mot != NULL ) {
      vTaskNotifyGiveFromISR(mot, &xhw);
      portYIELD_FROM_ISR(&xhw);
    }
  }
 
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
