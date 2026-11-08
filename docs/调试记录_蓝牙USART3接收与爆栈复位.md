# 调试记录：蓝牙 USART3 接收失效 + 爆栈导致 IWDG 复位循环（2026-11-08）

## 一、现象（按时间顺序）

1. 不接蓝牙时，平衡车能自己站住；用手机蓝牙一发指令，车立刻倾倒。
2. `testrc`（USART3 中断里按字节累加）最多涨到 23 左右就停住；
   `testRcrEecip`（rc 任务里按“整行”累加）最多 2，永远超不过 2。
3. USART2 调试口上收到的行是乱码：`RC<- [mmove 0 0]`、`RC<- [mve 0 0]`、`RC<- [m]`。
4. 调试器里 `g_ulStackOverflowCount` 从 0 变成 1；`g_ulMallocFailedCount` 一直是 0。
5. USART2 上 `B1` 和 `12345abcdef6789AB2` 每隔约 0.44s 重复一次。

## 二、结论：三个独立问题，最后一个才是“看起来收不到”的真凶

| 编号 | 问题 | 现象 | 根因 |
|---|---|---|---|
| 1 | HAL 逐字节接收遇 ORE 永久停收 | testrc 涨一小段后冻结 | `UART_EndRxTransfer()` 关掉 RXNEIE/PEIE/EIE 且无人重装 |
| 2 | DMA + 空闲中断版本更脆 | 之前“完全收不到” | DMA 模式下 HAL 把**任何错误**都当致命错误；启动返回值没检查；双缓冲有竞态 |
| 3 | **rc 任务栈溢出 → 爆栈钩子死循环 → IWDG 复位循环** | 车一收指令就倒、计数停在 23/2、行内容乱码 | 1KB 任务栈里跑 newlib 的 `sscanf` + `vsnprintf` |

> 关键认识：第 3 个问题会让整块板子每 ~0.41s 复位一次，看起来像“串口收不到/收到乱码”，
> 实际是 **CPU 在反复重启**，接收代码本身并没有错。

## 三、证据链

### 1. 为什么 `testrc` 和 `testRcrEecip` 对不上

- `testrc`：每收到 **1 个字节** +1（USART3 中断里）。
- `testRcrEecip`：每收齐 **1 整行**（遇到 `\r`/`\n`）+1（rc 任务里）。
- 一条 `move 0 0\r\n` 是 10 个字节 → 只算 1 行。
- 所以“23 个字节 / 2 行”比例约 9:1，本身是正常算术，不是 bug。

### 2. 为什么“栈水位显示 rc 还剩 228 字”却仍然爆栈

Memory Monitor 的输出是在**每轮开机最开始**打印的（`ulFeedDiv` 初始为 0），
此时 `App_RC_Proc()` 还没执行过；打印完之后第一条蓝牙数据进来，
`sscanf` + `vsnprintf` 才把剩余 912 字节栈一次性吃光。
所以水位报告永远“看起来正常”，实际爆栈发生在报告之后。

### 3. 为什么 `B1` + `12345abcdef6789AB2` 每 0.44s 重复

这一串就是完整开机序列被拼在一起：

```
B1                        main() 里的第一个路标
1 2 3 4 5                 BOOT_STEP('1'..'5')
a b c d e f               App_MPU6050_Init 里的 boot_mark('a'..'f')
6 7 8 9 A                 BOOT_STEP('6'..'9','A')
B2                        调度器启动前的最后一个路标
```

复位周期构成：

```
收到一行 → RC<- 打印 → App_RC_Proc 爆栈
        → vApplicationStackOverflowHook：taskDISABLE_INTERRUPTS() + 死循环
        → 运行期唯一喂狗者 MemMon 任务也被停掉
        → IWDG 超时（Prescaler=4 / Reload=4095 / LSI≈40kHz ≈ 0.41s）复位
        → 重新打印 B1 ... B2
```

所以复位间隔 = 0.41s + 开机时间 + “下一条指令到达的时刻”，
日志里 0.44s / 0.51s / 0.63s 不相等就是这个原因。
`RC<- [mmove 0 0]` 里的多出来的 `m`、`[mve 0 0]` 少掉的 `o`、`[m]`，
都是**复位瞬间半行 + 复位后整行**拼接出来的，不是线路噪声。

### 4. IWDG 参数

`MX_IWDG_Init()`：`Prescaler = IWDG_PRESCALER_4`，`Reload = 4095`。
LSI ≈ 40 kHz，4 分频后 10 kHz，4095 / 10 kHz ≈ **0.41s**。
运行期只有 `memory_monitor.c` 里的 MemMon 任务调用 `HAL_IWDG_Refresh()`，
任何让 MemMon 卡死超过 0.41s 的事情都会导致复位。

## 四、已实施的修复

