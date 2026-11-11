/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
/* 诊断容器：tick/中断屏蔽现场 + 运行统计，Watch 里展开这一个结构体就够了 */
typedef struct
{
    /* boot_mark() 时刻的快照（最后一次路标，即进 HAL_Delay 之前） */
    uint32_t primask;        /* 全局中断屏蔽，非 0 = 关中断 */
    uint32_t basepri;        /* 临界区阈值，0x50 会屏蔽优先级 5 的 TIM2 */
    uint32_t tim2_cnt;       /* TIM2 计数器，应在递增 */
    uint32_t tim2_cnt_prev;  /* 上一次路标时的 CNT，用来比较是否在计数 */
    uint32_t tim2_sr;        /* bit0 = UIF 更新标志 */
    uint32_t tim2_dier;      /* bit0 = UIE 更新中断使能 */
    uint32_t nvic_iser0;     /* bit28 = TIM2_IRQn 在 NVIC 里使能 */
    uint32_t tim2_vector;    /* 向量表 TIM2 项实际指向地址（应为 0x08004D81） */

    /* 运行统计 */
    uint32_t tim2_irq_count; /* TIM2 中断进过几次 */
    uint32_t i2c_last_ret;   /* 最近一次 HAL_I2C_Mem_Write 返回值：0=OK 1=ERR 2=BUSY 3=TIMEOUT */
} diag_ctx_t;

extern volatile diag_ctx_t g_diag;
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */
extern ADC_HandleTypeDef hadc1;
extern I2C_HandleTypeDef hi2c1;
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern IWDG_HandleTypeDef hiwdg;
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
