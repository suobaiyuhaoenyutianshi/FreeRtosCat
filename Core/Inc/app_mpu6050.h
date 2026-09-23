#ifndef APP_MPU6050_H
#define APP_MPU6050_H
#include "main.h"
#include <stdint.h>


/* ============ 姿态数据（邮箱里传的就是这个结构体） ============
 * 单位故意和下面那组 getter 保持一致，这样控制环从"读 getter"换成
 * "收邮箱"时不用改任何换算。
 * ⚠️ 这个 typedef 只能有这一处定义，别再在别的 .c 里抄一份 ——
 *    C 的类型只在单个 .c 文件里生效，两份写得不一致编译器也发现不了，
 *    运行时会静默写坏内存。
 */
typedef struct {
    float    pitch;     /* 倾角，单位：度（deg） */
    float    gx;        /* 倾角角速度，单位：°/s */
    float    gz;        /* 偏航角速度，单位：°/s */
    uint32_t tick;      /* 采样时刻，用来判断"这份数据是不是太旧了" */
} Attitude_t;

void App_MPU6050_Init(void);
void App_MPU6050_Proc(void);   // 每 5ms 调用一次，进行姿态更新与融合

void App_MPU6050_Update(void); // 读取传感器原始数据并换算

/* FreeRTOS 姿态任务（周期读取 + 解算，替代主循环里的 App_MPU6050_Proc） */
void App_Attitude_Task(void *argument);

/**
 * @brief  从"覆盖式邮箱"里窥探一份最新姿态（不移除，多个任务都能反复读）
 * @param  out  输出参数，成功时写入姿态数据
 * @return 1 = 成功；0 = 邮箱还空着（刚上电，姿态任务还没跑第一轮）
 * @note   失败时 out 的内容未定义，调用者必须先判断返回值再使用
 */
uint8_t App_MPU6050_GetAttitude(Attitude_t *out);

// 以下 getter 直接返回全局变量，单位见注释
float App_MPU6050_GetAx(void);
float App_MPU6050_GetAy(void);
float App_MPU6050_GetAz(void);
float App_MPU6050_GetTemperature(void);
float App_MPU6050_GetGx(void);
float App_MPU6050_GetGy(void);
float App_MPU6050_GetGz(void);
float App_MPU6050_GetYaw(void);
float App_MPU6050_GetPitch(void);
float App_MPU6050_GetRoll(void);


#endif