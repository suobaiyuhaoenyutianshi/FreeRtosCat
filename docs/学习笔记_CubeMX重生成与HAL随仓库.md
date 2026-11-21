# 学习笔记：CubeMX 重新生成 与 HAL 随仓库

> 这份笔记回答两个问题：
> 1. 为什么这个工程 `git clone` 下来就能编，不用装 STM32CubeF1？
> 2. 用 CubeMX 加外设时，哪些东西会被覆盖、哪些不会？怎么安全地改？
>
> 对象：`CARTESTFreertos`（STM32F103C8T6 + FreeRTOS + CMake）。
> 对照：`FreertosGUILab` 那套 `third_party/stm32cube_f1` 写法。
> 写给“还没完全搞懂”的自己，尽量说人话。

---

## 0. 先记住三句话

1. **HAL/CMSIS 是源码，不是安装包。** 源码放进仓库，换机器 clone 就能编；不装 STM32CubeF1 包也能编。
2. **CubeMX 只负责“生成初始化代码”，不负责你的业务代码。** 它靠 `USER CODE` 标签判断哪些不能动。
3. **重新生成后必须看 `git diff`。** 链接脚本、中断文件、引脚配置这三处最容易出问题。

---

## 1. 基础概念：HAL、CMSIS、Drivers、third_party 到底是什么

| 名词 | 一句话解释 | 类比 |
|---|---|---|
| **HAL** | ST 写的硬件抽象层，把 GPIO/I2C/UART/CAN 等外设封装成函数 | 一本“外设说明书 + 现成工具” |
| **CMSIS** | ARM 给 Cortex-M 内核写的访问层（寄存器定义、启动文件、系统初始化） | 一本“内核语法书” |
| **Drivers/** | CubeMX 默认放 HAL/CMSIS 的目录 | 把书放进书包 |
| **third_party/stm32cube_f1/** | 另一个项目自定义的同类目录 | 把同样的书放进另一个柜子 |
| **CubeMX** | 图形化配置工具，根据 `.ioc` 生成初始化代码 | 装修队，按图纸施工 |
| **.ioc** | CubeMX 的工程配置（引脚、时钟、外设、生成选项） | 装修图纸 |

关键点：**HAL 和 CMSIS 都是普通 C 源码**。只要它们躺在仓库里，CMake 能找到头文件和 `.c`，就能编译。

### 1.1 “随仓库”与“依赖安装包”的差别

| 做法 | 换机器 clone 后 | 优点 | 缺点 |
|---|---|---|---|
| **HAL 随仓库**（本工程 `Drivers/`） | 直接能编 | 版本固定、可复现、面试官 clone 就能跑 | 仓库多几十个文件 |
| **依赖 CubeMX 安装包** | 还要装 STM32CubeF1，路径还可能对不上 | 仓库小 | 换机器容易断、版本会漂 |

本工程选的是第一种：`git ls-files Drivers` 能看到 80 个文件已入库。

### 1.2 CMake 在这里做了什么

CMake 不“安装” HAL，它只做两件事：

```cmake
# 1) 告诉编译器：头文件去哪找
set(MX_Include_Dirs
    ../../Core/Inc
    ../../Drivers/STM32F1xx_HAL_Driver/Inc
    ../../Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ../../Drivers/CMSIS/Include
)

# 2) 告诉链接器：哪些 .c 要编进去
set(STM32_Drivers_Src
    ../../Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c
    ../../Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c
    ...
)
```

对应文件：[cmake/stm32cubemx/CMakeLists.txt](../cmake/stm32cubemx/CMakeLists.txt)。

> 你贴的那段 `third_party/stm32cube_f1` 写法，本质完全一样，只是把 `Drivers` 换成了 `third_party/stm32cube_f1`。**目录名不影响原理。**

---

## 2. CubeMX 重新生成，到底会动什么？

### 2.1 一张图看懂

```
                 ┌─────────────────────────────┐
                 │  CARTESTFreertos.ioc         │
                 │  （引脚/时钟/外设/生成选项）   │
                 └──────────────┬──────────────┘
                                │ GENERATE CODE
                                ▼
        ┌──────────────────────────────────────────┐
        │ CubeMX 生成/覆盖的文件                    │
        │  main.c（只改 USER CODE 以外的部分）      │
        │  stm32f1xx_it.c / hal_msp.c / hal_conf.h │
        │  system_stm32f1xx.c / startup_*.s        │
        │  stm32f103c8tx_flash.ld                  │
        │  cmake/stm32cubemx/CMakeLists.txt        │
        │  cmake/gcc-arm-none-eabi.cmake           │
        └──────────────────────────────────────────┘
                                │
              ┌─────────────────┴─────────────────┐
              ▼                                   ▼
   USER CODE 区（保留）                  手写文件（它不认识）
   /* USER CODE BEGIN */                 app_*.c / freeRTOS/
   /* USER CODE END   */                 user_sources.cmake
