#ifndef LPF_H
#define LPF_H

#include <stdint.h>

typedef struct
{
	float Tf; // 低通滤波器的时间常数
	float LastOutput;  // 上次低通滤波器的输出，用于迭代运算
	uint32_t LastTime; // 上次计算的 tick 时间戳（DWT CYCCNT），用于计算Δt
} LPF_TypeDef;

 void LPF_Init(LPF_TypeDef *Lpf, float Tf);
float LPF_Calc(LPF_TypeDef *Lpf, float Input, uint32_t now);

#endif
