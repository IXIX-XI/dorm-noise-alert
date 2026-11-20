/**
 * main.c - 噪音监测告警主程序（2 路麦克风 + M100M-C2 4G 推送）
 *
 * 系统架构：
 *   [STM32: 2路麦克风采集 + 三色LED]
 *        --(USART1 TTL)--> [M100M-C2] --[4G Cat.1 HTTPS POST]--> 企业微信群机器人 --> 微信群
 *
 * 告警策略：
 *   2 路麦克风取最大值作为"房间整体噪音水平", 超阈值即向全部 4 人推送,
 *   消息内容不包含声源位置（不点名）。
 *   布 2 路不是为了定位, 而是照顾麦克风的方向性：驻极体麦正面灵敏、
 *   背面衰减, 两个麦朝相反方向摆放即可全向覆盖, 同时互为冗余。
 *
 * 硬件接线（TTL 直连，不需要 MAX3232）：
 *   麦克风1 OUT -> PA0 (ADC1_IN0)
 *   麦克风2 OUT -> PA1 (ADC1_IN1)
 *   绿灯(LED1) PA4, 黄灯(LED2) PB0, 红灯(LED3) PB1
 *   LED 为共阳接法: 阳极接 3.3V, 阴极经 220R 到 GPIO -> 低电平点亮
 *
 *   显示逻辑: 安静 -> 绿灯常亮; 中等 -> 黄灯闪; 吵闹 -> 红灯常亮
 *   STM32 PA9 (TX)  -> M100 RXD  (TTL 3.3V 直连)
 *   STM32 PA10(RX)  <- M100 TXD  (TTL 3.3V 直连, 收 AT 响应)
 *   STM32 GND       <-> M100 GND (必须共地)
 *
 * 调试串口（仅调试用，接 USB转TTL 到电脑）：
 *   STM32 PB10(TX) -> USB转TTL RX   (115200, 看各路实时电平)
 *   STM32 GND      -> USB转TTL GND
 *   正式部署时把 DEBUG_UART 改成 0 即可关闭。
 *
 * 麦克风摆放：
 *   1. 两个麦克风朝相反方向摆放，实现全向覆盖：
 *      驻极体麦正面灵敏、背面衰减，反向摆可消除方向盲区。
 *   2. 两麦都靠近 STM32（线长尽量 ≤1.5 米），短线不必用屏蔽线；
 *      若某路必须拉长到 1 米以上，改用屏蔽线，屏蔽层仅在板子这端接 GND。
 *   3. 麦克风模块的 VCC 就近加一只 100nF 电容到 GND，
 *      抑制电源线引入的干扰。
 *   4. 麦克风选型：用带放大的模块（MAX4466 / MAX9814 等）即可，
 *      不要用无放大的驻极体裸片。
 *
 * 阈值调试：
 *   电平算法: 128 点均方根(RMS) -> 相对满量程 dBFS -> 加偏移得 dB SPL
 *   输出是"定量"的分贝值, 但绝对精度取决于麦克风增益, 所以要校准一次:
 *     1) 手机装个"分贝仪"App, 放在麦克风旁边, 播放一段稳定声音
 *     2) 接调试串口, 对比 App 读数和本程序打印的 SPL
 *     3) 把差值加到 SPL_OFFSET_DB 上 (差值 1dB = 10)
 *   校准后 THRESH_LOW/THRESH_HIGH 就可以直接按环境分贝来设.
 *
 * 模组型号：银尔达 YED-M100M-C2 (核心板, 合宙 Air780E 芯片, 合宙 AT 固件)
 *   AT 指令风格: Quectel/合宙 HTTP 指令 (AT+HTTPINIT / AT+HTTPPARA / AT+HTTPDATA...)
 *
 * 供电（锂电池 + TP4056）：
 *   电池  -> TP4056 B+/B-
 *   TP4056 OUT+ -> M100 的 VIN (本模块为 3.3~4.2V 版本, 可直吃锂电池)
 *   TP4056 OUT- -> GND
 *   VIN 与 GND 之间就近并 470uF 电解电容, 扛 4G 发射尖峰电流
 *   STM32 单独用 Micro-USB 供电 (不要和电池同时插, 避免 5V 顶到电池)
 *   GND   : STM32 与 M100 必须共地, 否则串口无法通信
 *
 * 用法：
 *   1. 把本 main.c 拷到工程的 User/ 目录覆盖现有 main.c
 *   2. 从工程移除 Hardware/LED.c (避免和本文件 LED 函数冲突)
 *   3. 改下面的 WECOM_WEBHOOK_KEY (企业微信群机器人 Webhook 里 key= 后面那段)
 *   4. 改 APN (APN_NAME) 为你 SIM 卡运营商的 APN
 *   5. F7 编译
 *
 * 依赖：你工程原本的 Delay.c 提供 Delay_ms()
 */