```

### 2.2 会保留的

- `main.c` 里 `/* USER CODE BEGIN ... */` 与 `/* USER CODE END ... */` 之间的内容。
  本工程的 `prvPrintBootInfo()`、NVIC 优先级编排、任务创建都在这些区里。
- 不在 `.ioc` 里管理的中间件：本工程 FreeRTOS 是**手动集成**的，`freeRTOS/` 和 `FreeRTOSConfig.h` 不会被 CubeMX 动。

### 2.3 不会保留的

- **链接脚本**：`stm32f103c8tx_flash.ld` 会按 CubeMX 模板重写。
  我们手动加的 `(READONLY)` 会消失，RWX 警告会回来。
- **中断文件**：`stm32f1xx_it.c` 可能被补上 `SVC_Handler` / `PendSV_Handler` 空实现；而 FreeRTOS 的 port 里已经有同名函数，链接会报 `multiple definition`。
- **HAL 配置**：`stm32f1xx_hal_conf.h` 的模块开关会按 `.ioc` 重置。比如 CAN 现在是注释掉的，重新生成后可能被打开或保持关闭。
- **生成的 CMake 文件**：`cmake/stm32cubemx/CMakeLists.txt` 和外设源文件列表会按新配置重写。

> 本工程 `.ioc` 里 `ProjectManager.DeletePrevious=true`，意思是生成前可能删掉上次生成的文件。**所以改之前一定先提交或开分支。**

---

## 3. 本工程 vs FreertosGUILab：同一件事，两种目录

| 对比项 | CARTESTFreertos（本工程） | FreertosGUILab（你贴的那套） |
|---|---|---|
| HAL/CMSIS 位置 | `Drivers/`（CubeMX 默认） | `third_party/stm32cube_f1/`（自定义） |
| CMake 路径 | `../../Drivers/...` | `${HAL_ROOT}/...` |
| `HAL_ROOT` 绝对路径风险 | **没有**（路径是仓库相对路径） | **有**（CubeMX 可能写回 `C:/Users/...`） |
| 重新生成会不会多出一份 HAL | **不会**（本来就是 `Drivers`） | **可能**（`third_party` + `Drivers` 两份） |
| CubeMX 版本 | 6.18.1 | 6.18.1 |
| 工具链 | CMake | CMake |
| KeepUserCode | true | true |
| FreeRTOS 管理方式 | 手动集成，不在 `.ioc` | 同样不在 `.ioc` |
| 额外专属风险 | 链接脚本 `(READONLY)`、引脚已满 | `HAL_ROOT`、重复 `Drivers` |
| 当前定位 | 已交付（P1 基线） | 学习项目，不构成交付 |

**结论：**
- 本工程没有「HAL_ROOT 断链」和「多一份 Drivers」这两个问题。
- 但本工程有「链接脚本被覆盖」和「引脚冲突」这两个更实际的问题。

---

## 4. 本工程的三个专属坑（重点）

### 坑 1：链接脚本被重写，RWX 警告回来

- 文件：[stm32f103c8tx_flash.ld](../stm32f103c8tx_flash.ld)
- 我们做过的事：给 `.ARM`、`.preinit_array`、`.init_array`、`.fini_array` 加了 `(READONLY)`，消除：

```
warning: CARTESTFreertos.elf has a LOAD segment with RWX permissions
```

- CubeMX 重新生成后：这些 `(READONLY)` 很可能没了。
- 处理：生成后执行 `git diff stm32f103c8tx_flash.ld`，把 `(READONLY)` 加回去，再构建确认 warning 消失。

### 坑 2：`stm32f1xx_it.c` 多出空的 `SVC_Handler` / `PendSV_Handler`

- 文件：[Core/Src/stm32f1xx_it.c](../Core/Src/stm32f1xx_it.c)
- 现状：这两个函数由 FreeRTOS 的 port 提供，`stm32f1xx_it.c` 里没有。
- 重新生成后：CubeMX 可能补两个空函数，链接时报重复定义。
- 处理：删掉新生成的两个空实现，只保留 FreeRTOS 的版本。

### 坑 3：引脚已经满了，PB10/PB11、PB8/PB9 都不是“空闲”

这是本工程和 FreertosGUILab 最大的区别：**不能照抄“I2C2 用 PB10/PB11”或“CAN 用 PB8/PB9”的建议。**

| 引脚 | 本工程实际用途 | 想拿来做 | 结果 |
|---|---|---|---|
| PB10 / PB11 | **USART3 蓝牙遥控** | I2C2 | 冲突，蓝牙会坏 |
| PB8 / PB9 | **I2C1 接 MPU6050** | CAN 重映射 | 冲突，姿态读不到 |
| PA11 / PA12 | 空闲（USB 脚） | CAN | 可用，但需要收发器 |
| PB3 / PB4 / PB14 / PB15 | 编码器 | 其它外设 | 冲突 |
| PA8 / PA9 / PA10 / PB5 / PB6 / PB7 | 电机 PWM/方向 | 其它外设 | 冲突 |

另外：**F103C8 只有 1 路 CAN**，做不了双节点。所以 09 文档明确说：CAN 不要塞进平衡车，去 `electronicControl` 或新建最小工程做。

### 附带要检查的 4 处

| 文件 | 检查什么 |
|---|---|
| [Core/Inc/stm32f1xx_hal_conf.h](../Core/Inc/stm32f1xx_hal_conf.h) | 新外设的 `HAL_xxx_MODULE_ENABLED` 有没有打开 |
| [cmake/stm32cubemx/CMakeLists.txt](../cmake/stm32cubemx/CMakeLists.txt) | 新外设的 HAL `.c` 有没有进 `STM32_Drivers_Src` |
| [Core/Src/main.c](../Core/Src/main.c) | USER CODE 区是否保留；新中断的 NVIC 优先级要补进统一编排 |
| [CMakeLists.txt](../CMakeLists.txt) | 手加的 `include(user_sources.cmake)` 和 `size` 目标有没有被冲掉 |

---

## 5. 安全操作流程（照这个做）

### 第 0 步：先让工作区干净

```powershell
git status --short
```

有无关改动（比如 `CARTEST.code-workspace` 的 Trae workspaceId）先提交或 stash。

### 第 1 步：开分支 + 备份 .ioc

```powershell
git switch -c exp/cubemx-xxx
Copy-Item CARTESTFreertos.ioc CARTESTFreertos.ioc.bak
```

### 第 2 步：CubeMX 里只改一个外设

- 打开 `CARTESTFreertos.ioc`（6.18.1）。
- 只改一个外设，别一次改一堆；否则出问题不知道是哪一步。
- Project Manager 保持 `Toolchain/IDE = CMake`。
- 点 `GENERATE CODE`，**不要在 CubeMX 里点编译**。

### 第 3 步：生成后先看 diff，不要急着构建

```powershell
git status --short
git diff -- cmake/stm32cubemx/CMakeLists.txt cmake/gcc-arm-none-eabi.cmake `
             stm32f103c8tx_flash.ld Core/Src/stm32f1xx_it.c `
             Core/Inc/stm32f1xx_hal_conf.h CMakeLists.txt
