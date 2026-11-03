# 变更记录：UART DMA 迁移（补齐 de89244 自洽性）

- 修复提交：`263194d` refactor(uart): 迁移至 app_usart2_dma 并移除旧串口 DMA 实现
- 相关前置提交：`de89244` fix: 清理平衡车临时任务残留并按总账调整堆大小
- 记录时间：2026-11-03

## 一、问题背景

`de89244` 的 `Core/Src/main.c` 已改为：

- `#include "app_usart2_dma.h"`（原 `#include "app_usart1_dma.h"`）
- 调用 `App_USART2_Init()`（原 `App_USART1_Init()`）
- 爆栈钩子改用 `App_USART2_Printf()` 打印任务名

但该提交只按文件粒度带上了 `main.c`，UART 相关的其余改动仍留在工作区：

- 新增文件 `Core/Src/app_usart2_dma.c`、`Core/Inc/app_usart2_dma.h` 未跟踪
- 旧文件 `app_usart1_dma.c/.h`、`app_usart2.c/.h` 处于「已删除未提交」

后果：单独检出 `de89244` 会直接编译失败：

```
fatal error: app_usart2_dma.h: No such file or directory
```

本次提交把整批 UART DMA 迁移作为一个自洽单元入库，使任意一次干净检出都能编译。

## 二、迁移前后文件对应表

| 迁移前 | 迁移后 | 说明 |
| --- | --- | --- |
| `Core/Src/app_usart1_dma.c`、`Core/Inc/app_usart1_dma.h` | `Core/Src/app_usart2_dma.c`、`Core/Inc/app_usart2_dma.h` | 改名：旧文件虽叫 USART1，实现实际挂在 USART2，命名与实际串口相反；接口由 `App_USART1_Init/App_USART1_Printf` 改为 `App_USART2_Init/App_USART2_Printf` |
| `Core/Src/app_usart2.c`、`Core/Inc/app_usart2.h` | 删除 | 旧实现是阻塞式 `HAL_UART_Transmit`，功能并入 DMA 版本 |
| `Core/Src/memory_monitor.c` | 修改（仅包含与调用名） | `app_usart1_dma.h → app_usart2_dma.h`，`App_USART1_Printf → App_USART2_Printf`，避免引用已删头文件 |
| `Core/Src/mputest.c` | 修改（仅包含） | `app_usart2.h → app_usart2_dma.h` |
| `Core/Src/app_rc.c` | 修改 | USART3 接收由逐字节 `HAL_UART_Receive_IT` 改为 `HAL_UARTEx_ReceiveToIdle_DMA` + 二值信号量通知解析任务 |
| `Core/Src/stm32f1xx_hal_msp.c` | 修改 | 增加 USART3_RX 的 DMA 句柄 `hdma_usart3_rx`（DMA1_Channel3）Init/DeInit 与 `__HAL_LINKDMA` |
| `Core/Src/stm32f1xx_it.c`、`Core/Inc/stm32f1xx_it.h` | 修改 | 增加 `DMA1_Channel3_IRQHandler` 及其声明 |
| `CARTESTFreertos.ioc` | 修改 | 增加 `DMA.Request1=USART3_RX` 与 `NVIC.DMA1_Channel3_IRQn` 配置 |

迁移后的 USART2 发送模型：业务任务 `App_USART2_Printf` 只做 `vsnprintf` 入队（`xTxQueue`，长度 5），守门员任务 `UART2_TX`（栈 256 字、优先级 2）负责 `HAL_UART_Transmit_DMA`，`HAL_UART_TxCpltCallback` 给出 `xTxDmaSem` 后释放内存。

## 三、main.c 调用点变化（已随 de89244 入库，本次未改 main.c）

| 位置 | 迁移前 | 迁移后 |
| --- | --- | --- |
| 头文件包含 | `#include "app_usart1_dma.h"` | `#include "app_usart2_dma.h"` |
| 初始化调用 | `App_USART1_Init();` | `App_USART2_Init();` |
| 爆栈钩子 | 无打印 | `App_USART2_Printf("*** STACK OVERFLOW in task: %s ***\n", pcTaskName);` |
| DMA 句柄声明 | 无 | 新增 `DMA_HandleTypeDef hdma_usart3_rx;` |
| `MX_DMA_Init` | 仅使能 `DMA1_Channel7_IRQn`（优先级 5） | 增加 `DMA1_Channel3_IRQn`（优先级 6） |

## 四、中断与 MSP 改动

- `HAL_UART_MspInit(UART_HandleTypeDef* huart)` 的 USART3 分支：配置 `hdma_usart3_rx`
  - `DMA_PERIPH_TO_MEMORY`、`MemInc=ENABLE`、`Mode=NORMAL`、字节对齐、`DMA_PRIORITY_LOW`
  - `HAL_DMA_Init` 失败走 `Error_Handler()`；随后 `__HAL_LINKDMA(huart, hdmarx, hdma_usart3_rx)`
- `HAL_UART_MspDeInit` 的 USART3 分支：`HAL_DMA_DeInit(huart->hdmarx)`
- `stm32f1xx_it.c` 新增 `DMA1_Channel3_IRQHandler()`，函数体只调用 `HAL_DMA_IRQHandler(&hdma_usart3_rx)`
- NVIC：`DMA1_Channel3_IRQn` 优先级 6（数值 ≥ 5，满足 FreeRTOS `configMAX_SYSCALL_INTERRUPT_PRIORITY` 前提；该中断内不调用 FreeRTOS API）
- USART3 本身仍保留 `USART3_IRQn`（优先级 7），空闲事件由 UART 中断进入 `HAL_UARTEx_RxEventCallback`