#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>

/* ==================== 配置：企业微信群机器人 ==================== *
 * 获取: 企业微信群 -> 右上角 ... -> 群机器人 -> 添加机器人
 *       -> 选「自定义机器人」-> 起名 -> 复制 Webhook 地址
 *       地址形如: https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=693a91f6-xxxx
 *       只需把 key= 后面那一段填到下面即可.
 * 4 个人拉进同一个群即可, 不需要逐个维护 UID.
 */
#define WECOM_WEBHOOK_KEY    "YOUR_WEBHOOK_KEY_HERE"   // 换成你自己的企业微信群机器人 key

/* ==================== 配置：APN ==================== *
 * 移动 CMNET, 联通 UNINET/3GNET, 电信 CTNET
 */
#define APN_NAME             "CMNET"

/* ==================== 配置：阈值 / 时序 ==================== *
 * 电平单位: 0.1 dB SPL, 例如 450 = 45.0 dB, 650 = 65.0 dB
 * SPL_OFFSET_DB 是校准偏移(单位 0.1dB), 换麦克风/改增益后要重新校准.
 */
#define SPL_OFFSET_DB       300     /* 84.0 dB 满量程, 校准后修改这里 */
#define THRESH_LOW          150     /* 【极限探测】15.0 dB: 只要有一点信号就会离开绿灯 */
#define THRESH_HIGH         200     /* 【极限探测】20.0 dB */
#define ALERT_COOLDOWN_MS   30000u
#define MAIN_LOOP_MS        10u
#define YELLOW_TOGGLE_MS    200u
#define ADC_SAMPLE_COUNT    128u

/* ==================== 配置：麦克风 ==================== */
#define MIC_COUNT           2u      /* 麦克风路数 */

/* ==================== 外部依赖 ==================== */
extern void Delay_ms(uint32_t ms);

/* ==================== 故障诊断：红灯闪码 ==================== *
 * 没有串口也能知道卡在哪一步: 红灯闪 N 次, 停一下, 再循环.
 *   1 = AT 无响应       -> 串口接错 / M100 没供电 / 波特率不对
 *   2 = SIM 卡未识别    -> 卡没插好 / 卡座方向反 / 卡欠费停机
 *   3 = 网络未注册      -> 天线没接 / 信号差 / 卡未激活 / APN 不对
 *   4 = HTTP 初始化失败 -> 模组固件不支持 AT+HTTP 系列
 *   5 = 发 body 被拒    -> 没等到 DOWNLOAD 提示符
 *   6 = HTTP 请求未完成 -> 网络中断 / DNS 解析失败
 *   7 = 服务器返回非200 -> Webhook key 错 / 机器人类型不对
 * 上电初始化失败时闪 3 轮; 每次发送失败也会闪一次.
 */

/* ==================== 内部工具：LED ==================== *
 *   绿(LED1) = PA4, 黄(LED2) = PB0, 红(LED3) = PB1
 *
 * 点亮电平由 LED_ON_LEVEL 决定, 板子实测是低电平点亮(共阳, 阳极接 3.3V):
 *   0 = 低电平点亮 (阳极接 3.3V, 阴极经 220R 到 GPIO)
 *   1 = 高电平点亮 (GPIO 经 220R 到阳极, 阴极接 GND)
 * 若灯的逻辑又反了, 只改下面这一行即可.
 */
#define LED_ON_LEVEL      0

#define LED_GREEN_PORT    GPIOA
#define LED_GREEN_PIN     GPIO_Pin_4
#define LED_YELLOW_PORT   GPIOB
#define LED_YELLOW_PIN    GPIO_Pin_0
#define LED_RED_PORT      GPIOB
#define LED_RED_PIN       GPIO_Pin_1

static void LED_Write(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
    if (on) {
#if LED_ON_LEVEL
        GPIO_SetBits(port, pin);
#else
        GPIO_ResetBits(port, pin);
#endif
    } else {
#if LED_ON_LEVEL
        GPIO_ResetBits(port, pin);
#else
        GPIO_SetBits(port, pin);
#endif
    }
}

static void LED_AllOff(void)
{
    LED_Write(LED_GREEN_PORT,  LED_GREEN_PIN,  0);
    LED_Write(LED_YELLOW_PORT, LED_YELLOW_PIN, 0);
    LED_Write(LED_RED_PORT,    LED_RED_PIN,    0);
}
static void LED_SetGreen(uint8_t on)  { LED_Write(LED_GREEN_PORT,  LED_GREEN_PIN,  on); }
static void LED_SetYellow(uint8_t on) { LED_Write(LED_YELLOW_PORT, LED_YELLOW_PIN, on); }
static void LED_SetRed(uint8_t on)    { LED_Write(LED_RED_PORT,    LED_RED_PIN,    on); }

