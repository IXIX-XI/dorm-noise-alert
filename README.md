# 静享寝居 · Dorm Noise Alert

基于 **STM32F103C8T6** 的宿舍智能噪音监测与提醒系统。双麦克风全向拾音，三色 LED 实时指示声级，超过阈值时通过 **4G 模组（合宙 Air780E 内核）** 向**企业微信群机器人**推送告警。

STM32F103-based dormitory noise monitoring & alert system. Dual MAX4466 mics, 3-color LED level indicator, 4G (Air780E) push to a WeCom group bot.

## ✨ 功能特性 / Features

- **双麦全向拾音**：2 路 MAX4466 背向布置，取两路最大值，消除指向性盲区
- **定点声级算法**：12 位 ADC 采样 → 去直流 → 功率域平滑 → 定点 RMS → 8.8 定点 log2 → 换算 dB，无 FPU 也能跑
- **三色灯分级指示**：绿灯常亮（安静）/ 黄灯闪烁（中等）/ 红灯常亮（吵闹）
- **4G 远程告警**：超阈值自动向企业微信群推送（含音量值，不含人名/位置）
- **无串口自诊断**：开机自检 + LED 闪码，无需串口也能定位故障

## 🧰 硬件清单 / BOM

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | STM32F103C8T6 (Blue Pill) | SPL 标准库 + Keil5 |
| 麦克风 ×2 | MAX4466 | 背向布置，OUT → PA0 / PA1 |
| 指示 | 三色 LED（共阳） | 绿 PA4 / 黄 PB0 / 红 PB1，低电平点亮 |
| 4G 模组 | 银尔达 YED-M100M-C2 | 合宙 Air780E 内核，3.3~4.2V 宽压 |
| 电源 | 3.7V 锂电池 + TP4056 | M100 直吃电池电压，STM32 经 LDO 转 3.3V |

**接线**：STM32 PA9(TX) → M100 RXD；STM32 PA10(RX) → M100 TXD；必须共地；VIN 旁就近并 470µF 电解电容。

## 🚀 快速开始 / Quick Start

1. 用 Keil5 打开工程，编译（`Rebuild all`，确认 0 Error）。
2. **替换 Webhook Key**：打开 `User/main.c`，修改下面这一行为你自己的企业微信群机器人 key：

   ```c
   #define WECOM_WEBHOOK_KEY    "YOUR_WEBHOOK_KEY_HERE"
   ```

   > ⚠️ 切勿把真实 key 提交到公开仓库。在企业微信群里添加「群机器人 → 自定义机器人」，复制 Webhook 里 `key=` 后面那段。

3. 烧录到 STM32。
4. 上电观察：
   - 绿→黄→红各 0.3s（灯自检）→ 黄灯常亮几秒（4G 注册）→ 三灯齐闪 2 次（自检通过）
   - 随后**自动发送一条 65.0 dB 测试推送**：绿灯慢闪 3 次 = 成功；灭 2 秒后红灯闪 N 次 = 失败（见下表）

## 📟 LED 闪码 / Blink Codes

| N | 含义 | 排查方向 |
|---|---|---|
| 1 | M100 不回 AT | 串口接线 / 供电 |
| 2 | SIM 未识别 | SIM 卡 / 卡槽 |
| 3 | 网络未注册 | 信号 / 欠费 |
| 4 | `AT+HTTPINIT` 失败 | 模组固件 |
| 5 | 收不到 `DOWNLOAD` | body 长度 / 数据模式 |
| 6 | `HTTPACTION` 15s 超时 | **4G 信号弱 / 无流量 / PDP 未激活** |
| 7 | 服务器返回非 200 **或 errcode≠0** | **key 错 / 机器人类型不对 / 频率受限** |
| 8 | 麦克风异常 | 接线 / 增益 |

## 📐 阈值与校准

`User/main.c` 顶部：

```c
#define THRESH_LOW    450u   /* 45.0 dB：安静/中等分界 */
#define THRESH_HIGH   650u   /* 65.0 dB：中等/吵闹分界，触发告警 */
#define SPL_OFFSET_DB 840    /* 声级校准偏移（未校准） */
```

校准 `SPL_OFFSET_DB`：手机分贝仪与装置同位置比对，差值 ×10 加到该宏上。

## 📄 许可证 / License

MIT License
