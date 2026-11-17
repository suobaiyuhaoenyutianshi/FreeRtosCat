#include "app_encoder.h"
#include "app_pwm.h"      /* 上电标定需要直接驱动左右电机 */
#include "delay.h"
#include "math.h"


#include "stm32f1xx_hal_gpio.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* 左右编码器 A 相的实测占空比：高电平时间 / 一个完整周期。
   上电时由 App_Encoder_AutoCalibrate() 自动测量并覆盖；
   这里的初值只是测量失败时的兜底，沿用之前手工测得的数值。 */
float g_duty_l = 0.500f;
float g_duty_r = 0.490f;

static volatile int64_t  encoder_l = 0;
static volatile int64_t  encoder_r = 0;

static volatile int8_t   dir_l = 0;
static volatile int8_t   last_dir_l = 0;
static volatile uint8_t  phase_l = 0;

static volatile int8_t   dir_r = 0;
static volatile int8_t   last_dir_r = 0;
static volatile uint8_t  phase_r = 0;

static volatile uint32_t t0_l = 0, t1_l = 0;
static volatile uint32_t t0_r = 0, t1_r = 0;

/* 测速 M/T 算法的相位权重，由 g_duty_l/g_duty_r 计算，不再写死 */
static float m_l[2];
static float m_r[2];

#define ENC_L_A_PIN        GPIO_PIN_14
#define ENC_L_B_PIN        GPIO_PIN_15
#define ENC_L_GPIO_PORT    GPIOB
#define ENC_L_EXTI_LINE    EXTI_LINE_14

#define ENC_R_A_PIN        GPIO_PIN_3
#define ENC_R_B_PIN        GPIO_PIN_4
#define ENC_R_GPIO_PORT    GPIOB
#define ENC_R_EXTI_LINE    EXTI_LINE_3

/*
22：霍尔元件 11 对极，转一圈产生 22 个方波，所以编码器每圈计数为 22。

1500 / 30613：这看起来是减速比或电子齿轮比，把编码器计数再换算到输出轴角度。

整体公式：

角度弧度
=
encoder_l
×
2
π
22
×
1500
30613
角度弧度=encoder_l× 
22
2π
​
 × 
30613
1500
​
*/


/* ======================== 上电自动标定 ========================
 * 目标：测出左右编码器 A 相各自的占空比（高电平时间 / 完整周期），
 *       写进全局 g_duty_l / g_duty_r，再据此刷新测速权重 m_l / m_r。
 * 做法：调度器启动前，先只转左轮、再只转右轮；跳过起步加速段后，
 *       统计 A 相“高电平总时长”和“低电平总时长”，两者之比就是该轮的占空比。
 * 为什么不“转几毫秒”：A 相一个完整周期约几毫秒，几毫秒只能采到一两个周期，
 *       噪声很大；这里起转 200ms、平均 300ms，能平均上百个周期。
 * ============================================================== */
#define CAL_PWM_DUTY           0.40f   /* 标定时的电机占空比，要高于启动死区 */
#define CAL_SPINUP_MS          200U    /* 起步加速段时间，不参与统计 */
#define CAL_MEASURE_MS         300U    /* 统计窗口 */
#define CAL_STOP_MS            100U    /* 停转后等待，避开残余滑行 */
#define CAL_FEED_INTERVAL_MS   50U     /* 喂狗间隔，IWDG 超时约 410ms */
#define CAL_MIN_EDGES          20U     /* 有效边沿数下限 */
#define CAL_MAX_INTERVAL_TICKS (SystemCoreClock / 20U)  /* >50ms 的间隔视为起步/异常，丢弃 */
#define CAL_DUTY_MIN           0.30f   /* 合理占空比范围，超出判定测量失败 */
#define CAL_DUTY_MAX           0.70f

/* 标定目标：0=不统计，1=左轮，2=右轮 */
static volatile uint8_t  cal_target = 0U;
static volatile uint32_t cal_high_ticks = 0U;   /* A 相高电平总时长（DWT tick） */
static volatile uint32_t cal_low_ticks  = 0U;   /* A 相低电平总时长 */
static volatile uint32_t cal_edges      = 0U;   /* 有效边沿数 */

/**
 * @brief  由 g_duty_l/g_duty_r 重新计算测速权重
 * @note   m[0] 对应 A 相高电平刚结束的阶段，权重 = 2 × 高电平占比；
 *         m[1] 对应低电平阶段，权重 = 2 × 低电平占比。占空比 0.5 时两者都是 1。
 */
static void prvEncoderUpdateWeights(void)
{
    m_l[0] = g_duty_l * 2.0f;
    m_l[1] = 2.0f - m_l[0];
    m_r[0] = g_duty_r * 2.0f;
    m_r[1] = 2.0f - m_r[0];
}

/**
 * @brief  复位编码器计数与测速状态
 * @note   标定前后调用：计数归零；dir/last_dir/t0/t1 清零，使第一次测速返回 0，
 *         避免用标定期间的旧时间戳算出虚假速度。
 */
