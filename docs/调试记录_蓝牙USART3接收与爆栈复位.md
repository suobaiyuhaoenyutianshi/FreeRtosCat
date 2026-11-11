# 调试记录：蓝牙 USART3 接收失效 + 爆栈导致 IWDG 复位循环（2026-11-08）

## 一、现象（按时间顺序）

1. 不接蓝牙时，平衡车能自己站住；用手机蓝牙一发指令，车立刻倾倒。
2. `testrc`（USART3 中断里按字节累加）最多涨到 23 左右就停住；
   `testRcrEecip`（rc 任务里按“整行/整帧”累加）最多 2，永远超不过 2。
3. USART2 调试口上收到的行是乱码：`RC<- [mmove 0 0]`、`RC<- [mve 0 0]`、`RC<- [m]`。
4. 调试器里 `g_ulStackOverflowCount` 从 0 变成 1；`g_ulMallocFailedCount` 一直是 0。
5. USART2 上 `B1` 和 `12345abcdef6789AB2` 每隔约 0.44s 重复一次。

## 二、结论：三个独立问题，最后一个才是“看起来收不到”的真凶

| 编号 | 问题 | 现象 | 根因 |
|---|---|---|---|
| 1 | HAL 逐字节接收遇 ORE 永久停收 | testrc 涨一小段后冻结 | `UART_EndRxTransfer()` 关掉 RXNEIE/PEIE/EIE 且无人重装 |
| 2 | DMA + 空闲中断旧版本更脆 | 之前“完全收不到” | DMA 模式下 HAL 把**任何错误**都当致命错误；启动返回值没检查；双缓冲有竞态 |
| 3 | **rc 任务栈溢出 → 爆栈钩子死循环 → IWDG 复位循环** | 车一收指令就倒、计数停在 23/2、行内容乱码 | 1KB 任务栈里跑 newlib 的 `sscanf` + `vsnprintf` |

> 关键认识：第 3 个问题会让整块板子每 ~0.41s 复位一次，看起来像“串口收不到/收到乱码”，
> 实际是 **CPU 在反复重启**，接收代码本身并没有错。

## 三、证据链

### 1. 为什么 `testrc` 和 `testRcrEecip` 对不上

- `testrc`：每收到 **1 个字节** +1（USART3 中断里）。
- `testRcrEecip`：每收齐 **1 整行/整帧** +1（rc 任务里）。
- 一条 `move 0 0\r\n` 是 10 个字节 → 只算 1 行/帧。
- 所以“23 个字节 / 2 行”比例约 9:1，本身是正常算术，不是 bug。

### 2. 为什么“栈水位显示 rc 还剩 228 字”却仍然爆栈

Memory Monitor 的输出是在**每轮开机最开始**打印的（`ulFeedDiv` 初始为 0），
此时 `App_RC_Proc()` 还没执行过；打印完之后第一条蓝牙数据进来，
`sscanf` + `vsnprintf` 才把剩余 912 字节栈一次性吃光。
所以水位报告永远“看起来正常”，实际爆栈发生在报告之后。

### 3. 为什么 `B1` + `12345abcdef6789AB2` 每 0.44s 重复（逐帧解读）

这一串就是**一次完整开机**的日志被串在一起：

| 输出 | 来源 |
|---|---|
| `B1` | `main()` 里 `HAL_UART_Transmit(&huart2,"B1\r\n")` |
| `1 2 3 4 5` | `BOOT_STEP('1')` 到 `BOOT_STEP('5')` |
| `a b c d e f` | `App_MPU6050_Init()` 里的 `boot_mark('a')` 到 `'f'` |
| `6 7 8 9 A` | `BOOT_STEP('6')` 到 `BOOT_STEP('A')` |
| `B2` | 调度器启动前的最后一个路标 |

拼起来正好是 `12345abcdef6789AB2`。**它每出现一次，就代表板子重启了一次**——这是整次调试里最值钱的一条线索。

复位周期是这样构成的：

```
收到一行 → rc 任务打印 RC<- → 进入 App_RC_Proc
        → sscanf + vsnprintf 把 1KB 栈吃爆
        → vApplicationStackOverflowHook：taskDISABLE_INTERRUPTS() + while(1)
        → 运行期唯一喂狗的人（MemMon 任务）也停了
        → IWDG 计数 4095 / 10kHz ≈ 0.41s 到点 → 硬件复位
        → 重新打印 B1 ... B2
```

