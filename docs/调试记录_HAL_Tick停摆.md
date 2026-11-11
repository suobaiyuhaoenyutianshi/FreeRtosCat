# 调试记录：HAL tick 停摆导致启动卡死（2026-11-07）

## 一、现象

1. 串口每轮固定输出 `B1 1 2 3 4 5 a b`，然后板子复位，约 0.41s 一次，无限循环。
2. 调试器暂停时调用栈：

   ```
   #0 HAL_GetTick()            stm32f1xx_hal.c:306
   #1 HAL_Delay(Delay=100)     stm32f1xx_hal.c:382
   #2 App_MPU6050_Init()       app_mpu6050.c:60
   #3 main()                   main.c:196
   ```

3. `hf_ctx` 全 0 → 没有发生过 HardFault/MemManage/BusFault/UsageFault。
4. `g_tim2_irq_count` 采样值 = 3~4。

## 二、涉及的机制链（为什么卡在这里）

```
HAL_Delay(100)                     阻塞等 100 个 tick
  └─ HAL_GetTick()                 返回 uwTick
      └─ HAL_IncTick()             只有它会让 uwTick += uwTickFreq
          └─ HAL_TIM_PeriodElapsedCallback(TIM2)
              └─ TIM2_IRQHandler
                  └─ TIM2 1ms 更新中断（PSC=71, ARR=999 @72MHz）
                      前提：NVIC 使能 + UIE=1 + 未被 PRIMASK/BASEPRI 屏蔽 + 计数器在跑
```

链上任何一环断开，`uwTick` 冻结 → `HAL_Delay` 的 `while` 永远成立 → IWDG（0.41s）复位。
**表现是"反复复位"，本质是"卡在一个永不返回的循环里"。**

## 三、已有证据能证明什么 / 不能证明什么

| 证据 | 已证明 | 未证明 |
|---|---|---|
| 串口 `B1..b` | 初始化前 5 步、MPU 队列创建、第一次 I2C 写都执行了 | `HAL_Delay` 之后 |
| 栈停在 `HAL_GetTick` | 卡点确实是 HAL 时基，不是 I2C 阻塞 | 谁让 tick 停 |
| `hf_ctx` 全 0 | 不是硬件异常，不是空指针跳转 | — |
| `g_tim2_irq_count = 3~4` | TIM2 中断确实进得来：向量表/NVIC/UIE/handler 全部接对；且上电到进 delay 约 3~4ms | 进入 delay 后中断是否继续 |

**关键反证：`c` 从未出现。** 如果 tick 正常，`HAL_Delay(100)` 最多 101ms 就返回，`c` 必然打印。
因此 tick 在此期间不再推进，问题发生在"进入 delay 之前或那一瞬间"。

## 四、两种可能（待 `g_diag` 判定）

### A. 中断被屏蔽

- `g_diag.primask != 0` → 某处 `__disable_irq()` 没有恢复。
- `g_diag.basepri == 0x50` → FreeRTOS 临界区未退出。`taskENTER_CRITICAL` 会把 BASEPRI 设为
  `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5<<4 = 0x50`，它正好屏蔽优先级 5 的 TIM2。
- 最可疑位置：`App_MPU6050_Init` 里的 `xQueueCreate()`（内部有 `taskENTER_CRITICAL`）。

### B. 定时器停止/中断未武装

- `g_diag.tim2_cnt == g_diag.tim2_cnt_prev` → TIM2 计数器不涨。
- `g_diag.tim2_dier` 的 bit0 = 0 → 更新中断未使能。
- `g_diag.nvic_iser0` 的 bit28 = 0 → NVIC 未使能 TIM2。
- 最可疑位置：`SystemClock_Config()` 里 `HAL_RCC_ClockConfig()` 触发的**第二次 `HAL_InitTick`**
  （`HAL_TIM_Base_Start_IT` 在 `State != READY` 时会直接返回 `HAL_ERROR`）。

## 五、判别字段一览

`g_diag`（类型 `diag_ctx_t`，定义在 `Core/Inc/main.h`，实例在 `Core/Src/main.c`）：

| 字段 | 正常值 | 异常含义 |
|---|---|---|
| `primask` | 0 | 非 0：全局关中断 |
| `basepri` | 0 | 0x50：FreeRTOS 临界区泄漏 |
| `tim2_cnt` / `tim2_cnt_prev` | 不同 | 相同：计数器不涨 |
| `tim2_sr` | bit0 短时为 1 | 恒为 1 且不涨：中断进不去 |
| `tim2_dier` | bit0 = 1 | bit0 = 0：更新中断未使能 |
| `nvic_iser0` | bit28 = 1（0x10000000） | bit28 = 0：NVIC 未使能 |
| `tim2_vector` | 0x08004D81 | 0x080052ED：接到 Default_Handler |
| `tim2_irq_count` | 每秒约 +1000 | 不涨：tick 停 |
| `i2c_last_ret` | 0 | 1=NACK / 2=BUSY / 3=TIMEOUT |