static void prvEncoderResetState(void)
{
    __disable_irq();
    encoder_l = 0;
    encoder_r = 0;
    dir_l = 0;  last_dir_l = 0;  phase_l = 0;
    dir_r = 0;  last_dir_r = 0;  phase_r = 0;
    t0_l = 0;   t1_l = 0;
    t0_r = 0;   t1_r = 0;
    __enable_irq();
}

/**
 * @brief  标定用的毫秒延时：基于 DWT，且每 50ms 喂一次 IWDG
 * @note   调度器还没启动，不能用 HAL_Delay / vTaskDelay，也不能依赖 MemMon 喂狗。
 */
static void prvCalDelayMs(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
    {
        DelayUs(1000U);
        if (((i + 1U) % CAL_FEED_INTERVAL_MS) == 0U)
        {
            HAL_IWDG_Refresh(&hiwdg);
        }
    }
    HAL_IWDG_Refresh(&hiwdg);
}

void App_Encoder_Init(void)
{
    Delay_Init();
    prvEncoderUpdateWeights();
}

float App_Encoder_GetPos_L(void)
{
    return encoder_l * 0.01399402208920360588844895090594f;
}

float App_Encoder_GetPos_R(void)
{
    return -encoder_r * 0.01399402208920360588844895090594f;
}

float App_Encoder_GetSpeed_L(void)
{
    __disable_irq();
    int8_t dir = dir_l;
    int8_t last = last_dir_l;
    uint8_t phase = phase_l;
    uint32_t t0 = t0_l;
    uint32_t t1 = t1_l;
    __enable_irq();
    uint32_t now = GetTick();
    if(dir ==0 || last ==0)return  0.00f;
    if (dir!= last) {
        return 0.00f;
    }
    uint8_t next_ph =phase ^1;
    float mo = dir * m_l[phase];
    float mNow = dir * m_l[next_ph];
    float M,T;
    if (fabs(mo) * (now - t0) >fabs(mNow)*(t0 - t1)) {
        M = mNow;
        T = TICKS_TO_SEC(now - t0);
    }else {
        M = mo;
        T = TICKS_TO_SEC(t0 - t1);
    }
     return M / T * 0.01399402208920360588844895090594f;

    
}

float App_Encoder_GetSpeed_R(void)
{
    __disable_irq();
    int8_t   dir  = dir_r;
    int8_t   last = last_dir_r;
    uint8_t  ph   = phase_r;
    uint32_t t0   = t0_r;
    uint32_t t1   = t1_r;
    __enable_irq();

    if (dir == 0 || last == 0) return 0.0f;
    if (dir != last) return 0.0f;

    uint32_t now = GetTick();
    uint8_t next_ph = ph ^ 1;

    float M0   = dir * m_r[ph];
    float Mnow = dir * m_r[next_ph];

    float M, T;
    if (fabsf(Mnow) * (t0 - t1) < fabsf(M0) * (now - t0))
    {
        M = Mnow;
        T = TICKS_TO_SEC(now - t0);
    }
    else
    {
        M = M0;
        T = TICKS_TO_SEC(t0 - t1);
    }

    return -M / T * 0.01399402208920360588844895090594f;// encoder_l * (2.0f * PI * 1500.0f / (22.0f * 30613.0f));
}



void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
   if (GPIO_Pin ==GPIO_PIN_14) {
        last_dir_l = dir_l;
        t1_l =t0_l;
        t0_l = GetTick();
        uint8_t A = HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_14);
        uint8_t B = HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_15);
        if (A) {
            phase_l =1;
            if (B) {
                dir_l = 1;  encoder_l++; 
            }else {
                dir_l = -1;  encoder_l--; 
            }    
        }else {
             phase_l = 0;
            if (!B )  { dir_l = 1;  encoder_l++; }
            else                { dir_l = -1; encoder_l--; }
            
        }



   }else if (GPIO_Pin == GPIO_PIN_3) {
      last_dir_r = dir_r;
        t1_r =t0_r;
        t0_r = GetTick();
        uint8_t A = HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_3);
        uint8_t B = HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_4);
        if (A) {
            phase_r =1;
            if (B) {
                dir_r = 1;  encoder_r++; 
            }else {
                dir_r = -1;  encoder_r--; 
            }    
        }else {
             phase_r = 0;
            if (!B )  { dir_r = 1;  encoder_r++; }
            else                { dir_r = -1; encoder_r--; }
            
        }
   }

   /* ---- 上电标定统计（只统计正在标定的那个轮子）----
      边沿刚发生：
        phase == 0 → 刚才是下降沿，刚刚结束的这段是高电平，累加到 high；
        phase == 1 → 刚才是上升沿，刚刚结束的这段是低电平，累加到 low。
      超过 50ms 的间隔说明轮子刚起步/停转，不具代表性，直接丢弃。 */
   if (cal_target == 1U && GPIO_Pin == ENC_L_A_PIN)
   {
       uint32_t dt = t0_l - t1_l;
       if ((dt > 0U) && (dt < CAL_MAX_INTERVAL_TICKS))
       {
           if (phase_l == 0U) { cal_high_ticks += dt; }
           else               { cal_low_ticks  += dt; }
           cal_edges++;
       }
   }
   else if (cal_target == 2U && GPIO_Pin == ENC_R_A_PIN)
   {
       uint32_t dt = t0_r - t1_r;
       if ((dt > 0U) && (dt < CAL_MAX_INTERVAL_TICKS))
       {
           if (phase_r == 0U) { cal_high_ticks += dt; }
           else               { cal_low_ticks  += dt; }
           cal_edges++;
       }
   }
}