static void LED_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_2MHz;

    GPIO_InitStruct.GPIO_Pin   = LED_GREEN_PIN;
    GPIO_Init(LED_GREEN_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin   = LED_YELLOW_PIN | LED_RED_PIN;
    GPIO_Init(LED_YELLOW_PORT, &GPIO_InitStruct);

    LED_AllOff();
}

/* 红灯闪 n 次表示故障码, 结束时停顿 1.5 秒方便区分轮次 */
/* 故障闪码: 红灯慢闪 n 次, 节奏放慢到 0.5s 亮/0.5s 灭, 方便数清楚 */
static void LED_BlinkCode(uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        LED_AllOff();
        LED_SetRed(1);
        Delay_ms(500);
        LED_AllOff();
        Delay_ms(500);
    }
    Delay_ms(2000);
}

/* 绿灯慢闪 n 次: 用于表示"发送成功" */
static void LED_GreenBlink(uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++) {
        LED_AllOff();
        LED_SetGreen(1);
        Delay_ms(400);
        LED_AllOff();
        Delay_ms(400);
    }
    Delay_ms(800);
}

/* 上电 LED 自检: 绿 -> 黄 -> 红 依次点亮 0.3 秒, 目视确认三个灯与接线 */
static void LED_SelfTest(void)
{
    LED_AllOff(); LED_SetGreen(1);  Delay_ms(300);
    LED_AllOff(); LED_SetYellow(1); Delay_ms(300);
    LED_AllOff(); LED_SetRed(1);    Delay_ms(300);
    LED_AllOff();
}

/* 自检通过提示: 三灯同时齐闪 n 次 */
static void LED_BlinkAll(uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        LED_AllOff();
        LED_SetGreen(1); LED_SetYellow(1); LED_SetRed(1);
        Delay_ms(200);
        LED_AllOff();
        Delay_ms(300);
    }
    Delay_ms(600);
}

/* ==================== 内部工具：ADC (2 路麦克风) ==================== */
/* 引脚 -> ADC 通道映射表. 改线序只需改这张表. */
static const uint8_t mic_adc_ch[MIC_COUNT] = {
    ADC_Channel_0,      /* PA0 -> 麦克风 1 */
    ADC_Channel_1       /* PA1 -> 麦克风 2 */
};

static void ADC_Mic_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    ADC_InitTypeDef  ADC_InitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);

    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AIN;
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    ADC_InitStruct.ADC_Mode               = ADC_Mode_Independent;
    ADC_InitStruct.ADC_ScanConvMode       = DISABLE;   /* 手动切通道, 不用扫描 */
    ADC_InitStruct.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStruct.ADC_ExternalTrigConv   = ADC_ExternalTrigConv_None;
    ADC_InitStruct.ADC_DataAlign          = ADC_DataAlign_Right;
    ADC_InitStruct.ADC_NbrOfChannel       = 1;
    ADC_Init(ADC1, &ADC_InitStruct);

    ADC_Cmd(ADC1, ENABLE);
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1)) { }
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1)) { }
}

static uint16_t ADC_ReadChannel(uint8_t ch)
{
    ADC_RegularChannelConfig(ADC1, ch, 1, ADC_SampleTime_55Cycles5);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET) { }
    return (uint16_t)ADC_GetConversionValue(ADC1);
}

/* 采集一轮, 输出各路的均方值(信号功率), 已用慢速跟踪去掉直流偏置 */
static void Mic_ReadAll(uint32_t *ms)
{
    static int32_t dc[MIC_COUNT] = {2048, 2048};   /* 直流跟踪, 上电按 Vcc/2 起 */
    uint32_t acc[MIC_COUNT];
    uint16_t i;
    uint8_t  c;

    for (c = 0; c < MIC_COUNT; c++) acc[c] = 0;

    for (i = 0; i < ADC_SAMPLE_COUNT; i++) {
        for (c = 0; c < MIC_COUNT; c++) {
            int32_t v  = (int32_t)ADC_ReadChannel(mic_adc_ch[c]);
            int32_t ac = v - dc[c];

            dc[c] += ac / 64;                  /* 一阶高通, 约 1~2Hz 以下滤掉 */
            acc[c] += (uint32_t)(ac * ac);     /* 最大 128*2048^2, 不会溢出 */
        }
    }

    for (c = 0; c < MIC_COUNT; c++) {
        ms[c] = acc[c] / ADC_SAMPLE_COUNT;
    }
}

