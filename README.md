# CARTESTFreertos

STM32F103C8T6 双轮平衡车固件，手动集成 FreeRTOS（非 CubeMX 中间件）。
控制链路：MPU6050 姿态（I2C + 互补滤波）→ 串级 PID（速度/倾角/角速度/转向）→ TB6612 PWM；编码器测速、蓝牙遥控、电池电压指示、IWDG 看门狗、任务栈水位监控。

## 硬件与接线

| 功能 | 引脚 | 说明 |
|---|---|---|
| 左电机 PWM | PA8（TIM1_CH1） | 10 kHz，占空比 0–100% |
| 左电机方向 | PA9（AIN1）/ PA10（AIN2） | 正反转 |
| 右电机 PWM | PB6（TIM4_CH1） | 10 kHz |
| 右电机方向 | PB5（BIN1）/ PB7（BIN2） | 正反转 |
| TB6612 STBY | PA1 | 高电平使能 |
| 左编码器 | PB14（A 相，EXTI）/ PB15（B 相） | 22 线霍尔 |
| 右编码器 | PB3（A 相，EXTI）/ PB4（B 相） | 22 线霍尔 |
| MPU6050 | PB8（SCL）/ PB9（SDA） | I2C1，400 kHz |
| 调试/产测串口 | PA2（TX）/ PA3（RX） | USART2，**921600 8N1** |
| 蓝牙遥控 | PB10（TX）/ PB11（RX） | USART3，9600 8N1 |
| 电池采样 | PB0（ADC1_IN8） | TIM3 TRGO 触发，100 Hz |
| 状态灯 | PA4 / PA5 / PA6 | 电量指示 |
| 调试接口 | PA13（SWDIO）/ PA14（SWCLK） | ST-Link |

## 5 分钟跑通

### 1. 依赖

- CMake ≥ 3.22（实测 4.4.0-rc3）
- Ninja
- `arm-none-eabi-gcc` 15.3.1（`arm-none-eabi-` 必须在 PATH）
- 烧录：ST-Link + OpenOCD 0.12（或 VS Code EIDE 插件）

### 2. 配置与构建

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

产物：`build/Debug/CARTESTFreertos.elf`。构建结束会打印 FLASH/RAM 占用（`size` 目标）。

### 3. 烧录

```powershell
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg `
  -c "program build/Debug/CARTESTFreertos.elf verify reset exit"
