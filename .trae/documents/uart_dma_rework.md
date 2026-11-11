# UART DMA 整理：USART2 打印统一 + 遥控 USART3 改 DMA 接收

> ⚠️ **阅读提示（2026-10-05 补）**：本文写于**改造之前**，是当时的执行记录。
> 文中出现的 `app_usart1_dma.c / .h`、`app_usart2.c / .h` 已在改造中**重命名或删除**
> （现为 `app_usart2_dma.c / .h`；`app_usart2.c` 已删）。
> 所以本文里指向这两个**旧文件名**的链接点不开是正常的——那是历史文件名，不是链接写错了。
> 要改代码请以 [app_usart2_dma.c](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_usart2_dma.c) 为准。

## Context

项目里 UART/DMA 现在是乱的，三处互相打架：

1. `app_usart1_dma.c` 名字叫 "usart1"，但实际就是 **USART2 的 DMA TX**：它 `extern` 了根本 `main.c` 里不存在的 `huart1` / `hdma_usart1_tx`（[main.c](file:///c:/flushbonading/CARTESTFreertos/Core/Src/main.c#L80-L82) 只有 `huart2` / `huart3` / `hdma_usart2_tx`），回调却去判 `USART2`（[L29](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_usart1_dma.c#L29)）。文件是坏的，link 会挂在未定义符号上。
2. `app_usart2.c` 又用阻塞 `HAL_UART_Transmit(&huart2, ...)`（[L15](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_usart2.c#L15)），和 DMA 版抢同一个 USART2。
3. 遥控 `app_rc.c` 走 USART3 逐字节 IT 接收，每字节都在回调里 `HAL_UART_Receive_IT` 重新武装（[L42](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_rc.c#L42)），低效且要反复开中断。

**目标（用户已确认）：**
- USART2 打印全部统一到 DMA 框架，命名对齐改成 USART2，删掉阻塞版 app_usart2.c。
- 遥控 USART3 改成 DMA + 空闲中断接收（`HAL_UARTEx_ReceiveToIdle_DMA`），一次武装收整行，不再逐字节反复开关中断。

## 现状事实（已核实）

- USART2（921600）已有 DMA TX：`hdma_usart2_tx` = DMA1_Channel7，NVIC 已能（[it.c#L194-L200](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_it.c#L194-L200)），MSP 已配（[hal_msp.c#L412-L429](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_hal_msp.c#L412-L429)）。
- USART3（9600）只有 GPIO + `USART3_IRQn` 已能（[hal_msp.c#L434+](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_hal_msp.c#L434-L454)），**没有 RX DMA**。F1 的 USART3_RX 标准关联是 **DMA1_Channel3**（本机唯一已用的 DMA 通道是 Ch7，Ch3 空闲，无冲突）。`DMA1_Channel3_IRQHandler` 在 it.c 里还不存在。
- 遥控协议：`\n` 结尾的变长文本行（`move <turn> <speed>`），双缓冲 `directIntBuf/proBuf`，`App_RC_Proc()` 解析（[app_rc.c#L13-L90](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_rc.c)）。
- `App_USART1_Printf` 的调用方主要是 [memory_monitor.c](file:///c:/flushbonading/CARTESTFreertos/Core/Src/memory_monitor.c)（约 15 处）；`App_USART2_Printf`（阻塞版）调用方是 [mputest.c](file:///c:/flushbonading/CARTESTFreertos/Core/Src/mputest.c)。

## 改动方案

### Part A — USART2 打印统一到 DMA 框架（去掉阻塞版）

1. **改名文件**：`app_usart1_dma.c` → `app_usart2_dma.c`，`app_usart1_dma.h` → `app_usart2_dma.h`。
2. **符号统一**（新文件内）：
   - `App_USART1_Init` → `App_USART2_Init`，`App_USART1_Printf` → `App_USART2_Printf`，`vUSART1_TxGateKeeperTask` → `vUSART2_TxGateKeeperTask`，任务名 `"UART1_TX"` → `"UART2_TX"`。
   - 修坏引用：`extern UART_HandleTypeDef huart1; extern DMA_HandleTypeDef hdma_usart1_tx;` → `extern UART_HandleTypeDef huart2; extern DMA_HandleTypeDef hdma_usart2_tx;`
   - `HAL_UART_Transmit_DMA(&huart1, ...)` → `&huart2`（[L52](file:///c:/flushbonading/CARTESTFreertos/Core/Src/app_usart1_dma.c#L52)）。回调判 `USART2` 已正确，保留。
   - 头文件里 `extern TaskHandle_t usart1` → `usart2`（文件内 `&usart1` 句柄同名改）。
3. **同步调用方**：
   - `main.c`：`#include "app_usart1_dma.h"` → `app_usart2_dma.h`（[L55](file:///c:/flushbonading/CARTESTFreertos/Core/Src/main.c#L55)），`App_USART1_Init()` → `App_USART2_Init()`（[main.c#L157](file:///c:/flushbonading/CARTESTFreertos/Core/Src/main.c#L157)）。同时删掉 `#include "app_usart2.h"`（[L44](file:///c:/flushbonading/CARTESTFreertos/Core/Src/main.c#L44)），因为 app_usart2.c 被删。
   - `memory_monitor.c`：约 15 处 `App_USART1_Printf` → `App_USART2_Printf`，头文件 include 同步改名。
   - `mputest.c`：把 `#include "app_usart2.h"` 改为 `app_usart2_dma.h`；其 `App_USART2_Printf(...)` 调用无需改名（新函数同名），自动走 DMA。
4. **删除阻塞版**：删 `app_usart2.c` + `app_usart2.h`。（确认无其他引用后删。）

### Part B — 遥控 USART3 改 DMA + 空闲中断接收

1. **加 USART3 RX DMA 句柄**（main.c 全局区 `UART` 旁加 `DMA_HandleTypeDef hdma_usart3_rx;`，仿 `hdma_usart2_tx` 声明 [L82](file:///c:/flushbonading/CARTESTFreertos/Core/Src/main.c#L82)）。
2. **MSP 加 RX DMA 配置**（[hal_msp.c USART3 分支](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_hal_msp.c#L434-L470)）：
   - `hdma_usart3_rx.Instance = DMA1_Channel3; Direction = DMA_PERIPH_TO_MEMORY;` 其余仿 Ch7 参数（MinInc、字节对齐、DMA_NORMAL）。
   - `__HAL_LINKDMA(huart, hdmarx, hdma_usart3_rx);`
   - `HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 5, 0); HAL_NVIC_EnableIRQ(...)`（注意 F1 优先组是 4 位抢占，仿 USART2 用 5）。
3. **it.c 加 DMA1_Channel3_IRQHandler**：仿 [DMA1_Channel7 处理器](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_it.c#L194-L200) 写 `HAL_DMA_IRQHandler(&hdma_usart3_rx);`，顶部加 `extern DMA_HandleTypeDef hdma_usart3_rx;`（仿 L26 的 huart2 extern [it.c#L63-L64](file:///c:/flushbonading/CARTESTFreertos/Core/Src/stm32f1xx_it.c#L63-L64)）。
4. **重写 app_rc.c 接收部分**（保留 `App_RC_Proc` 解析、双缓冲、`move` 命令解析逻辑）：
   - 加 `volatile` 接收缓冲 + `HAL_UARTEx_RxEventCallback`（空闲回调，签名 `(UART_HandleTypeDef*, uint16_t Size)`）。
   - 启动：`App_RC_Init` 里不再逐字节 `HAL_UART_Receive_IT`，改为 `HAL_UARTEx_ReceiveToIdle_DMA(&huart3, pBuf, MAX_FRAME)`。
   - 空闲回调里：把 `Size` 字节当作一行，从 ISR 用队列/信号量投递给 `APP_rc_rxTask`（继续用现有 `xMessage`/解析任务结构），然后重新 `HAL_UARTEx_ReceiveToIdle_DMA` 再武装——**一次武装、空闲即收整行，不再逐字节开关中断**。
   - `APP_rc_rxTask` 收到后调用现成的 `App_RC_Proc()`。

## 关键复用（不要重复造）

- 空闲 DMA 接收用现成 HAL API：`HAL_UARTEx_ReceiveToIdle_DMA` / `HAL_UARTEx_RxEventCallback`（[stm32f1xx_hal_uart.h#L762 / L784](file:///c:/flushbonading/CARTESTFreertos/Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal_uart.h#L762)）。
- 既有技术：`HAL_UARTEx_GetRxEventType(huart)` 可区分 "空闲" vs "收满 Size"，防止静默丢帧。
- 既有框架：TX 守门员任务 + 队列 + `xSemaphoreGiveFromISR/HAL_UART_TxCpltCallback` 这套不动，仅改名。

## 验证

1. **编译**：整个工程应 0 error（重点盯 link：不再有 `huart1`/`hdma_usart1_tx`/`usart1` 未定义；`App_USART1_Printf` 全部改名后无残留）。
2. **USART2 打印**：上板后 memory_monitor 输出的内存/栈监控窗口仍能正常打印（验证 DMA 打印链路还活着）。
3. **遥控 DMA 接收**：串口助手发 `move 30 50\n`，观察平衡车按预期运动；连续发多条验证空闲帧能一条条稳定解析、不丢。可用 `DMA1_Channel3` 断点确认 ISR 被触发。

## 风险 / 注意

- DMA1_Channel3 目前空闲，无冲突；但若未来新增 DMA 请重新核对通道占用。
- F1 中断优先级组为 4 位抢占；DMA/串口优先级沿用工程现有 `5`，别改成抢占比特串位。
- `HAL_UARTEx_ReceiveToIdle_DMA` 需要 MSP 里 `__HAL_LINKDMA(hdmarx)` 与 it.c 的 ISR 都就位，否则回调不触发；三处（main 句柄 / MSP / it.c）必须配套改。