/* 麦克风自检: 读各路基线直流, 判断是否断路或饱和.
 * MAX4466 正常输出基线约 Vcc/2, ADC 读数应在 1000~3000 之间.
 * 返回位图: bit0 = 麦1 异常, bit1 = 麦2 异常, 0 = 全部正常.
 */
static uint8_t Mic_SelfTest(void)
{
    uint32_t sum[MIC_COUNT];
    uint16_t i;
    uint8_t  c, bad = 0;

    for (c = 0; c < MIC_COUNT; c++) sum[c] = 0;

    for (i = 0; i < 64; i++) {
        for (c = 0; c < MIC_COUNT; c++) {
            sum[c] += ADC_ReadChannel(mic_adc_ch[c]);
        }
    }

    for (c = 0; c < MIC_COUNT; c++) {
        uint32_t avg = sum[c] / 64;
        /* <60: 引脚被拉地/没供电;  >4035: 输出顶到 Vcc, 增益过大或短路到电源 */
        if (avg < 60UL || avg > 4035UL) {
            bad |= (uint8_t)(1u << c);
        }
    }
    return bad;
}

/* ==================== 内部工具：房间整体噪音水平 ==================== *
 * 定量算法: 均方值 -> RMS -> dBFS -> dB SPL
 *   dBFS = 20*log10(rms / 2048)       (12bit ADC, 2048 = 满量程)
 *   dB SPL = dBFS + SPL_OFFSET_DB     (偏移靠校准得到)
 * 全程用整数定点, 不引入浮点库, 适配无 FPU 的 F103.
 */
static uint32_t mic_ms_smooth[MIC_COUNT];   /* 各路平滑后的均方值 */

/* 整数平方根 */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 0x40000000UL, t;

    while (b > x) b >>= 2;
    while (b) {
        t = r + b;
        r >>= 1;
        if (x >= t) {
            x -= t;
            r += b;
        }
        b >>= 2;
    }
    return r;
}

/* log2(x) 的 8.8 定点值, x>=1; 尾数线性插值, 误差约 0.26dB */
static uint16_t Log2Q8(uint32_t x)
{
    uint8_t  n = 0;
    uint32_t y = x, p, frac;

    if (x == 0) return 0;
    while (y >= 2u) {
        y >>= 1;
        n++;
    }
    p    = 1UL << n;                       /* 2^n <= x < 2^(n+1) */
    frac = ((x - p) * 256UL) / p;          /* 小数部分 0~255 */
    return (uint16_t)((n << 8) + frac);
}

/* 均方值 -> dB SPL (单位 0.1dB) */
static int16_t Spl10FromMs(uint32_t msq)
{
    uint32_t rms = isqrt32(msq);
    int32_t  l2, db10;

    if (rms == 0) return (int16_t)(SPL_OFFSET_DB - 700);   /* 静音下限 */

    l2   = (int32_t)Log2Q8(rms);                 /* 8.8 定点 */
    /* 20*log10(rms/2048) = 6.0206*(log2(rms)-11), 换成 0.1dB 单位 */
    db10 = (l2 - (11L << 8)) * 2352L / 10000L;

    return (int16_t)(db10 + SPL_OFFSET_DB);
}

/**
 * 各路取最大值作为房间整体噪音水平.
 * 用最大值而非平均值: 只要房间任一角落超标就告警, 避免位置盲区导致漏报.
 * 平滑放在"功率域"做(先平均功率再取对数), 才是正确的声级平均方式.
 * 系数 7/8 约等于 80ms 时间常数, 接近声级计的 Fast 档.
 */
static int16_t Mic_RoomLevel(const uint32_t *ms)
{
    uint32_t lvl = 0;
    uint8_t  c;

    for (c = 0; c < MIC_COUNT; c++) {
        mic_ms_smooth[c] = (mic_ms_smooth[c] * 7UL + ms[c]) / 8UL;
        if (mic_ms_smooth[c] > lvl) lvl = mic_ms_smooth[c];
    }
    return Spl10FromMs(lvl);
}

/* ==================== USART1 (M100 串口) ==================== *
 * PA9=TX -> M100 RXD,  PA10=RX <- M100 TXD
 * 115200, 8N1, 双向
 */
static void USART_M100_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStruct;
    USART_InitTypeDef USART_InitStruct;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_9;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_10;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    USART_InitStruct.USART_BaudRate            = 115200;
    USART_InitStruct.USART_WordLength          = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits            = USART_StopBits_1;
    USART_InitStruct.USART_Parity              = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &USART_InitStruct);
    USART_Cmd(USART1, ENABLE);
}

static void UART_SendByte(uint8_t b)
{
    USART_SendData(USART1, (uint16_t)b);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
}

