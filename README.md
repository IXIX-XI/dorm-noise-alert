# 静享寝居 · Dorm Noise Alert

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-STM32F103-blue.svg)](https://www.st.com/en/microcontrollers-microprocessors/stm32f103c8t6.html)
[![Framework](https://img.shields.io/badge/framework-SPL_Standard_Peripheral_Library-orange.svg)](https://www.st.com/en/embedded-software/stm32-embedded-software.html)
[![IDE](https://img.shields.io/badge/IDE-Keil5-009900.svg)](https://www.keil.com/)

> 🏆 萌芽杯参赛作品

基于 **STM32F103C8T6** 的宿舍智能噪音监测与提醒系统。双麦克风全向拾音，三色 LED 实时指示声级，超过阈值时通过 **4G 模组（合宙 Air780E 内核）** 向**企业微信群机器人**推送告警。

STM32F103-based dormitory noise monitoring & alert system. Dual MAX4466 mics for omnidirectional pickup, 3-color LED level indicator, and 4G (Air780E) push to a WeCom group bot when noise exceeds threshold.

---

## 🎯 系统架构 / Architecture

```
  MAX4466 ×2                    STM32F103C8T6                  银尔达 M100 (Air780E)        企业微信群机器人
  ┌────────┐                    ┌──────────────┐               ┌──────────────────┐        ┌──────────────┐
  │麦1 → PA0│──┐                 │              │   PA9(TX) →   │  RXD (Pin4)      │        │              │
  │麦2 → PA1│──┤→ ADC1 CH0/1 ──→ │ RMS + dB 换算 │               │                  │  HTTP  │              │
  └────────┘  │                 │  阈值判断     │               │ HTTPDATA → body  │ ────→  │ errcode=0?   │
              │                 │  LED 驱动     │   PA10(RX) ←   │ HTTPACTION → 200 │        │ ✓ 投递消息   │
  绿 PA4 ─────┼────────────────→ │  PA4 / PB0 / PB1              │  +HTTPACTION     │        │              │
  黄 PB0 ─────┤                 │              │               │                  │        └──────────────┘
  红 PB1 ─────┘                 └──────┬───────┘               └──────────────────┘
                                        │ 电池 3.7V → TP4056 → M100 VIN
                                        │          → LDO 3.3V → STM32 VDD
```

---

## ✨ 功能特性 / Features

- **双麦全向拾音**：2 路 MAX4466 背向布置，取两路最大值，消除指向性盲区
- **定点声级算法**：12 位 ADC 采样 → 去直流 → 功率域平滑 → 定点 RMS → 8.8 定点 log2 → 换算 dB，无 FPU 也能跑
- **三色灯分级指示**：绿灯常亮（安静）/ 黄灯闪烁（中等）/ 红灯常亮（吵闹）
- **4G 远程告警**：超阈值自动向企业微信群推送（含音量值，不含人名/位置）
- **无串口自诊断**：开机自检 + LED 闪码，无需串口也能定位故障

### 算法核心（无 FPU 定点实现）

```c
// 一阶高通去直流
dc += ac / 64;
// 功率域一阶平滑 (τ ≈ 80ms)
pwr = (pwr * 7 + (ac - dc) * (ac - dc)) / 8;
// 定点 RMS + log2 → dB
rms   = isqrt32(pwr);
log2  = Log2Q8(rms);          // 8.8 定点 log2, 误差 ≈ 0.26 dB
db10  = (log2 - 2816) * 2352 / 10000;
spl   = db10 + SPL_OFFSET_DB;
```

## 🧰 硬件清单 / BOM

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | STM32F103C8T6 (Blue Pill) | SPL 标准库 + Keil5 |
| 麦克风 ×2 | MAX4466 | 背向布置，OUT → PA0 / PA1 |
| 指示 | 三色 LED（共阳） | 绿 PA4 / 黄 PB0 / 红 PB1，低电平点亮 |
| 4G 模组 | 银尔达 YED-M100M-C2 | 合宙 Air780E 内核，3.3~4.2V 宽压 |
| 电源 | 3.7V 锂电池 + TP4056 | M100 直吃电池电压，STM32 经 LDO 转 3.3V |

**接线**：STM32 PA9(TX) → M100 RXD(Pin4)；STM32 PA10(RX) → M100 TXD(Pin3)；必须共地；VIN 旁就近并 470µF 电解电容。

**⚠️ 电源提醒**：USB 与外部 3.3V 绝对不能同时接（板上 AMS1117 会与外部电源并联抢电流）。建议用 **MCP1700-3.3 / RT9193-3.3 / ME6211C33** 等低 Iq LDO，不要用 AMS1117。

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

## 🛠️ 4G 模组 AT 指令（合宙 Air780E）

```
AT+HTTPINIT
AT+HTTPSSL=1
AT+HTTPPARA="URL","https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=YOUR_KEY"
AT+HTTPPARA="CONTENT","application/json"
AT+HTTPDATA=<body_len>,10000        ← 注意：Air780E 回 DOWNLOAD (不是 SIM800 的 > )
< 发送 JSON body >
AT+HTTPACTION=1
← 等待 +HTTPACTION: 1,200,<len>
AT+HTTPREAD
AT+HTTPTERM
```

## 📁 目录结构

```
.
├── User/                  # 主应用 (main.c)
├── Hardware/              # 板级外设驱动 (LED / Mic / M100)
├── Library/               # STM32 SPL 标准库
├── start/                 # 启动文件 (startup_stm32f10x_md.s)
├── Project.uvprojx        # Keil 工程文件
└── README.md
```

## 📄 许可证 / License

MIT License © IXIX-XI

---

<details>
<summary>📊 开发说明 / Development Notes</summary>

- **为何不用 FPU？** STM32F103C8T6 没有硬件 FPU，用定点运算比软浮点快 5~10 倍
- **为何取两路 max？** 单麦有指向性盲区，两麦背向布置后取 max ≈ 全向拾音
- **为何开机强制推？** 没有串口调试通道，强制推送 + LED 闪码是唯一的无串口自诊断手段
- **为何用 HTTPDATA 而非 HTTPPARA="DATA"?** 前者支持大于 2KB 的 body，后者有长度限制
- **Air780E vs SIM800L：** HTTPDATA 提示符是 `DOWNLOAD` 不是 `>`，这是最容易踩的坑
- **Webhook Key 必须私有**：企业微信群机器人 key 任何人拿到都能刷消息，公开仓库务必占位

</details>