用实际日志对时间：

```
14:13:11.576  B1                    ← 上一轮复位后的开机
14:13:11.679  12345abcdef6789AB2
14:13:11.680  Memory Monitor        ← 这一轮唯一一次监控报告（开机时打的）
14:13:11.717  RC<- [mmove 0 0]      ← 收到指令，进入 App_RC_Proc
（之后 CPU 已经死在爆栈钩子的 while(1) 里，再也不输出）
14:13:12.201  B1                    ← 0.484s 后 IWDG 复位
```

`11.717 → 12.201` 这 0.484s ≈ **0.41s IWDG + 开机时间 + 等指令的时间**。
另一轮 `12.303 → 12.720` 是 0.417s，几乎就是纯 IWDG 时间。
间隔不完全相等，是因为“复位后多久收到第一条指令”每次不同。

三个顺带的疑惑：

- **为什么每轮只看到一次 Memory Monitor？** 监控是每 2 秒打印一次，但系统每 0.4~0.6 秒就复位，永远活不到第二次；开机那次是 `ulFeedDiv==0` 的立即打印。
- **为什么栈水位显示 `rc` 还剩 228 字？** 报告在 `App_RC_Proc` 执行**之前**采样；报告打完，第一条指令进来，`sscanf`+`vsnprintf` 才把剩下的 912 字节吃光。
- **为什么 `RC<-` 内容是 `mmove 0 0` / `mve 0 0` / `m`？** 见下一节。

### 4. 多出来的那个 `m` 到底从哪来（不是 RAM 残留）

复位**不会**把上一轮的数据带到下一轮：`directIntBuf` / `proBuf` / `intBufCursor`
都在 `.bss` 段，启动文件在进 `main` 前会整段清零（`startup_stm32f103xb.s` 的 `FillZerobss`）。

那个 `m` 是**这一轮开机过程中**收下来的：

```
上电/复位
  │
  ├─ MX_USART3_UART_Init()        USART3 硬件开始收发
  ├─ App_RC_Init()                __HAL_UART_ENABLE_IT(RXNE) 打开中断使能
  │                                ★ 但 FreeRTOS 已进过临界区，BASEPRI=0x50，
  │                                  USART3（优先级 5）被屏蔽
  ├─ App_MPU6050_Init() DelayUs(100ms)   ← 这 100ms 中断仍被屏蔽
  ├─ 创建任务、App_USART2_Init……
  │                                ★ 这段窗口里手机一直在发 move 0 0：
  │                                  硬件照收，但 RDR 只能放 1 个字节，
  │                                  其它全部 ORE 丢掉
  └─ vTaskStartScheduler() → SVC 里 msr basepri,#0 → 中断终于打开
         │
         └─ 第一个字节被读走，成为 intBufCursor=0 处的 directIntBuf[0]
```

如果幸存字节正好是 `m`，后面 `ove 0 0\r\n` 都在屏蔽窗口里丢了，
那么下一帧完整的 `move 0 0\r\n` 会追加在它后面：

```
"m" + "move 0 0\r\n"  →  "mmove 0 0"  →  RC<- [mmove 0 0]
```

所以每轮形状不同，取决于“中断打开那一瞬间手机发到哪个字节”。

## 四、这次真正帮上忙的调试手段（本文最有价值的部分）

> 结论先行：**找到问题比修问题难得多。** 这次真正定位到根因，靠的不是读代码猜，
> 而是下面这几类“路标 + 计数器 + 标志位 + 时间戳”的组合。

### 1. 开机路标链：一眼区分“收不到”和“在重启”

- `B1`、`BOOT_STEP('1'..'9','A')`、`App_MPU6050_Init` 里的 `boot_mark('a'..'f')`、`B2`。
- 正常上电只应出现**一次**；如果 `B1` 和 `12345abcdef6789AB2` **周期性重复**，
  说明不是串口问题，而是 **CPU 在反复复位**。
- 这是把“接收 bug”误判成“复位循环”的关键一步。

### 2. 关键计数器：把“感觉”变成“数字”