static void UART_SendString(const char *s)
{
    while (*s) UART_SendByte((uint8_t)*s++);
}

/* ==================== 调试串口 (USART3) ==================== *
 * PB10=TX, PB11=RX. 接 USB转TTL 到电脑, 串口助手 115200 看各路实时电平.
 * 用途: 验证麦克风信号是否干净、各路读数是否稳定.
 * 正式部署时把 DEBUG_UART 改成 0 即可 (省代码、省功耗).
 *
 * 输出格式示例:
 *   rms= 123  118 | SPL= 52.3 dB
 *   rms = 本轮各路均方根   SPL = 房间整体声压级(0.1dB 精度)
 */
#define DEBUG_UART          1
#define DEBUG_PERIOD_MS     500u

#if DEBUG_UART
static void DEBUG_SendByte(uint8_t b)
{
    USART_SendData(USART3, (uint16_t)b);
    while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET) { }
}

static void DEBUG_SendString(const char *s)
{
    while (*s) DEBUG_SendByte((uint8_t)*s++);
}

static void DEBUG_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStruct;
    USART_InitTypeDef USART_InitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_10;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_11;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    USART_InitStruct.USART_BaudRate            = 115200;
    USART_InitStruct.USART_WordLength          = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits            = USART_StopBits_1;
    USART_InitStruct.USART_Parity              = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART3, &USART_InitStruct);
    USART_Cmd(USART3, ENABLE);
}

static void DEBUG_Print(const uint32_t *ms, int16_t spl10)
{
    char    line[80];
    int     n = 0;
    uint8_t c;
    char    sign = (spl10 < 0) ? '-' : ' ';
    unsigned a = (unsigned)((spl10 < 0) ? -spl10 : spl10);

    /* 循环拼接各路 RMS, 路数由 MIC_COUNT 决定, 不写死下标 */
    n += snprintf(line + n, (size_t)(sizeof(line) - (size_t)n), "rms=");
    for (c = 0; c < MIC_COUNT; c++) {
        if (n >= (int)sizeof(line) - 1) break;
        n += snprintf(line + n, (size_t)(sizeof(line) - (size_t)n),
                      "%4u ", (unsigned)isqrt32(ms[c]));
    }
    if (n < (int)sizeof(line) - 1) {
        snprintf(line + n, (size_t)(sizeof(line) - (size_t)n),
                 "| SPL=%c%u.%u dB\r\n", sign, a / 10u, a % 10u);
    }
    DEBUG_SendString(line);
}
#endif

/* ==================== AT 指令工具 ==================== *
 * 发送一条 AT 指令并等待响应中包含 expect 字符串.
 * 返回 1=成功, 0=超时或没匹配到.
 * cmd 传 NULL 时不发送任何内容, 只等待响应 (用于等模组自发的 OK).
 */
#define AT_BUF_SIZE  256
static char at_buf[AT_BUF_SIZE];

static uint8_t AT_Cmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    uint16_t idx = 0;
    uint32_t elapsed = 0;

    /* 发指令 (cmd 为 NULL 则跳过, 只等响应)
     * 注意: 只有发新指令前才清空 RX. cmd==NULL 表示"等模组自发响应",
     *       此时响应可能已经到达缓冲, 若清空会把 OK 吃掉, 导致永远等不到.
     */
    if (cmd != NULL) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            USART_ReceiveData(USART1);
        }
        UART_SendString(cmd);
        UART_SendByte('\r');
        UART_SendByte('\n');
#if DEBUG_UART
        DEBUG_SendString("\r\n>> ");
        DEBUG_SendString(cmd);
#endif
    }

    /* 收响应 */
    while (elapsed < timeout_ms) {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            uint8_t b = (uint8_t)USART_ReceiveData(USART1);
#if DEBUG_UART
            DEBUG_SendByte(b);      /* M100 原始响应透传到调试串口 */
#endif
            if (idx < AT_BUF_SIZE - 1) {
                at_buf[idx++] = (char)b;
                at_buf[idx]   = '\0';
            }
            if (strstr(at_buf, expect) != NULL) {
                return 1;
            }
        } else {
            Delay_ms(1);
            elapsed++;
        }
    }
    return 0;
}

/* 匹配到 expect 后, 继续把剩下半行读完(读到 \n 为止), 便于解析状态码 */
static void AT_ReadRest(uint32_t timeout_ms)
{
    uint16_t idx = (uint16_t)strlen(at_buf);
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            uint8_t b = (uint8_t)USART_ReceiveData(USART1);
#if DEBUG_UART
            DEBUG_SendByte(b);
#endif
            if (idx < AT_BUF_SIZE - 1) {
                at_buf[idx++] = (char)b;
                at_buf[idx]   = '\0';
            }
            if (b == '\n') return;
        } else {
            Delay_ms(1);
            elapsed++;
        }
    }
}