void App_Encoder_StartCalibration(void) { }
int App_Encoder_EndCalibration(float *duty_l, float *duty_r)
{
    *duty_l = g_duty_l;
    *duty_r = g_duty_r;
    return 0;
}

/**
 * @brief  标定单个轮子的编码器 A 相占空比
 * @param  target   1=左轮，2=右轮
 * @param  duty_out 测量成功时写入占空比
 * @retval 0=成功，-1=失败（边沿太少 / 高或低电平为 0 / 结果超出合理范围）
 * @note   只让目标轮转动，另一个轮子保持 0，避免机械耦合干扰。
 */
static int prvCalibrateWheel(uint8_t target, float *duty_out)
{
    /* 1. 两轮停转、状态清零，确保本次测量从干净状态开始 */
    App_PWM_Set_L(0.0f);
    App_PWM_Set_R(0.0f);
    prvEncoderResetState();

    /* 2. 只驱动目标轮，先跑过起步加速段 */
    if (target == 1U) { App_PWM_Set_L(CAL_PWM_DUTY); }
    else              { App_PWM_Set_R(CAL_PWM_DUTY); }

    prvCalDelayMs(CAL_SPINUP_MS);

    /* 3. 打开统计窗口：清空累加器，让中断只统计目标轮 */
    __disable_irq();
    cal_high_ticks = 0U;
    cal_low_ticks  = 0U;
    cal_edges      = 0U;
    cal_target     = target;
    __enable_irq();

    prvCalDelayMs(CAL_MEASURE_MS);

    /* 4. 关统计并拍快照 */
    __disable_irq();
    cal_target = 0U;
    uint32_t high  = cal_high_ticks;
    uint32_t low   = cal_low_ticks;
    uint32_t edges = cal_edges;
    __enable_irq();

    /* 5. 停转并等待滑行结束 */
    App_PWM_Set_L(0.0f);
    App_PWM_Set_R(0.0f);
    prvCalDelayMs(CAL_STOP_MS);

    /* 6. 数据有效性检查 */
    if ((edges < CAL_MIN_EDGES) || (high == 0U) || (low == 0U))
    {
        return -1;
    }

    float duty = (float)high / (float)(high + low);
    if ((duty < CAL_DUTY_MIN) || (duty > CAL_DUTY_MAX))
    {
        return -1;
    }

    *duty_out = duty;
    return 0;
}

/**
 * @brief  上电自动标定左右编码器 A 相占空比
 * @note   必须在调度器启动前、车轮架空时调用：
 *           - TIM2 此刻只喂 HAL 时基，不会触发 PID；
 *           - IWDG 已启动，内部延时每 50ms 喂一次狗；
 *           - 发送队列的消费者任务还没跑，因此用阻塞 HAL_UART_Transmit 打印。
 *         标定失败时保留原来的 g_duty_l/g_duty_r，绝不用不可信数据覆盖。
 */
void App_Encoder_AutoCalibrate(void)
{
    float duty;
    char line[80];
    int len;

    App_PWM_Cmd(1);   /* 使能 TB6612，否则标定期间电机不会转 */

    if (prvCalibrateWheel(1U, &duty) == 0)
    {
        g_duty_l = duty;
        len = snprintf(line, sizeof(line), "CAL L: %lu/1000\r\n",
                       (unsigned long)(g_duty_l * 1000.0f + 0.5f));
    }
    else
    {
        len = snprintf(line, sizeof(line), "CAL L: FAIL, keep %lu/1000\r\n",
                       (unsigned long)(g_duty_l * 1000.0f + 0.5f));
    }
    if (len > 0) { HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)len, 100U); }

    if (prvCalibrateWheel(2U, &duty) == 0)
    {
        g_duty_r = duty;
        len = snprintf(line, sizeof(line), "CAL R: %lu/1000\r\n",
                       (unsigned long)(g_duty_r * 1000.0f + 0.5f));
    }
    else
    {
        len = snprintf(line, sizeof(line), "CAL R: FAIL, keep %lu/1000\r\n",
                       (unsigned long)(g_duty_r * 1000.0f + 0.5f));
    }
    if (len > 0) { HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)len, 100U); }

    /* 7. 用新占空比重算测速权重，并清掉标定期间的计数/时间戳 */
    prvEncoderUpdateWeights();
    prvEncoderResetState();
}