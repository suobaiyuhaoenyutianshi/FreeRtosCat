# 构建环境与工具链体检（env）

> 目的：让任意一台新机器按本文能在 5 分钟内判断“环境是否齐全”，缺哪一项就补哪一项。
> 记录日期：2026-11-17；代码基线：`main`（`git describe` 注入到固件启动行）。

## 1. 本机实测版本

| 项 | 实测值 | 要求 |
|---|---|---|
| OS | Windows 10/11（PowerShell 5.1） | — |
| 交叉编译器 | `arm-none-eabi-gcc` 15.3.1（Arm GNU Toolchain 15.3.Rel1） | ≥ 12，且在 `PATH` |
| CMake | 4.4.0-rc3 | ≥ 3.22（`CMakePresets.json` 使用 preset v3） |
| Ninja | 1.13.2 | 任意近版本 |
| OpenOCD | 0.12.0 | ≥ 0.11，带 `interface/stlink.cfg`、`target/stm32f1x.cfg` |
| 目标芯片 | STM32F103C8T6，FLASH 64 KB / RAM 20 KB | — |
| 调试器 | ST-Link（SWD：PA13/PA14） | — |

## 2. 一键体检（逐条打勾）

在仓库根目录执行：

```powershell
arm-none-eabi-gcc --version
cmake --version
ninja --version
openocd --version

# 编译预设 + 构建（Debug）
cmake --preset Debug
cmake --build --preset Debug

# 查看产物与体积
arm-none-eabi-size build/Debug/CARTESTFreertos.elf
```

| # | 检查项 | 通过标准 | 2026-11-17 状态 |
|---|---|---|---|
| 1 | `arm-none-eabi-gcc --version` | 有版本输出 | 通过（15.3.1） |
| 2 | `cmake --version` / `ninja --version` | 有版本输出 | 通过（4.4.0-rc3 / 1.13.2） |
| 3 | `openocd --version` | 有版本输出 | 通过（0.12.0） |
| 4 | `cmake --preset Debug && cmake --build --preset Debug` | 生成 `.elf`，0 error 0 warning | 通过 |
| 5 | `openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "init; shutdown"` | 识别到芯片 IDCODE | **待上板**（本机执行报 `Error: open failed`，未接板或 ST-Link 驱动未就绪） |
| 6 | 烧录后串口读到 `fw ...` 首行 | 921600 8N1 可见 | **待上板** |

> 第 4 项通过只说明“能编”；第 5、6 项必须接板才能成立，属于 P1 的“能交付”验收，缺一项都不能算完成。

## 3. 构建 / 烧录命令

```powershell
# 配置（三构型任选；发布用 MinSizeRel）
cmake --preset Debug
cmake --preset MinSizeRel

# 构建
cmake --build --preset Debug
cmake --build --preset MinSizeRel

# 烧录（OpenOCD）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg `
  -c "program build/Debug/CARTESTFreertos.elf verify reset exit"
```

- 唯一生效的链接脚本：根目录 `stm32f103c8tx_flash.ld`（由 `cmake/gcc-arm-none-eabi.cmake` 指定）。
- 根目录 `STM32F103xx_FLASH.ld` 只在 `cmake/starm-clang.cmake`（ARMClang）路径下使用，不参与默认构建。
- 版本号在配置阶段由 `git describe --tags --always --dirty` 注入 `FW_VERSION`；源码导出（无 `.git`）时回退为 `dev`。

## 4. 输出通道（三条路，按优先级）

### ① 串口（首选）

- 硬件：USB-TTL（CH340/CP2102）接 PA2(TX)/PA3(RX)，共地。
- 参数：**921600 8N1**，无流控。
- 上电首行固定格式：

```
fw <版本> | build <月 日 年> <时:分:秒> | reset <POWER-ON|IWDG|SOFTWARE|PIN|...>
```

随后是 `B1`、初始化路标 `1..9 A`、`B2`、`Memory Monitor` 周期表。

### ② GDB / OpenOCD 读 RAM（无串口时兜底）

只需 ST-Link，不需要 COM 口。固件把诊断现场集中在 `g_diag`（`main.c` 定义，`main.h` 声明）：

```gdb
# 连接（OpenOCD 已在另一个终端运行）
arm-none-eabi-gdb build/Debug/CARTESTFreertos.elf
(gdb) target extended-remote localhost:3333
(gdb) p g_diag
(gdb) p/x g_diag.tim2_irq_count
(gdb) p/x g_diag.i2c_last_ret
```

- `g_diag.tim2_irq_count` 持续增长 = TIM2 中断与 HAL 时基活着；
- `g_diag.i2c_last_ret` = 最近一次 MPU6050 I2C 返回值（0 = HAL_OK）；
- `g_diag` 还包含 PRIMASK/BASEPRI、TIM2 计数与 NVIC 使能位，用于判断“中断是否被关掉”。

### ③ SWO/ITM（可选）

ST-Link 的 SWO 脚接到板上后，可用 OpenOCD `itm` 或 ST-Link Utility 看实时打印。本工程默认不启用 ITM，串口已能覆盖调试需求。

## 5. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `cmake --preset` 报找不到编译器 | `arm-none-eabi-` 不在 `PATH` | 把工具链 `bin` 加入 `PATH` 后重开终端 |
| 构建报 `ninja: command not found` | Ninja 未安装/不在 `PATH` | 安装 Ninja 或改用 `-G "Unix Makefiles"`（需同步改 preset） |
| 烧录报 `Error: open failed` | 没接板、ST-Link 驱动异常、SWD 被占用 | 检查接线与驱动；关闭其他调试会话后重试 |
| 上电只看到 `B1` 后反复重启 | 初始化阻塞超过 IWDG（约 410 ms） | 看 `B2`/`SCHED-FAIL`；按 `docs/调试记录_HAL_Tick停摆.md` 排查 |
| 重新生成 CubeMX 后链接报重复定义 | 生成器重新生成 `SVC_Handler`/`PendSV_Handler` 空实现 | 删除生成的两个空实现；检查 `cmake/stm32cubemx/CMakeLists.txt` 是否被覆盖 |

## 6. 更新记录

| 日期 | 说明 |
|---|---|
| 2026-11-17 | 首次建立；记录工具链实测版本、6 项体检状态、三条输出通道与烧录命令 |