## 五、ioc 变化

- `Dma.Request1=USART3_RX`，`Dma.RequestsNb=1 → 2`
- 新增 `Dma.USART3_RX.1.*`：`Instance=DMA1_Channel3`、`Direction=DMA_PERIPH_TO_MEMORY`、`MemInc=DMA_MINC_ENABLE`、`Mode=DMA_NORMAL`、`Priority=DMA_PRIORITY_LOW`
- 新增 `NVIC.DMA1_Channel3_IRQn=true:6:0:true:false:true:false:true:true`

注意：重新用 CubeMX 生成时，若要保留 `App_USART2_Init()` 等用户代码，需确认 `main.c` 的 `USER CODE` 区未被覆盖。

## 六、验证方式与结果

### 1. 工作区 Release 构建（提交前）

```
cmake --preset Release
cmake --build --preset Release --clean-first
```

结果：50/50 全部通过，0 error；仅链接器既有告警 `LOAD segment with RWX permissions`（与本次改动无关）。

```
RAM:   13264 B / 20 KB  64.77%
FLASH: 53824 B / 64 KB  82.13%
```

### 2. 自洽性验收：提交后干净导出构建

导出目录（工作区外）：`%TEMP%\cartest_check`

```
git archive HEAD -o %TEMP%\cartest_check.zip
tar -xf %TEMP%\cartest_check.zip -C %TEMP%\cartest_check
cmake --preset Release
cmake --build --preset Release
```

结果：

- `Core/Inc`、`Core/Src` 内只存在 `app_usart2_dma.h/.c`，无 `app_usart1*` / `app_usart2.c/.h` 残留
- 全工程搜索 `app_usart1|app_usart2.h` 无命中
- `stm32f1xx_it.c:195` 存在 `void DMA1_Channel3_IRQHandler(void)`
- 50/50 编译链接通过，0 error

```
RAM:   13192 B / 20 KB  64.41%
FLASH: 52836 B / 64 KB  80.62%
```

导出构建比工作区构建小（FLASH 少 988 B、RAM 少 72 B），原因是工作区里尚未提交的 `app_mpu6050.c`、`app_pwm.c`、`app_motor.c`、`delay.c`、`stm32f1xx_it.c`（HardFault 探针）改动不在导出树中；两份产物均为 0 error，差异符合预期。

## 七、未纳入本次提交的文件与理由

| 文件 | 判断 | 理由 |
| --- | --- | --- |
| `Core/Src/app_mpu6050.c` | 暂不提交 | 改动是「姿态任务写完邮箱后 `xTaskNotifyGive(ctrl)`」的事件驱动修复，属于控制环另一条线；与 UART 迁移无关（详见风险 1） |
| `Core/Src/app_pwm.c` | 暂不提交 | 占空比从 0~100 归一化为 0~1 的标度改动，属于电机/控制线（详见风险 2） |
| `Core/Src/app_motor.c` | 暂不提交 | 仅空白行与 `main.h`/`stm32f1xx_hal_i2c.h` 包含，无功能变化 |
| `Core/Src/delay.c` | 暂不提交 | `DWT CYCCNT` 回绕改用减法比较，属于时基线 |
| `Core/Src/stm32f1xx_it.c`（HardFault 探针部分） | 部分提交 | 仅暂存 `hdma_usart3_rx` 外部声明与 `DMA1_Channel3_IRQHandler`；异常帧探针（`g_FaultFrame_MSP/PSP`、`__get_MSP/__get_PSP`）属于调试取证线，仍留在工作区 |
| `CARTESTFreertos.code-workspace`、`.trae/`、`面试评分总结.md` | 不提交 | 按约束不动 |

## 八、剩余风险

1. **控制环在干净检出中不会运行（高）**：`de89244` 已把 `app_control.c` 改成 `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` 等待通知，`main.c` 也已把控制任务句柄传给姿态任务，但真正发通知的 `app_mpu6050.c` 改动仍未提交。干净检出可编译，但姿态任务不会 `vTaskNotifyGive`，控制任务永久阻塞，整车只有电机环（TIM2 通知）在跑。建议下一个批次立即补提 `app_mpu6050.c`。
2. **PWM 标度不一致（高）**：`app_motor.c` 输出的 `duty = vol/batVol` 约为 0~1，而已提交的 `app_pwm.c` 仍按 0~100 解释并除以 100，干净检出下实际占空比被压到约 1%，车基本不动。工作区未提交的 `app_pwm.c` 才是对应版本。
3. **调试探针未入库（中）**：`stm32f1xx_it.c` 的 HardFault 异常帧探针仍在工作区，历史提交里不含该能力，后续 HardFault 取证需先明确该改动是否入库。
4. **USART3 满帧接收**：DMA 长度固定 `MAX_leng=64`，正好 64 字节且无 `\n` 时依赖 IDLE 事件收尾；目前 `HAL_UARTEx_RxEventCallback` 中 `Size >= MAX_leng` 时截到 63 字节并重新武装，行为已明确，但超长行会被截断（与原逐字节版本「超长归零」策略不同）。
5. **`App_USART2_Printf` 的 `portMAX_DELAY` 入队**：队列满时调用任务会阻塞，若在中断上下文调用会违规。已给出的调用点（爆栈钩子、内存监控任务、`mputest`）均在任务上下文且爆栈钩子随后关中断死循环，风险可控。
