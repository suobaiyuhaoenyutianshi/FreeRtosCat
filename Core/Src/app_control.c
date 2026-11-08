#include "app_control.h"
#include "pid.h"
#include "app_mpu6050.h"
#include "qmath.h"
#include "app_motor.h"
#include "app_encoder.h"
#include "app_bat.h"
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

/* ===== 安全限幅 =====
 * 目标轮速上限：取“物理上真能跑到的最大转速”。
 * 换算：地面 1.0 m/s ÷ 车轮半径 0.032 m ≈ 31.25 rad/s，取 31.4 rad/s（约 5 圈/秒）。
 * 意义：wheel_omega_ref_radps 既是积分器的状态量，又直接当电机的目标转速下发，
 *       一旦超过电机实际能力，环路就进入“饱和区”，必须限住。 */
#define WHEEL_OMEGA_REF_MAX_RADPS   (31.4f)

/* ===== 控制器状态 ===== */
static volatile uint64_t last_calc_us = 0;         // 上次控制运算的时间戳 (us)
static float wheel_omega_ref_radps = 0.0f;         // 积分得到的目标轮速 (rad/s)
static Attitude_t last_good_att;                   // 最近一次有效的姿态快照（fail-safe 兜底用）
static uint32_t   att_fail_cnt = 0;                // 连续失效次数（预留：可据此做超阈降级/停车）

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
    /////有问题  记得你要一开始测个默认值
    /* ===== 1. 计算控制周期 dt =====
     * 原理：dt 是积分器的“步长”。积分式是  目标轮速 += 加速度 × dt，
     *       所以 dt 错，积分结果就错，而且误差会在每一步累积。
     * 标称周期是 5ms（见 App_Control_Task 里的 vTaskDelayUntil），
     * 但真实间隔会被这些情况拉长：任务被高优先级抢占、中断占用较久、
     * 调试时下断点暂停、以及上电后的第一次调用。
     *
     * 【单位陷阱】dt 必须在“微秒域”钳位，再转成秒。若在秒域判断，
     * 阈值就会错 100 万倍（100000us 的钳位写成 100000s），形同虚设。
     */
    uint64_t diff_us = now_us - last_calc_us;   // 时间差 (us)，无符号相减，回绕安全
    if (diff_us > 100000u) {                    // 超过 100ms（20×5ms）算异常
        diff_us = 5000u;                        // 回退到标称 5ms，防巨量灌进积分器
    }
    float dt_s = diff_us * 1.0e-6f;             // 真正的时间间隔 (s)
    float wheel_omega_meas = (App_Encoder_GetSpeed_R()+App_Encoder_GetSpeed_L())/2;
    Attitude_t a;
     float pitch_rad       ;
    float pitch_rate_radps ;
   if ( App_MPU6050_GetAttitude(&a)==0) {
     /* 修复前：姿态失效时把 pitch/gx/gz 全部置0 —— 这会让转向环 gz=0 看似"没有偏差"，
        小车会继续直行(假装健康)，违反"禁止静默填充默认值掩盖故障"的约束，最危险。
        修复后：X 用的是"上一帧有效姿态"，而非清零假装健康。
        bound: 若连续失效帧数超阈值(如 10 次=50ms)，表明传感器链路真断了，
        att_fail_cnt 已累计，可在此接入"停车降级"逻辑(当前仅预留计数)。 */
     att_fail_cnt++;
     a = last_good_att;
     pitch_rad        = a.pitch * 0.0174533f;
     pitch_rate_radps = a.gx * 0.0174533f;
   }else {
   /* 修复后新增：每读到一帧有效姿态就缓存一份，供下次失效时兜底 */
     last_good_att = a;
     att_fail_cnt  = 0;
    pitch_rad        = a.pitch * 0.0174533f;  // 倾角 ° →rad
    pitch_rate_radps = a.gx*0.0174533f;// 角速度 °/s → rad/s
   }

    /* ===== 安全停机：倾角超过 ±40° 立即切断电机 =====
     * 倒地/被拿起后继续给轮子出力只会让车乱窜。这里直接拉低 STBY（TB6612 待机
     * = 电机断电），并清掉轮速积分和全部 PID，避免"倒着还在继续加速"的积分残留。
     * 回正到 ±5° 以内才恢复（滞回，防止在 40° 附近反复启停）。 */
    static uint8_t s_pitch_stop = 0;
    if ((pitch_rad > 0.6981f) || (pitch_rad < -0.6981f)) {
        s_pitch_stop = 1;                            /* 40°：停机 */
    } else if ((pitch_rad < 0.0873f) && (pitch_rad > -0.0873f)) {
        s_pitch_stop = 0;                            /* 5°：允许恢复 */
    }

    if (s_pitch_stop) {
        App_Motor_Cmd(0);                            /* STBY 低：电机断电 + 复位电机环 PID */
        App_Contro_setMoveSpeed(0.0f);               /* 清掉遥控残留的速度/转向目标 */
        App_Contro_setTurnSpeed(0.0f);
        wheel_omega_ref_radps = 0.0f;
        PID_Reset(&pid_speed);
        PID_Reset(&pid_pitch);
        PID_Reset(&pid_pitch_rate);
        PID_Reset(&pid_yaw_rate);
        last_calc_us = now_us;
        return;                                      /* 不再向电机环下发目标轮速 */
    }
    App_Motor_Cmd(1);                                /* 正常姿态：保证 STBY 为高（重复写无害） */

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

        /* 积分限幅（anti-windup，抗积分饱和）
         * 原理：wheel_omega_ref_radps 有两个身份——一个是积分器的状态量，
         *       另一个是直接下发给电机速度环的目标值。
         * 为什么必须限：积分器不看电机能不能做到，只按误差不停累加。如果目标值
         *       超过了电机/车实际能达到的转速，电机早就转不动了（饱和），而积分
         *       还在继续涨；等到误差反向时，必须先把这个“多存下来的量”反向积
         *       回去，环路才会重新响应——这段延迟就表现为车失控、压不回来。
       * 限幅值：WHEEL_OMEGA_REF_MAX_RADPS，即物理上真正能跑到的最大转速。 */
        if (wheel_omega_ref_radps > WHEEL_OMEGA_REF_MAX_RADPS) {
            wheel_omega_ref_radps = WHEEL_OMEGA_REF_MAX_RADPS;
        } else if (wheel_omega_ref_radps < -WHEEL_OMEGA_REF_MAX_RADPS) {
            wheel_omega_ref_radps = -WHEEL_OMEGA_REF_MAX_RADPS;
        }
    }
    float yaw_rate_meas_radps = a.gz*0.0174533f;
    float yaw_diff_omega   =  PID_Compute1(&pid_yaw_rate,yaw_rate_meas_radps,now_us) ;
     // 9. 将目标轮速发给电机速度环 
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
    /* 事件驱动修复：控制任务不再用固定 5ms 轮询，而是等姿态任务发来的通知。
       修复前：同周期 + Peek 碰运气读帧，控制环可能用上一帧姿态（相位不确定性）。
       修复后：姿态任务每写完一帧就 vTaskNotifyGive 通知本任务，取到的必是最新帧。
       注意：pdTRUE 清零语义一次清光累积通知，避免"堆积多帧只唤醒一次"的坑。
             初值给 1，保证上电调度器启动后先跑一帧，避免首帧空等。 */
    for(;;){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        App_Control_Proc();
        /* 电量指示：原临时任务删除后，这条 20ms 内部节流的裸机逻辑
           并入控制任务，保证低电量 LED 行为不变 */
        App_Bat_Pro();
    }
}