| 计数器 | 位置 | 能说明什么 |
|---|---|---|
| `testrc` | USART3 中断 | 字节到底有没有进来；停在某个值 = 要么没数据、要么 CPU 死了 |
| `testRcrEecip` | rc 任务 | 整帧有没有解析；不涨 = 没成帧或任务没跑 |
| `testRcErr` | 错误回调 | FE/NE/ORE 触发了多少次重新武装；一直涨 = 线上有噪声或波特率不对 |
| `g_ulStackOverflowCount` / `g_cOverflowTaskName` | 爆栈钩子 | **0→1 就直接锁定“栈溢出”**，并告诉你是哪个任务 |
| `g_ulMallocFailedCount` / `g_ulMallocFailedFreeHeap` / `g_pvMallocFailedCaller` | malloc 失败钩子 | 区分“堆耗尽”和“碎片化”；`caller` 还能反查调用点 |
| `g_diag.tim2_irq_count` | TIM2 中断 | 会涨 = 调度器时基正常；会归零 = 发生过复位 |
| `tx_cplt_count` | USART2 DMA 完成回调 | 打印链路是否正常；不涨 = 打印会阻塞喂狗任务 |

### 3. 标志位/寄存器：判断“到底是哪一类串口错误”

`USART3->SR` 低 8 位：

```
bit7 TXE   发送寄存器空
bit6 TC    发送完成
bit5 RXNE  收到一个字节
bit4 IDLE  总线空闲
bit3 ORE   溢出（上一个没读走，下一个又到了，丢 1 字节）
bit2 NE    噪声（一位中间 3 次采样不一致）
bit1 FE    帧错误（停止位采样到 0，通常=波特率不匹配）
bit0 PE    奇偶校验错误（本工程无校验，正常不出现）
```

配套看：

- `USART3->CR1` bit5（RXNEIE）——是否为 1，能证明“中断有没有被关掉”；
- `huart3.RxState`——`0x22`=DMA/IT 接收已武装，`0x20`=READY（可能已被中止）；
- `huart3.RxXferSize`——裸 ISR 版本恒为 0；HAL 逐字节版本为 1（可用来确认“跑的是哪版固件”）。

### 4. 日志时间戳：算出复位周期

- 串口助手每行都有时间（`14:13:11.576`）。
- 比较相邻 `B1` 的时间差：≈0.41s 就是 IWDG；更长则加上“等第一条指令”的时间。
- IWDG 参数：`Prescaler=4`、`Reload=4095`、LSI≈40kHz → 4095/10kHz ≈ **0.41s**。

### 5. 排查方法论：先把现象分成三类

```
“串口收不到”其实只有三种可能：
  ① 字节根本没进来        → 看 testrc、USART3->SR.RXNE、接线/波特率
  ② 字节进来了但没成行    → testrc 涨、testRcrEecip 不涨 → 缺 \r\n 或没空闲间隔
  ③ 字节进来了但 CPU 死了/复位 → 计数器停在某值不涨 / B1 重复 / 硬件看门狗
```

先分类，再针对每一类看对应的“路标 + 计数器 + 标志位”，比盲目读代码快得多。
这次就是先看到 `g_ulStackOverflowCount` 0→1 和 `B1` 重复，才把“接收 bug”纠正为“复位循环”。

## 五、已实施的修复

| 修复 | 位置 |
|---|---|
| 接收实现：先改裸 RXNE 中断（提交 `4311c42`），最终切回 **DMA + 空闲中断 + 双缓冲**（提交 `de44803`） | `Core/Src/app_rc.c`、`Core/Src/stm32f1xx_it.c` |
| DMA 双缓冲：两块 DMA 缓冲交替 + 独立成品缓冲 `s_ready_buf`，任务拷贝时 DMA 不会踩同一块内存 | `Core/Src/app_rc.c` |
| `HAL_UART_ErrorCallback`：FE/NE/ORE 时丢弃当前帧并重新 `HAL_UARTEx_ReceiveToIdle_DMA` | `Core/Src/app_rc.c` |
| 过滤 `HAL_UART_RXEVENT_HT`（DMA_NORMAL 下 32 字节“半传输”不是帧结束） | `Core/Src/app_rc.c` |
| 检查 `HAL_UARTEx_ReceiveToIdle_DMA` 启动返回值（有挂起错误会返回 HAL_ERROR） | `Core/Src/app_rc.c` |
| 删掉 `sscanf`，改手写“跳过空格 + 可选符号 + 十进制整数”解析 | `Core/Src/app_rc.c` |
| 删掉 `vsnprintf` 打印，改 `App_USART2_SendRaw()`（只做 strlen + memcpy） | `Core/Src/app_usart2_dma.c` |
| `rc` 任务栈 256 → **512 字** | `Core/Src/main.c` |
| FreeRTOS 堆 7680 → **9728**（任务栈从堆里分配，栈加大后堆必须同步加大） | `freeRTOS/Inc/FreeRTOSConfig.h` |
| `App_USART2_Printf` 入队等待 `portMAX_DELAY` → 100ms 超时 | `Core/Src/app_usart2_dma.c` |
| 收到 turn/move 后先限幅到 ±100 | `Core/Src/app_rc.c` |
| **倾角安全停机**：pitch > +40° 或 < -40° 立即 `App_Motor_Cmd(0)`（拉低 STBY 断电），清积分/PID/遥控目标；回正到 ±5° 以内才恢复（滞回） | `Core/Src/app_control.c` |

