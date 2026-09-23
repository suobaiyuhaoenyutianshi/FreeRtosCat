#include "app_control.h"
#include "pid.h"
#include "app_mpu6050.h"
#include "qmath.h"
#include "app_motor.h"
#include "app_encoder.h"
#include "delay.h"
#include "sched.h"
#include <stdint.h>
#include <sys/_types.h>
#include "FreeRTOS.h"
#include "task.h"

/* ===== 机械常数 ===== */
static const float GRAVITY_MPS2   = 9.8f;    // 重力加速度 m/s²
static const float PENDULUM_LEN_M = 0.062f;  // 质心到轮轴距离 m
static const float WHEEL_RADIUS_M = 0.032f;  // 车轮半径 m

/* ===== 控制器状态 ===== */
static volatile uint64_t last_calc_us = 0;         // 上次控制运算的时间戳 (us)
static float wheel_omega_ref_radps = 0.0f;         // 积分得到的目标轮速 (rad/s)

//PID 控制器定义
static PID_TypeDef pid_speed;      //速度环     输入 m/s      输出 m/s²
static PID_TypeDef pid_pitch;      //倾角环     输入 rad      输出 rad/s
static PID_TypeDef pid_pitch_rate; //角速度环   输入 rad/s    输出 rad/s²
static PID_TypeDef pid_yaw_rate;   //转向环     输入 rad/s    输出 rad/s



void App_Control_Init(void){
    PID_InitTypeDef pid_cfg;

    //速度环 PI 控制器
    pid_cfg.Kp = 10.0f;pid_cfg.Ki =1.0f;pid_cfg.Kd = 0.0f;
    pid_cfg.Setpoint = 0.0f;                  // 初始目标速度 0 m/s
    pid_cfg.OutputUpperLimit = +0.5f * GRAVITY_MPS2;     // 加速度上限 +0.5g ≈ +4.9 m/s²
    pid_cfg.OutputLowerLimit = -0.5f * GRAVITY_MPS2;     // 加速度下限 -0.5g
    pid_cfg.DefaultOutput = 0.0f;             // 默认输出（积分初值）
    PID_Init(&pid_speed, &pid_cfg);
    //倾角环
    pid_cfg.Kp = 4.0f;pid_cfg.Ki = 0.0f;pid_cfg.Kd = 0.0f;
    pid_cfg.OutputUpperLimit = +12.57f;       // 角速度上限 ±12.57 rad/s (≈720°/s)
    pid_cfg.OutputLowerLimit = -12.57f;
    pid_cfg.DefaultOutput = 0.0f;
    pid_cfg.Setpoint = 0.0f;                  // 初始目标倾角 0 rad
    PID_Init(&pid_pitch, &pid_cfg);
    //角速度环
    pid_cfg.Kp =10.0f;pid_cfg.Ki = 10.0f;pid_cfg.Kd = 0.0f;
    pid_cfg.Setpoint = 0.0f;                  // 初始目标角速度 0 rad/s
    pid_cfg.OutputUpperLimit = +125.7f;       // 角加速度上限 ±125.7 rad/s²
    pid_cfg.OutputLowerLimit = -125.7f;
    pid_cfg.DefaultOutput = 0.0f;
    PID_Init(&pid_pitch_rate, &pid_cfg);
    //转向环
    pid_cfg.Kp = 1.0f;pid_cfg.Ki =0.0f;pid_cfg.Kd =0;pid_cfg.Setpoint = 0.0f;pid_cfg.DefaultOutput =0.0f;pid_cfg.OutputUpperLimit = 15.0f;pid_cfg.OutputLowerLimit =-15.0f;
    PID_Init(&pid_yaw_rate,&pid_cfg);
}
/**
 * @brief  平衡车控制主进程（串级 PID 核心）
 * @note   内部 PERIODIC(5) 保证每 5ms 执行一次。
 *         调用顺序：速度环 → 加速度转倾角 → 角度环 → 角速度环 → 逆解算 → 积分 → 电机命令
 */