/* 从 at_buf 里解析 +HTTPACTION: 1,<status>,<len> 的 status; 解析不到返回 0 */
static uint16_t HTTP_GetStatus(void)
{
    const char *p = strstr(at_buf, "+HTTPACTION: 1,");

    if (p == NULL) return 0;
    p += 15;   /* 跳过 "+HTTPACTION: 1," 共 15 个字符 */
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') {
        return 0;
    }
    return (uint16_t)((p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0'));
}

/* 解析企业微信返回 body 里的业务码 errcode.
 * 成功: {"errcode":0,"errmsg":"ok"}
 * 失败: {"errcode":93000,"errmsg":"invalid webhook url, ..."} (key 错)
 *       {"errcode":45009,"errmsg":"api freq out of limit"}   (频率超限)
 * 注意: 这些失败 HTTP 状态码仍是 200, 所以必须看 errcode, 否则会"假成功".
 * 只有 errcode 为 0 才算真成功; 找不到 errcode 也视为失败(保险).
 */
static uint8_t WeCom_IsSuccess(void)
{
    const char *p = strstr(at_buf, "\"errcode\":");
    if (p == NULL) return 0;
    p += 10;                 /* 跳过 "\"errcode\":" 共 10 个字符 */
    while (*p == ' ') p++;   /* 跳过可能的空格 */
    return (uint8_t)(*p == '0');
}

/* ==================== M100 初始化 ==================== *
 * 银尔达 M100M-C2 是合宙 Air780E 核心板, 跑合宙 AT 固件.
 * 不需要提前配 HTTPD, 每次发告警时走标准 HTTP AT 指令即可.
 *
 * 初始化流程:
 *   AT                        测试通信
 *   ATE0                      关闭回显
 *   AT+CPIN?                  检查 SIM 卡 (期望 +CPIN: READY)
 *   AT+CGDCONT=1,"IP","APN"   设置 APN
 *   AT+CGREG?                 等待注册网络 (期望 +CGREG: 0,1 或 0,5)
 */
static uint8_t M100_Init(void)
{
    char cmd[64];
    uint8_t try;

    /* 等模组上电稳定 */
    Delay_ms(3000);

    /* 1. 测试通信 */
    if (!AT_Cmd("AT", "OK", 2000)) {
        return 1;               /* 串口不通 */
    }

    /* 2. 关闭回显 */
    AT_Cmd("ATE0", "OK", 1000);

    /* 3. 检查 SIM 卡 */
    if (!AT_Cmd("AT+CPIN?", "READY", 3000)) {
        return 2;               /* SIM 未识别 */
    }

    /* 4. 设置 APN */
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN_NAME);
    AT_Cmd(cmd, "OK", 2000);

    /* 5. 等待注册网络 (最多 30 秒)
     *    0,1 = 已注册本地网络; 0,5 = 已注册漫游
     */
    for (try = 0; try < 30; try++) {
        if (AT_Cmd("AT+CGREG?", "CGREG: 0,1", 1000) ||
            AT_Cmd("AT+CGREG?", "CGREG: 0,5", 1000)) {
            return 0;
        }
        Delay_ms(1000);
    }
    return 3;                   /* 网络未注册 */
}

/* ==================== 发企业微信群告警 ==================== *
 * 走合宙 Air780E 标准 HTTP AT 指令发 HTTPS POST.
 *
 * 流程:
 *   AT+HTTPINIT                                   初始化 HTTP 服务
 *   AT+HTTPSSL=1                                  开启 SSL (HTTPS)
 *   AT+HTTPPARA="URL","https://..."               设置 URL
 *   AT+HTTPPARA="CONTENT","application/json"      设置 Content-Type
 *   AT+HTTPDATA=<len>,10000                       通知模组要传 len 字节, 回 ">"
 *   <发送 JSON body len 字节>                     模组收到后回 OK
 *   AT+HTTPACTION=1                               执行 POST, 回 +HTTPACTION: 1,200,n
 *   AT+HTTPREAD                                   读响应 (可选)
 *   AT+HTTPTERM                                   关闭 HTTP 服务
 *
 * 消息不含声源位置信息, 只告知"有噪音", 避免点名引发矛盾.
 */

/* JSON body 缓冲: 企业微信文本消息, 300 字节足够 */
#define WX_BODY_SIZE  300
static char wx_body[WX_BODY_SIZE];

