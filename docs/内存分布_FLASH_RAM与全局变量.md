# FLASH / RAM 空间分布与全局变量存放规则

> 对象：STM32F103C8T6（FLASH 64 KB / RAM 20 KB）
> 依据：`build/Debug/CARTESTFreertos.map`、`arm-none-eabi-readelf -l/-S`、链接脚本 `stm32f103c8tx_flash.ld`
> 数据日期：2026-11-17（Debug `-Og` 构型，`main` @ `e74a48f` + 交付批次）

## 1. 编译输出的含义

```

RAM:   14,512 B / 20 KB = 70.9%
FLASH: 49,712 B / 64 KB = 75.9%

```

这两个百分比表示“链接器已经分配出去的地址空间占比”，而不是运行时真实用掉的量。最典型的例子：RAM 里的 9.7 KB 是 FreeRTOS 堆，上电就整块划走，用不用都算在 70.9% 里。

## 2. 全局变量、常量、局部变量放在哪

| 代码写法 | 所在段 | 占 FLASH | 占 RAM | 说明 |
|---|---|---|---|---|
| 函数内局部变量 | 栈（MSP 或任务栈） | 否 | 是（运行时） | 函数返回即失效 |
| 全局 / static，未初始化或初始化为 0 | `.bss` | 否 | 是 | 上电由启动代码清零 |
| 全局 / static，初始化为非 0 | `.data` | 是（初值副本） | 是 | 上电从 FLASH 复制到 RAM |
| `const` 变量、字符串字面量、查表数组 | `.rodata` | 是 | 否 | 只读，映射在 FLASH |
| 函数体（机器码） | `.text` | 是 | 否 | — |
| 中断向量表 | `.isr_vector` | 是 | 否 | 固定在 0x0800_0000 |
| `malloc` / `pvPortMalloc` / `xTaskCreate` | 堆 | 否 | 是 | FreeRTOS 从 `ucHeap` 分配 |

启动流程：`Reset_Handler` 先把 `.data` 的初值从 FLASH 复制到 RAM，再把 `.bss` 整块清零，最后跳 `main`。所以“初值在 FLASH、变量在 RAM”说的是同一份数据的两份拷贝。

### 对照本项目的代码

以 `Core/Src/app_control.c` 为例：

| 变量 | 段 |
|---|---|
| `static volatile uint32_t last_calc_tick = 0;` | `.bss`（初值为 0） |
| `static float wheel_omega_ref_radps = 0.0f;` | `.bss` |
| `static Attitude_t last_good_att;` | `.bss` |
| `static PID_TypeDef pid_speed;` 等 4 个 PID | `.bss` |
| `static const float GRAVITY_MPS2 = 9.8f;` | `.rodata`（只占 FLASH） |
| `App_Control_Init()` 里的 `PID_InitTypeDef pid_cfg;` | 栈，函数返回即消失 |
| `#define WHEEL_OMEGA_REF_MAX_RADPS` | 宏，不占任何空间 |

## 3. 本项目当前的实际分布

### FLASH 49,712 B（75.9%）

| 内容 | 大小 | 占比 |
|---|---|---|
| 中断向量表 `.isr_vector` | 268 B | 0.4% |
| 代码 `.text` | 40,180 B | 61.3% |
| 常量 `.rodata` | 9,144 B | 14.0% |
| 启动表 `.ARM/.init_array/.fini_array` + `.data` 初值副本 | 120 B | 0.2% |
| 未使用 | 15,824 B | 24.1% |

`.rodata` 里 `sin_vals` 和 `tan_vals` 各 1024 个 float，合计 8,192 B，占了常量的 89.6%（`Core/Src/qmath.c`）。如果以后不再使用 qmath 的查表三角函数，这是 FLASH 里最大的一块可回收空间。

### RAM 14,512 B（70.9%）

| 内容 | 大小 | 占 RAM |
|---|---|---|
| `.data` 已初始化全局变量 | 104 B | 0.5% |
| **`.bss` 合计（`size` 口径）** | **14,408 B** | **70.4%** |
| 　└ FreeRTOS 堆 `ucHeap` | 9,728 B | 47.5% |
| 　└ `.bss` 其余（含空闲任务栈 512 B、链接脚本预留 1,540 B、其他全局/静态约 2,628 B） | 4,680 B | 22.8% |
| 未分配（主栈 MSP 可用） | 5,968 B | 29.1% |

> `.bss` 的两行是内部拆解，不是并列项：9,728 + 4,680 = 14,408 B，合计只算一次。`arm-none-eabi-size` 的 `.bss` 已包含链接脚本里的 `._user_heap_stack`（`_Min_Heap_Size 0x200` + `_Min_Stack_Size 0x400` = 1,540 B）。

- `ucHeap` 定义在 `heap_4.c` 中：`static uint8_t ucHeap[configTOTAL_HEAP_SIZE]`，未初始化，因此落在 `.bss`；大小就是 `configTOTAL_HEAP_SIZE`（`freeRTOS/Inc/FreeRTOSConfig.h`），所有任务栈、队列、信号量、互斥量都从这里分配。
- 空闲任务的静态栈 `uxIdleTaskStack` 为 512 B，直接算在 `.bss` 里（`configKERNEL_PROVIDED_STATIC_MEMORY=1`）。
- `_estack = 0x2000_5000`，已分配区顶到 `0x2000_38B0`，到 `_estack` 之间余 **5,968 B**，这块就是主栈真正可用的余量。
- 内存区域定义见 `stm32f103c8tx_flash.ld`：

```ld
RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
```

## 4. 关于 RWX 警告（已修复）

旧版链接脚本的 `.init_array` / `.fini_array` 没有标注只读，导致第一个 LOAD 段被合并成 `RWE`，链接时出现：

```
CARTESTFreertos.elf has a LOAD segment with RWX permissions
```

2026-11-17 交付批次已给 `stm32f103c8tx_flash.ld` 的 `.ARM.extab`、`.ARM`、`.preinit_array`、`.init_array`、`.fini_array` 加上 `(READONLY)`（GCC 11+ 支持，当前 15.3 满足），该 warning 已消失。

复验命令与预期：

```powershell
arm-none-eabi-readelf -lW build\Debug\CARTESTFreertos.elf
```

第一个 LOAD 段权限应为 `R E`（不再是 `RWE`）。若在 GCC 10 及更早版本上构建，删除 `(READONLY)` 或改用 `-Wl,--no-warn-rwx-segments`。

## 5. 想继续压占用时的做法

- **RAM**：先在代码里调用 `xPortGetFreeHeapSize()` 看堆的真实余量，再按需下调 `configTOTAL_HEAP_SIZE`。每降 1 KB，RAM 占比约降 5 个百分点。
- **FLASH**：优先处理 `sin_vals` / `tan_vals` 这 8 KB 查表；或确认 `-Og` 已是当前 Debug 配置（见 `CMakeLists.txt`，`-O0` 会超出 64 KB）。
- 修改后重新编译，用 `arm-none-eabi-size build/Debug/CARTESTFreertos.elf` 或构建日志里的 Memory region 表复核。

## 6. 附：查看命令

```powershell
# 各段大小
arm-none-eabi-size build\Debug\CARTESTFreertos.elf

# 程序头（看 RWX）
arm-none-eabi-readelf -lW build\Debug\CARTESTFreertos.elf

# 各段标志
arm-none-eabi-readelf -SW build\Debug\CARTESTFreertos.elf

# 占用最大的符号
arm-none-eabi-nm --print-size --size-sort --radix=d build\Debug\CARTESTFreertos.elf
```
