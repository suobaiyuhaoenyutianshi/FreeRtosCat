#include "app_pwm.h"
#include "main.h"
#include "math.h"

/*
 * 占空比标度（全工程唯一约定）：
 *   App_PWM_Set_L/R 的入参是【归一化占空比】，范围 -1.0 ~ +1.0，符号表示方向，1.0 = 100%。
 *   直接对接 app_motor.c 的 duty = vol_l / totalBatVol（0~1）。
 *   禁止再传百分数（例如传 50 表示 50%），否则会被钳位成 100%。
 */
#define PWM_ARR 99  // 必须和 CubeMX 里 TIM1/TIM4 的 ARR 一致
#define PWM_DUTY_MAX 1.0f
#define PWM_CCR_MAX  (PWM_ARR + 1)   /* 满占空比对应的 CCR：100 > ARR=99 → 恒高 */

/*
 * 引脚功能说明
 * 左电机：PA9=AIN1, PA10=AIN2, PA8=TIM1_CH1(PWM)
 * 右电机：PB5=BIN1, PB7=BIN2, PB6=TIM4_CH1(PWM)
 * STBY：PA1（高电平使能）
 **/

void App_PWM_Init(void)
{
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
}

void App_PWM_Cmd(uint8_t State)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, State ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/*
 * 左电机方向控制
 * 正 Duty → 左轮向前 → AIN1 低, AIN2 高
 * 负 Duty → 左轮向后 → AIN1 高, AIN2 低
 */
void App_PWM_Set_L(float Duty)
{
    if (Duty > PWM_DUTY_MAX)  Duty = PWM_DUTY_MAX;
    if (Duty < -PWM_DUTY_MAX) Duty = -PWM_DUTY_MAX;
    if (Duty >= 0)
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9,  GPIO_PIN_RESET); // AIN1 低
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_SET);   // AIN2 高
    }
    else
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9,  GPIO_PIN_SET);   // AIN1 高
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_RESET); // AIN2 低
    }
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint16_t)(fabsf(Duty) * PWM_CCR_MAX));
}

/*
 * 右电机方向控制
 * 正 Duty → 右轮向前 → BIN1 高, BIN2 低
 * 负 Duty → 右轮向后 → BIN1 低, BIN2 高
 */
void App_PWM_Set_R(float Duty)
{
    if (Duty > PWM_DUTY_MAX)  Duty = PWM_DUTY_MAX;
    if (Duty < -PWM_DUTY_MAX) Duty = -PWM_DUTY_MAX;

    if (Duty >= 0)
    {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);   // BIN1 高
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET); // BIN2 低
    }
    else
    {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET); // BIN1 低
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);   // BIN2 高
    }
    
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, (uint16_t)(fabsf(Duty) * PWM_CCR_MAX));
}