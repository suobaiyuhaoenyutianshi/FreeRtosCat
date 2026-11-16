# 变更记录：USART3 错误回调清标志并条件重装

- 日期：2026-11-16
- 提交：`d00db29`（`fix(rc): USART3 错误回调清标志并按 RxState 重新武装接收`）
- 文件：`Core/Src/app_rc.c`（`HAL_UART_ErrorCallback`）
- 验证状态：**未上板验证**（本次提交仅做静态修正，蓝牙收发/错误注入回归尚未执行）

## 1. 改前

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart3) return;

    testRcErr++;
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng);
}
```

问题：
- HAL 在 FE/NE/ORE 后调用本回调，但错误标志未被显式清除；
- 不判断 `RxState` 就直接重装，若 HAL 尚未释放 DMA/接收锁，重装可能失败或被忽略；
- 两种情况都可能导致“错误后不再进空闲回调”，表现为蓝牙永久停收。

## 2. 改后

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart3) return;

    __HAL_UART_CLEAR_PEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);

    if (huart->RxState == HAL_UART_STATE_READY)
    {
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf[s_dma_idx], MAX_leng);
    }
    testRcErr++;
}
```

## 3. 原因

1. 清除 PE/FE/NE/ORE 标志，避免错误标志残留引发中断风暴或后续帧被立即判错。
2. 仅当 `RxState == HAL_UART_STATE_READY` 时重装，确保 HAL 已释放 DMA 与接收流程。
3. 当前帧按设计丢弃：一帧丢失不影响遥控，下次空闲事件会重新建立帧边界。

## 4. 影响面与风险

| 项 | 说明 |
|---|---|
| 影响文件 | 仅 `Core/Src/app_rc.c` 的 `HAL_UART_ErrorCallback` |
| 正常路径 | 无改动：`HAL_UARTEx_RxEventCallback` 的双缓冲逻辑不变 |
| 失败路径 | 错误帧丢弃；若 `RxState` 不是 READY，本次不重装，等待 HAL 状态恢复 |
| 残留风险 | 清标志与检查之间若新字节到达，可能丢 1 字节；未上板验证前不排除重装仍失败 |
| 回滚方式 | `git revert d00db29` 或恢复为无条件重装 |

## 5. 待办验证（硬件回归）

1. 正常蓝牙收发 10 帧，确认 `RC<- [...]` 打印与 `move` 响应正常。
2. 人为制造 ORE（降低串口工具速率/连续灌数据），观察 `testRcErr` 增长后仍能继续收帧。
3. 拔插蓝牙模块电源，确认恢复后无需复位即可继续接收。
4. 回归通过后，在本文件补充“验证结果”，并把状态改为“已上板验证”。