```

或在 VS Code 里用 EIDE 的 `build and flash`。

### 4. 看串口

USB-TTL 接 PA2/PA3，串口工具设为 **921600 8N1**。正常上电顺序：

```
fw <git describe> | build <月 日 年> <时:分:秒> | reset <POWER-ON|IWDG|SOFTWARE|PIN|...>
B1            ← 已跑到 main 初始化中段，USART2 硬件通路正常
1 2 3 4 5 6 7 8 9 A   ← 各模块初始化路标（缺哪个就卡在哪一步之前）
B2            ← 所有初始化完成，即将启动调度器
Memory Monitor ← 每 2 s 打印一次堆用量与各任务栈高水位
```

首行是 `fw ver / build date / reset cause`：版本号在配置阶段由 `git describe` 注入，复位原因可区分上电（`POWER-ON`）、独立看门狗（`IWDG`）、软复位（`SOFTWARE`）等，打印后自动清标志。

若只看到 `B1` 后反复重启，先查 IWDG（约 410 ms）与初始化阻塞；若 `B2` 都不出现，看 `SCHED-FAIL`（FreeRTOS 堆不足）。

### 5. 遥控协议

蓝牙串口（9600）发送一行文本：

```
move <turn> <speed>
```

`turn` / `speed` 范围 ±100，换算关系见 [app_rc.c](Core/Src/app_rc.c)。

## 构建构型与体积

| 构型 | 命令 | FLASH | RAM |
|---|---|---|---|
| Debug | `cmake --build --preset Debug` | 49,712 B / 64 KB（75.9%） | 14,512 B / 20 KB（70.9%） |
| MinSizeRel | `cmake --build --preset MinSizeRel` | **44,840 B（68.4%）** | 14,512 B（70.9%） |
| Release | `cmake --build --preset Release` | 52,956 B（80.8%） | 14,512 B（70.9%） |

> **发布用 MinSizeRel。** Release 预设的 `-O3` 追加在工具链 `-Os` 之后，实际生效 `-O3`，体积反而更大。
> 数据为 2026-11-17 干净检出实测（`arm-none-eabi-size` 的 `text+data` / `data+bss` 口径），复现命令见 [docs/构建基线.md](docs/构建基线.md)。

## 软件结构

| 任务 | 栈（字） | 优先级 | 触发 |
|---|---|---|---|
| motor | 128 | 5 | TIM2 1 ms 中断通知 |
| ctrl | 384 | 4 | 姿态任务通知（≈5 ms） |
| att | 256 | 3 | `vTaskDelayUntil` 5 ms |
| rc | 512 | 2 | USART3 DMA 空闲中断 → 二值信号量 |
| UART2_TX | 128 | 2 | 发送队列 + DMA 完成信号量 |
| MemMon | 256 | 1 | 100 ms 喂狗 / 2000 ms 打印 |
| idle | 128（静态） | 0 | — |

IPC 与中断优先级详见 [docs/任务表.md](docs/任务表.md)。

## 目录

```
Core/Inc, Core/Src   应用与 CubeMX 生成代码（app_*.c 为业务模块）
freeRTOS/            FreeRTOS 内核与 FreeRTOSConfig.h
Drivers/             STM32F1 HAL 与 CMSIS
cmake/               工具链文件与 CubeMX 生成构建脚本
docs/                设计、调试、变更记录与构建基线
2026-11-16/          T1 证据计划（执行计划与缺口清单）
```

## 交付验收状态（P1：环境与交付闭环）

> 对应执行文档 `09_单一主线_三个月执行路径.md` §4 P1 与 §6 速查表。

| # | 验收项 | 状态 | 证据 |
|---|---|---|---|
| 1 | 空目录干净检出，`cmake --preset Debug && cmake --build` 0 error 0 warning | **通过** | 2026-11-17 复制到空目录（排除 `build/`、`.git`）后实测；工作区 Debug/MinSizeRel/Release 三构型均 0 warning |
| 2 | 陌生人按 README 5 分钟编译 + 烧录 + 看到版本行 | 待陌生人实测 | 自测流程已跑通；烧录步骤见第 3 节 |
| 3 | 上电到首行 < 3 s，复位原因可区分上电/看门狗/软复位 | **代码已实现，待上板** | 首行 `fw ... reset ...`；`prvResetCause()` 见 [main.c](Core/Src/main.c) |
| 4 | 本批提交只含本阶段文件，`git status` 干净 | **已提交（2026-11-17）** | 本批文件清单与改前/改后见 [交付基线变更记录](docs/变更记录_交付基线_2026-11-17.md) |
| 5 | 一页硬件实测定位记录（波形 → 假设 → 改动 → 复测） | **待实测** | 模板与测点见 [docs/调试记录_示波器实测.md](docs/调试记录_示波器实测.md) |

> 第 1 条证明“能编”，第 5 条证明“能交付”。两条都成立，P1 才算完成。

## 已知限制（当前版本）

- **OTA / Bootloader 尚未实现**：T1 计划为单槽升级（Boot 8 KB + App 54 KB + 参数 2×1 KB），无 A/B 回滚；详见 [T1 执行计划](2026-11-16/t1-evidence-plan/outputs/01-T1执行计划.md)。
- **参数写 Flash 会阻塞 CPU**：F103 页擦除约 20–40 ms，会打断 1 ms 电机环，只能在停机/启动阶段写。
- **IWDG 约 410 ms**：任何长时间阻塞（断点、升级页擦除）都必须喂狗或用调试器冻结。
- **Debug 构型体积较大**：调试用 `-Og`；发布前必须看 MinSizeRel 尺寸。
- **`qmath.c` 查表占 8 KB FLASH**：`sin_vals` / `tan_vals` 是当前最大的一块可回收常量。
- **CubeMX 重新生成有冲突风险**：会覆盖 `cmake/stm32cubemx/CMakeLists.txt`，并在 `stm32f1xx_it.c` 生成 `SVC_Handler`/`PendSV_Handler` 空实现，需人工删除。

## 文档索引

- [环境与工具链体检（env）](docs/env.md)
- [任务表（栈/优先级/IPC/中断）](docs/任务表.md)
- [构建基线（三构型尺寸与复现命令）](docs/构建基线.md)
- [FLASH/RAM 分布](docs/内存分布_FLASH_RAM与全局变量.md)
- [FreeRTOSConfig 配置解析](docs/FreeRTOSConfig_配置解析.md)
- [调试记录：HAL tick 停摆](docs/调试记录_HAL_Tick停摆.md)
- [调试记录：蓝牙 USART3 接收与爆栈复位](docs/调试记录_蓝牙USART3接收与爆栈复位.md)
- [调试记录：示波器实测（P1 模板，待实测）](docs/调试记录_示波器实测.md)
- [变更记录：UART DMA 迁移](docs/变更记录_UART_DMA迁移.md)
- [变更记录：USART3 错误回调](docs/变更记录_USART3错误回调.md)
- [变更记录：交付基线（2026-11-17）](docs/变更记录_交付基线_2026-11-17.md)
- [T1 执行计划](2026-11-16/t1-evidence-plan/outputs/01-T1执行计划.md) ｜ [T1 缺口清单](2026-11-16/t1-evidence-plan/outputs/02-仓库缺口清单.md)