```

按第 4 节的清单逐项修。

### 第 4 步：构建 + 上板

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

要求：**0 error 0 warning**。上板先看首行 `fw ... reset ...`，再验证新外设。

### 第 5 步：通过就提交，不通过就丢弃

```powershell
# 通过
git add -A
git commit -m "feat(xxx): 用 CubeMX 增加 xxx 外设"

# 不通过
git checkout -- .
git clean -fd
git switch -
```

---

## 6. 检查清单（可直接复制）

- [ ] 改之前 `git status` 干净，或已开分支
- [ ] `.ioc` 已备份
- [ ] CubeMX 只改了一个外设
- [ ] `cmake/stm32cubemx/CMakeLists.txt` 路径仍是 `../../Drivers`，没有绝对路径
- [ ] 没有多出一个重复的 `Drivers/`（本工程本来就有，别被换成两份）
- [ ] `stm32f103c8tx_flash.ld` 的 `(READONLY)` 还在
- [ ] `stm32f1xx_it.c` 没有多出 `SVC_Handler` / `PendSV_Handler`
- [ ] `stm32f1xx_hal_conf.h` 新模块宏已打开
- [ ] `main.c` 的 USER CODE 区完整，NVIC 优先级已补
- [ ] 根 `CMakeLists.txt` 的 `include(user_sources.cmake)` 和 `size` 目标还在
- [ ] `cmake --build --preset Debug` 0 error 0 warning
- [ ] 上电首行正常，新外设实测通过

---

## 7. 这和“交付”有什么关系

P1 的验收不是“能编译”，而是：

1. 换一台机器 `git clone` 后能编；
2. 陌生人按 README 5 分钟能跑；
3. 启动首行能看出固件版本和复位原因；
4. 仓库干净、一批一提交；
5. 有一页硬件实测记录。

**HAL 随仓库**解决的是第 1 条：不依赖本机安装的 STM32CubeF1。
**CubeMX 重生成的安全流程**解决的是“以后还能继续交付”，不是一次性跑通。

所以：交付不是“把代码跑起来一次”，而是“别人 clone 下来还能跑，自己下次改也不把证据弄坏”。

---

## 8. 面试怎么讲（30 秒版）

> 这个工程把 HAL/CMSIS 源码直接入库，CMake 用相对路径引用，所以 clone 下来不需要装 STM32CubeF1，构建可复现。
> CubeMX 只用来生成初始化代码，业务代码放在 USER CODE 区和独立 `app_*.c`；重新生成后我会按 diff 检查链接脚本的只读段、`stm32f1xx_it.c` 的重复中断向量和 HAL 模块开关。
> 平衡车引脚已经占满，CAN 只有一路，所以 CAN 双节点放到另一个工程做，不污染这个已交付仓库。

---

## 9. 术语小抄

| 术语 | 解释 |
|---|---|
| **HAL** | ST 的硬件抽象层，封装外设操作 |
| **CMSIS** | ARM 的 Cortex-M 内核接口层 |
| **.ioc** | CubeMX 工程配置文件 |
| **USER CODE 区** | `main.c` 等文件里 `/* USER CODE BEGIN */ ... /* USER CODE END */` 之间，CubeMX 不会改 |
| **链接脚本 (.ld)** | 告诉链接器代码/数据放到 Flash/RAM 的哪个地址 |
| **RWX** | 可读/可写/可执行权限；Flash 段不该有 W |
| **NVIC** | Cortex-M 的中断控制器，管优先级 |
| **`multiple definition`** | 同一个函数被定义两次，链接器报错 |

---

## 10. 相关文件

- 工程配置：[CARTESTFreertos.ioc](../CARTESTFreertos.ioc)
- HAL 路径与驱动列表：[cmake/stm32cubemx/CMakeLists.txt](../cmake/stm32cubemx/CMakeLists.txt)
- 链接脚本：[stm32f103c8tx_flash.ld](../stm32f103c8tx_flash.ld)
- 中断文件：[Core/Src/stm32f1xx_it.c](../Core/Src/stm32f1xx_it.c)
- HAL 模块开关：[Core/Inc/stm32f1xx_hal_conf.h](../Core/Inc/stm32f1xx_hal_conf.h)
- 主程序：[Core/Src/main.c](../Core/Src/main.c)
- 构建环境：[docs/env.md](env.md)
- 交付验收状态：[README.md](../README.md)
