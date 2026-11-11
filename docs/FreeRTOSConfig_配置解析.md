# FreeRTOSConfig.h 配置解析

## 工程背景

| 项目 | 值 |
|---|---|
| FreeRTOS 版本 | V11.1.0（官方模板，注释完整保留） |
| 芯片 | STM32F103C8T6（20KB RAM / 64KB Flash） |
| 主频 `configCPU_CLOCK_HZ` | 72 MHz |
| tick 频率 `configTICK_RATE_HZ` | 1000 Hz（1 tick = 1ms） |
| 配置文件 | `freeRTOS/Inc/FreeRTOSConfig.h` |

> 说明：该文件基于官方模板，因此"哪些被定制"只能从**非默认值**判断。下表列出几乎可以确定是手工定制过的项。

---

## 一、明显被定制的项

| 行 | 配置 | 当前值 | 模板默认 | 作用与对本工程的影响 |
|---|---|---|---|---|
| L292 | `configTOTAL_HEAP_SIZE` | **7680** | 4096 | 内核堆（heap_4，静态数组在 .bss）。中文注释按 6 个任务 + 2 队列 + 2 信号量核算到 7136B，**只剩约 536B 余量** |
| L187 | `configHEAP_CLEAR_MEMORY_ON_FREE` | **1** | 0 | `vPortFree` 时把内存清零。调试期能发现 use-after-free 读到脏数据，代价是每次释放多一次 memset |
| L270 | `configSUPPORT_STATIC_ALLOCATION` | **1** | 0 | 允许静态创建任务/队列，也是 `sizeof(StaticTask_t)` 可用的前提（TCB 大小就靠它） |
| L567 | `configKERNEL_PROVIDED_STATIC_MEMORY` | **1** | 未定义 | 内核**自己**给 Idle / Timer 任务提供静态内存。好处：这两个任务不占那 7680B 堆，且不用写 `vApplicationGetIdleTaskMemory()` |
| L362 | `configCHECK_FOR_STACK_OVERFLOW` | **2** | 0 | 每次任务切换检查栈末端的 `0xA5A5A5A5` 标记是否被踩。最费时但最有效，配合 `memory_monitor.c` 的栈水位统计是双保险 |
| L218 | `configUSE_TIMERS` | **1** | 0 | 编入软件定时器，会额外产生 Timer 任务 + 定时器队列 |
| L157 | `configENABLE_BACKWARD_COMPATIBILITY` | **0** | 1 | 关掉旧版 API 别名（`vTaskDelayUntil` 的老名字等），省一点 Flash |
| L328 | `configMAX_SYSCALL_INTERRUPT_PRIORITY` | **5<<4** | 0 | **最关键的项**：只有优先级数值 ≥5 的中断才允许调用 `xxxFromISR()` |
| L612 | `INCLUDE_uxTaskGetStackHighWaterMark` | **1** | 0 | 就是 `memory_monitor.c` 用的那个函数 |
| L620-622 | handler 映射 | 3 条 define | 无 | 把 SVC/PendSV/SysTick 直接指向 FreeRTOS 的汇编入口，即 "Direct Routing" |

---

## 二、其余项按用途分组

以下都是模板/常用值，不是改动。

### 调度

- `configTICK_RATE_HZ 1000` → 1ms 一个 tick，`pdMS_TO_TICKS()` 的基准
- `configUSE_PREEMPTION 1` + `configUSE_TIME_SLICING 1` → 抢占式，同优先级任务轮转
- `configMAX_PRIORITIES 7` → 任务优先级 1~6 有空间
- `configMINIMAL_STACK_SIZE 128` → **128 字 = 512 字节**（F103 是一字 4 字节），Idle 任务栈
- `configIDLE_SHOULD_YIELD 1` → 有同优先级的应用任务时，Idle 主动让路
- `configTICK_TYPE_WIDTH_32_BITS` → tick 约 **49.7 天**回绕（32 位 × 1ms）

### 内存

- `configSUPPORT_DYNAMIC_ALLOCATION 1` → `xTaskCreate` / `xQueueCreate` 走堆
- `configAPPLICATION_ALLOCATED_HEAP 0` → 堆数组由链接器放进 .bss
- `configENABLE_HEAP_PROTECTOR 0` → 不做堆块指针保护（省 RAM/时间）

### 中断