| 修复 | 位置 |
|---|---|
| USART3 接收改为**裸 RXNE 中断**：开一次 `RXNEIE`，ISR 直接读 `SR`+`DR`，不经过 HAL 状态机 | `Core/Src/app_rc.c`（`App_RC_Init` / `App_RC_ISR`）、`Core/Src/stm32f1xx_it.c`（`USART3_IRQHandler` 里调用后直接 return） |
| 删掉 `sscanf`，改手写“跳过空格 + 可选符号 + 十进制整数”解析 | `Core/Src/app_rc.c` 的 `rc_parse_int` |
| 删掉 `vsnprintf` 打印，改 `App_USART2_SendRaw()`（只做 strlen + memcpy） | `Core/Src/app_usart2_dma.c` |
| `rc` 任务栈 256 → **512 字** | `Core/Src/main.c` 的 `xTaskCreate(APP_rc_rxTask, ...)` |
| FreeRTOS 堆 7680 → **9728**（任务栈从堆里分配，栈加大后堆必须同步加大） | `freeRTOS/Inc/FreeRTOSConfig.h` |
| `App_USART2_Printf` 入队等待 `portMAX_DELAY` → 100ms 超时 | `Core/Src/app_usart2_dma.c` |
| 收到 turn/move 后先限幅到 ±100 | `Core/Src/app_rc.c` |
| **倾角安全停机**：pitch > +40° 或 < -40° 立即 `App_Motor_Cmd(0)`（拉低 STBY 断电），清积分/PID/遥控目标；回正到 ±5° 以内才恢复（滞回） | `Core/Src/app_control.c` `App_Control_Proc` 开头 |

构建结果（Debug）：FLASH 约 47.5 KB（72.5%），RAM 约 14.5 KB（70.8%），0 error。

## 五、验证清单

1. `g_ulStackOverflowCount` 一直为 **0**。
2. USART2 上不再重复出现 `B1` / `12345abcdef6789AB2`。
3. `RC<-` 后面是干净的 `move 0 0`，`testRcrEecip` 能超过 2 并持续增长。
4. 车倾斜超过 ±40°：电机立即断电（推轮子无阻力）；放回 ±5° 以内自动恢复。
5. 若第 3 条仍不满足，再按 `USART3->SR` 的 FE/NE/ORE 标志检查波特率/干扰/中断延迟。

## 六、以后改回 DMA + 双缓冲时的注意事项

原提交版本的问题不是“双缓冲思路错”，而是缺少错误恢复 + 启动检查。
改回时必须补齐：

1. **实现 `HAL_UART_ErrorCallback`**：在回调里重新调用
   `HAL_UARTEx_ReceiveToIdle_DMA(&huart3, 当前缓冲, MAX_leng)`。
   DMA 模式下 HAL 把任何 FE/NE/ORE 都当阻塞错误，不重装就永久停收。
2. **检查 `HAL_UARTEx_ReceiveToIdle_DMA` 返回值**：启动瞬间若有挂起错误会返回
   `HAL_ERROR` 并把 `ReceptionType` 复位成 STANDARD，之后永远不会进
   `HAL_UARTEx_RxEventCallback`。
3. **双缓冲竞态**：任务读 `ucready` 之后、`strncpy` 之前，如果 ISR 又完成一帧并翻转
   缓冲，DMA 会被重新武装到**任务正在拷贝的那块内存**上；`taskENTER_CRITICAL`
   只能挡 CPU 中断，**挡不住 DMA 写内存**。解决方式任选：
   - 任务拷贝期间置 `buf_in_use` 标志，ISR 不要重新武装到该缓冲；
   - 增加到 3 块缓冲，按“完成队列”轮转；
   - 或改成 DMA 循环模式 + 半传输/传输完成事件，避免每次重装。
4. **64 字节满帧**：`Size >= MAX_leng` 时要明确截断策略（当前是截到 63 并丢掉最后
   一个字节），且连续数据没有空闲间隔时要靠“收满”切帧。
5. 空闲切帧依赖发送端在两条指令之间留出 ≥1 字节时间的间隙（9600 下约 1.04ms）。

## 七、USART 错误标志速查（`USART3->SR`）

```
bit7 TXE   发送寄存器空
bit6 TC    发送完成
bit5 RXNE  收到一个字节（接收中断靠它触发）
bit4 IDLE  总线空闲
bit3 ORE   溢出：上一个字节没读走，下一个又到了（丢 1 字节）
bit2 NE    噪声：一位中间 3 次采样不一致（干扰）
bit1 FE    帧错误：停止位采样到 0（波特率不匹配/接线/电平反相）
bit0 PE    奇偶校验错误（本工程无校验，正常不应出现）
```

- STM32F1 规则：**先读 SR、再读 DR**，会一次性清掉 PE/FE/NE/ORE（同时清 RXNE）。
- 裸 RXNE 方案里这些错误只丢字节，RXNEIE 不会被关，接收继续；
- HAL 的 IT/DMA 方案里 ORE（DMA 模式下任意错误）会触发 `UART_EndRxTransfer()` 关中断，
  且没有自动重装，是“接收永久停摆”的根源。

## 八、经验规则（以后不要再踩）

1. **中断里要调用 RTOS FromISR API 的，优先级数字必须 >= `configMAX_SYSCALL_INTERRUPT_PRIORITY`（本工程=5）**；不调用 RTOS 的高频中断再往下降（本工程=6）。
2. **不要在 1KB 任务栈里跑 newlib 的 `sscanf`/`vsnprintf`**；要么加大栈（≥2KB），要么改手写解析/裸打印。
3. **爆栈钩子和 malloc 钩子都是“关中断 + 死循环”**，会被 IWDG 表现成“每 0.41s 复位一次”。
   看到反复复位，先看 `g_ulStackOverflowCount` / `g_ulMallocFailedCount`。
4. **Memory Monitor 的栈水位是在开机时刻采样的**，只能反映那之前的使用量，不能证明“运行中不会爆栈”。
5. 判断“串口收不到”之前，先分清：字节没进来、字节进来但没成行（缺 `\r\n`）、
   字节进来了但 CPU 被复位/卡死。