（`tim2_vector` 的期望值来自当前 elf：`TIM2_IRQHandler = 0x08004D80`，Thumb 位 → `0x08004D81`；
`Default_Handler = 0x080052EC` → `0x080052ED`。）

## 六、当前已实施的环境修正

1. IWDG 恢复 `IWDG_PRESCALER_4`（超时约 0.41s）。
2. 中断优先级重排（`main.c` USER CODE 2）：
   - `TIM2_IRQn` = 5（允许调用 FreeRTOS API 的最高优先级，`configMAX_SYSCALL_INTERRUPT_PRIORITY=5`）
   - `EXTI3 / EXTI15_10`（编码器）= 6，`ADC1_2` = 6，`TIM1/TIM3/TIM4` = 6
   - 目的：高频 EXTI 原本是优先级 0，会饿死优先级 15 的 TIM2。

## 七、下一步

1. 烧 Debug，暂停后展开 `g_diag`，按第五节判 A 还是 B。
2. 若 A：查 `xQueueCreate` 前后 BASEPRI 是否被恢复（在 `boot_mark` 快照里已包含）。
3. 若 B：查第二次 `HAL_InitTick` 的返回值，确认 `HAL_TIM_Base_Start_IT` 是否返回 `HAL_ERROR`。
4. 修复后移除调试路标（`B1`/`1..9`/`A`/`a..f`、`BOOT_STEP`）与临时诊断代码。

## 八、根因确认（同日补充）：调度器启动前的临界区把中断永久屏蔽

### 证据链

1. `freeRTOS/Src/port.c:141`：`static UBaseType_t uxCriticalNesting = 0xaaaaaaaa;`（初值不是 0）。
2. `port.c:427-453`：
   - `vPortEnterCritical()` → `portDISABLE_INTERRUPTS()`（`BASEPRI = 5<<4 = 0x50`），然后 `uxCriticalNesting++`
   - `vPortExitCritical()` → 只有 `uxCriticalNesting == 0` 时才 `portENABLE_INTERRUPTS()`
   - `0xaaaaaaaa` 加减 1 永远到不了 0 → **退出临界区不会恢复中断**
3. `freeRTOS/Src/queue.c:318`：`xQueueGenericReset()` **无条件** `taskENTER_CRITICAL()`，
   所以 `xQueueCreate` / `xSemaphoreCreateBinary` / `xTaskCreate` 都会触发上述屏蔽。
4. `port.c:400`：只有 `xPortStartScheduler()` 才把 `uxCriticalNesting` 清 0；
   真正解除屏蔽的是 `vPortSVCHandler` 里的 `msr basepri, #0`。
   **在调度器启动之前，BASEPRI 一直保持 0x50。**

### 本工程的触发顺序

```
App_PWM_Init()          无 RTOS 调用
APP_batInit()           无
App_Encoder_Init()      无
App_RC_Init()            xSemaphoreCreateBinary()  ← 第一次进临界区，中断从此被屏蔽
App_MPU6050_Init()       HAL_Delay(100)  ← 依赖 TIM2 中断推进 uwTick → 永远等不到 → 死循环
```

因此：

- `g_tim2_irq_count = 3~4` = TIM2 从上电跑到 `App_RC_Init` 的 3~4ms，之后被屏蔽；
- 把 TIM2 优先级提到 5 **仍然无效**，因为 `BASEPRI = 0x50` 屏蔽的是所有 ≥5 的中断；
- `hf_ctx` 全 0 = 不是硬件异常；
- 最终现象 = `HAL_Delay` 死循环 + IWDG 复位循环。

### 修复

`Core/Src/app_mpu6050.c`：调度器启动前的 `HAL_Delay(100)` 改为 `DelayUs(100000)`（DWT 周期计数，
不依赖中断）。代码里已加注释说明原因。

### 通用规则（以后不要再踩）

1. **调度器启动前，任何依赖中断的等待都不能放在 FreeRTOS 对象创建之后**（`HAL_Delay`、依赖 tick 的超时、
   等待中断置标志的轮询）。
2. 更稳的做法：把所有 `xQueueCreate / xSemaphoreCreate* / xTaskCreate` 集中放到 `vTaskStartScheduler()`
   之前最后一段，前面只做纯硬件初始化；确实需要在中间延时，就用 DWT/`DelayUs`。
3. 这一条同样影响 I2C/UART 的 `HAL_xxx(..., timeout)`：屏蔽期间 `HAL_GetTick()` 不涨，超时永远不会触发。

