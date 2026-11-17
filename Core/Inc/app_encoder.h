#ifndef APP_ENCODER_H
#define APP_ENCODER_H
#include "main.h"

void App_Encoder_Init(void);
float App_Encoder_GetPos_L(void);
float App_Encoder_GetPos_R(void);
float App_Encoder_GetSpeed_L(void);
float App_Encoder_GetSpeed_R(void);

/* 左右编码器 A 相占空比（高电平时间 / 完整周期），上电自动测量后写入 */
extern float g_duty_l;
extern float g_duty_r;

/* 上电自动标定：分别转动左右轮，测量并更新 g_duty_l/g_duty_r */
void App_Encoder_AutoCalibrate(void);

// 编码器占空比校准（如果你后面要做）
void App_Encoder_StartCalibration(void);
int  App_Encoder_EndCalibration(float *duty_l, float *duty_r);
   

#endif