/* 返回 0=发送成功; 非 0 = 失败步骤(对应红灯闪码) */
static uint8_t Send_NoiseAlert(int16_t spl10)
{
    char     cmd[160];
    uint16_t body_len;
    uint16_t i;
    uint8_t  ret = 0;
    unsigned a = (unsigned)((spl10 < 0) ? -spl10 : spl10);

    /* 1. 组装 JSON body (企业微信文本消息, 中文用 \uXXXX 转义, 避免 Keil GBK 编码问题) */
    body_len = (uint16_t)snprintf(wx_body, sizeof(wx_body),
        "{\"msgtype\":\"text\",\"text\":{\"content\":\""
        "\\u26a0\\ufe0f \\u566a\\u97f3\\u8fc7\\u5927\\u544a\\u8b66\\n"
        "\\u97f3\\u91cf: %u.%u dB\\n"
        "\\u6765\\u6e90: STM32 \\u76d1\\u6d4b\\u8282\\u70b9\\u81ea\\u52a8\\u89e6\\u53d1"
        "\"}}",
        a / 10u, a % 10u);

    if (body_len == 0 || body_len >= sizeof(wx_body)) {
        return 4;   /* 缓冲不够或出错, 放弃 */
    }

    /* 1.5 先确认模组在线, 掉线(没供电/串口断了)直接报 1 */
    if (!AT_Cmd("AT", "OK", 2000)) return 1;

    /* 2. 初始化 HTTP */
    if (!AT_Cmd("AT+HTTPINIT", "OK", 3000)) return 4;

    /* 3. 开启 SSL */
    AT_Cmd("AT+HTTPSSL=1", "OK", 2000);

    /* 4. 设置 URL (企业微信群机器人 Webhook) */
    snprintf(cmd, sizeof(cmd),
        "AT+HTTPPARA=\"URL\",\"https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key="
        WECOM_WEBHOOK_KEY "\"");
    if (!AT_Cmd(cmd, "OK", 2000)) { ret = 4; goto http_exit; }

    /* 5. 设置 Content-Type */
    AT_Cmd("AT+HTTPPARA=\"CONTENT\",\"application/json\"", "OK", 2000);

    /* 6. 通知模组准备接收 body
     *    合宙 Air780E 回 "DOWNLOAD" (不是 SIM800 那种 ">")
     */
    snprintf(cmd, sizeof(cmd), "AT+HTTPDATA=%u,10000", (unsigned)body_len);
    if (!AT_Cmd(cmd, "DOWNLOAD", 5000)) { ret = 5; goto http_exit; }

    /* 7. 发送 body 内容 (精确 body_len 字节, 不含结尾 \0) */
    for (i = 0; i < body_len; i++) {
        UART_SendByte((uint8_t)wx_body[i]);
    }

    /* 8. 等模组收完数据回 OK */
    if (!AT_Cmd(NULL, "OK", 5000)) { ret = 5; goto http_exit; }

    /* 9. 执行 POST (最多等 15 秒, 4G 建链 + 服务端响应) */
    if (!AT_Cmd("AT+HTTPACTION=1", "+HTTPACTION: 1,", 15000)) { ret = 6; goto http_exit; }

    /* 10. 把这一行读完, 取出 HTTP 状态码; 不是 200 说明服务器拒了 */
    AT_ReadRest(2000);
    if (HTTP_GetStatus() != 200) ret = 7;

    /* 11. 读取响应 body 并检查企业微信业务码 errcode.
     *     关键: key 错误 / 频率超限时 HTTP 仍是 200, 但 errcode != 0,
     *     消息不会投递. 所以必须看 errcode, 不能只看 HTTP 状态码,
     *     否则会出现"绿灯亮了(以为成功)但群里没消息"的假成功. */
    AT_Cmd("AT+HTTPREAD", "OK", 5000);
    if (ret == 0 && !WeCom_IsSuccess()) {
        ret = 7;   /* HTTP 200 但 errcode != 0: key 错 / 机器人类型不对 / 频率受限 */
    }

http_exit:
    /* 12. 关闭 HTTP 服务, 释放资源 */
    AT_Cmd("AT+HTTPTERM", "OK", 3000);

    return ret;
}