- `configKERNEL_INTERRUPT_PRIORITY 15<<4` → SysTick/PendSV 用最低优先级（15），F1 是 4 位优先级所以左移 4 位
- `configCHECK_HANDLER_INSTALLATION 1` → 启动时校验那 3 个 handler 真的装上了

### 裁剪（为省 Flash/RAM 关掉的）

- `configUSE_TRACE_FACILITY 0`、`configGENERATE_RUN_TIME_STATS 0`、`configUSE_STATS_FORMATTING_FUNCTIONS 0` → 不做 CPU 占用统计、不编 `vTaskList`
- `configQUEUE_REGISTRY_SIZE 0` → 调试器看不到队列名（内核感知调试用）
- `configUSE_QUEUE_SETS 0`、`configUSE_CO_ROUTINES 0`、`configNUM_THREAD_LOCAL_STORAGE_POINTERS 0`、`configUSE_APPLICATION_TASK_TAG 0`
- `configUSE_IDLE_HOOK` / `TICK_HOOK` / `MALLOC_FAILED_HOOK` 全 0 → 对应钩子函数都不用实现

### 保留的功能

- `configUSE_TASK_NOTIFICATIONS 1` → 项目大量用 `xTaskNotifyGive` 驱动控制环
- `configUSE_MUTEXES / RECURSIVE_MUTEXES / COUNTING_SEMAPHORES 1`
- `configUSE_EVENT_GROUPS 1`、`configUSE_STREAM_BUFFERS 1`（编进 Flash，但不实例化就不占 RAM）

### 调试

- `configASSERT` 实现成"关中断 + 死循环"，出错时停在现场，方便接调试器

---

## 三、三个值得注意的地方

### 1. `configUSE_TIMERS 1` 可能是多余的

项目里没有任何 `xTimerCreate` / `xTimerStart` 调用。开了它，内核会常驻一个 Timer 任务（128 字栈 = 512B，虽然走静态内存不占堆，但占 .bss）和一个队列。F103C8T6 只有 20KB RAM，如果确认不用，改成 `0` 可以省下这些。

### 2. `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5` 是个硬约束

`main.c:695` 的 TIM 回调里调用了 `vTaskNotifyGiveFromISR(mot, &xhw)`。这类 `FromISR` API 要求**中断优先级数值 ≥ 5**（数值越大优先级越低）。如果 TIM2 / UART 的 NVIC 优先级在 CubeMX 里被设成 0~4，`configASSERT` 会让你直接卡死在死循环里。这条是配置里最容易踩的坑，建议核对一下所有调 `FromISR` 的中断。

### 3. 堆余量只有 ~536B

注释算得很清楚，但这也意味着再增加一个任务或队列就可能 `pvPortMalloc` 失败（`xTaskCreate` 返回 `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`），而 `configUSE_MALLOC_FAILED_HOOK = 0` 意味着**失败时不会有任何提示**，程序看着能跑但任务没起来。建议要么留出更多余量，要么把 `MALLOC_FAILED_HOOK` 打开并在钩子里打印。

---

## 四、命名规律速查

| 前缀/词根 | 字面含义 | 用来说明什么 |
|---|---|---|
| `config` | 配置常量 | 一切编译期开关的总前缀 |
| `configUSE_` | 是否**使用**某功能 | 决定功能代码是否被编进固件（0 = 整段代码消失） |
| `configSUPPORT_` | 是否**支持**某种创建方式 | 决定某个 API 族是否存在 |
| `INCLUDE_` | 是否**包含**某函数 | 针对单个 API 函数做裁剪 |
| `configMAX_` | 最大上限 | 数组长度/编号范围 |
| `configCHECK_` | 检查 | 调试期校验 |
| `configKERNEL_` | 内核自身 | 内核（而非应用）使用的配置 |

---

## 五、关键换算公式

```
每个 tick 的 SysTick 重装值 = configCPU_CLOCK_HZ / configTICK_RATE_HZ
                            = 72,000,000 / 1000 = 72,000

tick 计数器回绕周期 = 2^32 / configTICK_RATE_HZ
                    = 4,294,967,296 / 1000 ≈ 4,294,967 s ≈ 49.7 天

DWT CYCCNT 回绕周期 = 2^32 / SystemCoreClock
                    = 4,294,967,296 / 72,000,000 ≈ 59.65 s

1 微秒对应的 CPU 周期数 = SystemCoreClock / 1,000,000 = 72
```