构建结果（Debug）：FLASH 约 48.1 KB（73.4%），RAM 约 14.6 KB（71.1%），0 error。

## 六、验证清单

1. `g_ulStackOverflowCount` 一直为 **0**。
2. USART2 上不再重复出现 `B1` / `12345abcdef6789AB2`。
3. `RC<-` 后面是干净的 `move 0 0`，`testRcrEecip` 能超过 2 并持续增长。
4. `testRcErr` 尽量保持 0；若持续增长，说明线上有 FE/NE/ORE（噪声/波特率），但接收会自动恢复。
5. 车倾斜超过 ±40°：电机立即断电（推轮子无阻力）；放回 ±5° 以内自动恢复。

## 七、DMA 版本的实现要点与已知限制

1. **每次空闲/收满都要重新武装**：`HAL_UARTEx_ReceiveToIdle_DMA` 在 RxEvent 回调里调用。
2. **错误必须重装**：DMA 模式下 HAL 把 FE/NE/ORE 都当阻塞错误，
   `HAL_UART_ErrorCallback` 里不重新武装就会永久停收。
3. **启动返回值必须检查**：启动瞬间有挂起错误会返回 `HAL_ERROR` 并复位 `ReceptionType`。
4. **半传输事件不是帧结束**：`HAL_UART_RXEVENT_HT` 要直接忽略。
5. **帧边界依赖空闲**：发送端两条指令之间要留 ≥1 字节时间（9600 约 1.04ms）；
   连续不停发时会按 64 字节切帧（本实现截到 63 字节）。
6. **双缓冲竞态**：任务读 `ucready` 后、拷贝前若 ISR 又翻缓冲，DMA 会写到任务正在读的缓冲；
   本实现用“DMA 缓冲 + 独立成品缓冲”彻底避开（临界区挡不住 DMA）。

## 八、USART 错误标志速查（`USART3->SR`）

见第四节第 3 小节；补充：STM32F1 规则是**先读 SR、再读 DR**，
这一对读操作会一次性清掉 PE/FE/NE/ORE（同时清 RXNE）。

- 裸 RXNE 方案里这些错误只丢字节，RXNEIE 不会被关，接收继续；
- HAL 的 IT/DMA 方案里 ORE（DMA 模式下任意错误）会触发 `UART_EndRxTransfer()` 关中断，
  没有 `HAL_UART_ErrorCallback` 重装就是“接收永久停摆”的根源。

## 九、经验规则（以后不要再踩）

1. **中断里要调用 RTOS FromISR API 的，优先级数字必须 >= `configMAX_SYSCALL_INTERRUPT_PRIORITY`（本工程=5）**；不调用 RTOS 的高频中断再往下降（本工程=6）。
2. **不要在 1KB 任务栈里跑 newlib 的 `sscanf`/`vsnprintf`**；要么加大栈（≥2KB），要么改手写解析/裸打印。
3. **爆栈钩子和 malloc 钩子都是“关中断 + 死循环”**，会被 IWDG 表现成“每 0.41s 复位一次”。
   看到反复复位，先看 `g_ulStackOverflowCount` / `g_ulMallocFailedCount`。
4. **Memory Monitor 的栈水位是在开机时刻采样的**，只能反映那之前的使用量，不能证明“运行中不会爆栈”。
5. 判断“串口收不到”之前，先分清：字节没进来、字节进来但没成行、字节进来了但 CPU 被复位/卡死。
6. **改实现前先提交**：切换接收方案前把当前可用版本提交到 git，避免“直接覆盖、旧版本丢失”。