void App_Control_Proc(void){
    uint64_t now_us = GetUs();
    float dt_s = (now_us - last_calc_us)*1.0e-6f;//真正的时间间隔
    float wheel_omega_meas = (App_Encoder_GetSpeed_R()+App_Encoder_GetSpeed_L())/2;
    Attitude_t a;
     float pitch_rad       ;
    float pitch_rate_radps ;
   if ( App_MPU6050_GetAttitude(&a)==0) {
     pitch_rad        = 0.00f;  // 倾角 ° →rad
    pitch_rate_radps = 0.00f;
    a.pitch=0;a.gx=0;
    a.gz=0;
   }else {
   
    pitch_rad        = a.pitch * 0.0174533f;  // 倾角 ° →rad
    pitch_rate_radps = a.gx*0.0174533f;// 角速度 °/s → rad/s
   }
// ===== 3. 融合编码器与陀螺仪，计算真实水平速度 vel_body_mps (m/s) =====
    // 车轮转动速度包含两部分：车身转动导致轮子跟着转 + 真正水平移动
    // 车身以 pitch_rate_radps 角速度旋转时，轮子中心线速度 = pitch_rate_radps * (PENDULUM_LEN_M + WHEEL_RADIUS_M)
    // 换算成轮子角速度 = pitch_rate_radps * (PENDULUM_LEN_M + WHEEL_RADIUS_M) / WHEEL_RADIUS_M，方向相反
//     后撤让编码器读数变负（假速度）。

// 前倾让陀螺仪读数变正。

// 合成公式（编码器 + 陀螺仪补偿）把假速度抵消，得出真速度 = 0。

// 控制器看到真速度是0，目标速度是正，误差很大，于是坚定地继续命令前倾。

// 重力开始真正把车往前推，真速度开始变正，系统进入稳定加速
    float wheel_omega_from_tilt = -(PENDULUM_LEN_M + WHEEL_RADIUS_M)* pitch_rate_radps / WHEEL_RADIUS_M;// 由车身旋转引起的等效轮速

    float wheel_omega_translation = wheel_omega_meas - wheel_omega_from_tilt;// 纯水平移动的轮速
    float vel_body_mps  = wheel_omega_translation * WHEEL_RADIUS_M;   // 水平速度 (m/s)
     // ===== 4. 速度环 PID：速度误差 → 所需加速度 =====
    // PID 输出直接代表目标水平加速度 accel_ref_mps2 (m/s²)
    // 物理原理：Kp*error 产生与误差成正比的加速度，积分项消除稳态误差
    float accel_ref_mps2 = PID_Compute1(&pid_speed,vel_body_mps,now_us);
    // 加速度 → 目标倾角：重力分量提供加速度，a = g*tanθ → θ = atan(a/g)
    float pitch_ref_rad = qatan(accel_ref_mps2/GRAVITY_MPS2);
    // ===== 5. 角度环 PID：倾角误差 → 目标角速度 =====
    PID_ChangeSetpoint(&pid_pitch,pitch_ref_rad);
    float pitch_rate_ref_radps = PID_Compute1(&pid_pitch,pitch_rad,now_us);
    // ===== 6. 角速度环 PID：角速度误差 → 目标角加速度 =====
    PID_ChangeSetpoint(&pid_pitch_rate,pitch_rate_ref_radps);
    float pitch_accel_ref_radps2 = PID_Compute1(&pid_pitch_rate,pitch_rate_radps,now_us);
    // ===== 7. 逆解算：角加速度 → 车轮线加速度 =====
    // 根据倒立摆动力学：车轮加速度 accel_wheel_ref_mps2 = (g*sinθ - pitch_accel_ref * PENDULUM_LEN_M) / cosθ
    float accel_wheel_ref_mps2 = (GRAVITY_MPS2 * qsin(pitch_rad) - pitch_accel_ref_radps2 * PENDULUM_LEN_M) / qcos(pitch_rad);
    if (last_calc_us!=0) {
        wheel_omega_ref_radps += (1.0f / WHEEL_RADIUS_M) * accel_wheel_ref_mps2 * dt_s;
    }
    float yaw_rate_meas_radps = a.gz*0.0174533f;
    float yaw_diff_omega   =  PID_Compute1(&pid_yaw_rate,yaw_rate_meas_radps,now_us) ;
     // ===== 9. 将目标轮速发给电机速度环 =====
    App_Motor_SetOmega_L(wheel_omega_ref_radps-yaw_diff_omega);
    App_Motor_SetOmega_R(wheel_omega_ref_radps+yaw_diff_omega);

    last_calc_us = now_us; // 保存本次时间戳

}
/**
 * @brief  复位控制系统（清除所有积分和状态）
 */
void App_Control_Reset(void)
{
    last_calc_us  = 0;
    wheel_omega_ref_radps = 0.0f;

    PID_Reset(&pid_speed);
    PID_Reset(&pid_pitch);
    PID_Reset(&pid_pitch_rate);
    PID_Reset(&pid_yaw_rate);
}

void App_Contro_setMoveSpeed(float target_speed){
    PID_ChangeSetpoint(&pid_speed,target_speed);
}
void App_Contro_setTurnSpeed(float target_speed){
    PID_ChangeSetpoint(&pid_yaw_rate,target_speed);
}
void App_Control_Task(void *argument){
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    for(;;){
        App_Control_Proc();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5U));
    }
}