/* ==================== main ==================== */
int main(void)
{
    uint32_t sys_tick         = 0;
    uint32_t yellow_tick      = 0;
    uint32_t last_alert_tick  = 0;
    uint8_t  first_alert_done = 0;
    uint8_t  yellow_state     = 0;
    uint8_t  m100_err         = 0;
    uint8_t  mic_bad          = 0;
#if DEBUG_UART
    uint32_t debug_tick       = 0;
#endif

    /* 1) 初始化外设 */
    LED_GPIO_Init();
    ADC_Mic_Init();
    USART_M100_Init();
#if DEBUG_UART
    DEBUG_Init();
#endif

    /* 2) 上电自检
     *    四步: LED 逐灯点亮 -> 麦克风基线 -> 4G 握手续网 -> 汇总结果
     *    黄灯常亮表示"正在联网自检", 避免误认为死机.
     */
    LED_SelfTest();                    /* 绿->黄->红 依次亮 0.3s, 目视确认 */

    mic_bad = Mic_SelfTest();          /* 位图: bit0=麦1 异常, bit1=麦2 异常 */
#if DEBUG_UART
    DEBUG_SendString("\r\n[SELF] mic1=");
    DEBUG_SendString((mic_bad & 0x01) ? "BAD" : "OK");
    DEBUG_SendString(" mic2=");
    DEBUG_SendString((mic_bad & 0x02) ? "BAD" : "OK");
    DEBUG_SendString("\r\n");
#endif

    LED_AllOff();
    LED_SetYellow(1);                  /* 黄灯常亮: 正在与 4G 模组握手 */
    m100_err = M100_Init();
    LED_AllOff();

#if DEBUG_UART
    DEBUG_SendString("[SELF] m100=");
    DEBUG_SendByte((uint8_t)('0' + m100_err));
    DEBUG_SendString(m100_err ? " FAIL\r\n" : " OK\r\n");
#endif

    /* 3) 汇总自检结果 (没有串口也能看)
     *    麦克风异常 -> 红灯闪 8 次; 模组异常 -> 闪 1~7 次;
     *    全部通过   -> 三灯同时齐闪 2 次.
     */
    if (mic_bad) {
        uint8_t r;
        for (r = 0; r < 3; r++) {
            LED_BlinkCode(8);
        }
    } else if (m100_err) {
        uint8_t r;
        for (r = 0; r < 3; r++) {
            LED_BlinkCode(m100_err);
        }
    } else {
        LED_BlinkAll(2);

        /* 开机强制测试推送: 不管当前噪声, 主动发一条, 验证整条 4G->服务器链路
         * 成功 -> 绿灯慢闪 3 次; 失败 -> 全灭 2 秒后红闪 N 次, 重复 3 轮
         * 这样不用制造噪声、不用串口, 上电 30 秒内就能知道卡在哪一步.
         */
        {
            uint8_t r;
            m100_err = Send_NoiseAlert(650);   /* 650 = 65.0 dB 测试值 */
            if (m100_err == 0) {
                LED_GreenBlink(3);            /* 成功: 绿灯闪 3 次 */
            } else {
                for (r = 0; r < 3; r++) {
                    LED_AllOff();
                    Delay_ms(2000);           /* 先灭 2 秒, 与常亮红灯区分 */
                    LED_BlinkCode(m100_err);  /* 再红闪 N 次 */
                }
            }
        }
    }

    /* 4) 主循环 */
    while (1) {
        uint32_t ms[MIC_COUNT];
        int16_t  level;

        Mic_ReadAll(ms);
        level = Mic_RoomLevel(ms);

#if DEBUG_UART
        /* 每 500ms 打印一次各路实时数据, 用于验证信号质量 */
        if ((sys_tick - debug_tick) >= DEBUG_PERIOD_MS) {
            debug_tick = sys_tick;
            DEBUG_Print(ms, level);
        }
#endif

        /* ---- LED 指示 ---- */
        if (level < THRESH_LOW) {
            LED_AllOff();
            LED_SetGreen(1);            /* 安静: 绿灯常亮 */
        } else if (level < THRESH_HIGH) {
            if ((sys_tick - yellow_tick) >= YELLOW_TOGGLE_MS) {
                yellow_state = !yellow_state;
                yellow_tick  = sys_tick;
            }
            LED_AllOff();
            LED_SetYellow(yellow_state);
        } else {
            LED_AllOff();
            LED_SetRed(1);
        }

        /* ---- 告警推送 ---- */
        if (level >= THRESH_HIGH) {
            if (!first_alert_done ||
                (sys_tick - last_alert_tick) >= ALERT_COOLDOWN_MS) {
                /* 开始推送: 三灯齐闪 1 次, 表示"正在发送"(约 5~20 秒) */
                LED_BlinkAll(1);

                m100_err = Send_NoiseAlert(level);
                last_alert_tick  = sys_tick;
                first_alert_done = 1;

                if (m100_err == 0) {
                    /* 发送成功: 绿灯慢闪 3 次 */
                    LED_GreenBlink(3);
                } else {
                    /* 发送失败: 先全灭 2 秒, 与"常亮红灯"区分开, 再闪码 */
                    LED_AllOff();
                    Delay_ms(2000);
                    LED_BlinkCode(m100_err);
                }
            }
        }

        Delay_ms(MAIN_LOOP_MS);
        sys_tick += MAIN_LOOP_MS;
    }
}
