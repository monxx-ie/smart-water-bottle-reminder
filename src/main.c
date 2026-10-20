#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t uart_mutex;
static TaskHandle_t h_led, h_btn, h_sens, h_mon, h_buz;
static RTC_HandleTypeDef hrtc;
static int rtc_src_lse = 0;

extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

void vApplicationMallocFailedHook(void) { for (;;); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    (void)t; (void)name;
    for (;;);
}

/* ---------- UART logging ---------- */
static void uart_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &g);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

static void log_printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;

    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 100);
    xSemaphoreGive(uart_mutex);
}

/* prints a value stored in tenths as "-12.3": use with "%s%d.%d" */
#define F1(v) (((v) < 0) ? "-" : ""), (int)(abs(v) / 10), (int)(abs(v) % 10)

/* ---------- microsecond delay (DWT cycle counter) ---------- */
static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) {}
}

/* ---------- software I2C (bit-banged) ---------- */
typedef struct {
    GPIO_TypeDef *scl_port; uint16_t scl_pin;
    GPIO_TypeDef *sda_port; uint16_t sda_pin;
} i2c_bus_t;

/* bus A: ICM20608 (0x68), AP3216C (0x1E), codec (0x10)   bus B: AHT10 (0x38) */
static const i2c_bus_t bus_a = { GPIOF, GPIO_PIN_1, GPIOF, GPIO_PIN_0 };
static const i2c_bus_t bus_b = { GPIOE, GPIO_PIN_0, GPIOE, GPIO_PIN_1 };

#define SCL(b, v)  HAL_GPIO_WritePin((b)->scl_port, (b)->scl_pin, (v) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define SDA(b, v)  HAL_GPIO_WritePin((b)->sda_port, (b)->sda_pin, (v) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define SDA_READ(b) (HAL_GPIO_ReadPin((b)->sda_port, (b)->sda_pin) == GPIO_PIN_SET)
#define T_US 5

static void i2c_init(const i2c_bus_t *b)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = b->scl_pin; HAL_GPIO_Init(b->scl_port, &g);
    g.Pin = b->sda_pin; HAL_GPIO_Init(b->sda_port, &g);
    SCL(b, 1);
    SDA(b, 1);
}

static void i2c_start(const i2c_bus_t *b)
{
    SDA(b, 1); SCL(b, 1); delay_us(T_US);
    SDA(b, 0); delay_us(T_US);
    SCL(b, 0); delay_us(T_US);
}

static void i2c_stop(const i2c_bus_t *b)
{
    SDA(b, 0); delay_us(T_US);
    SCL(b, 1); delay_us(T_US);
    SDA(b, 1); delay_us(T_US);
}

static int i2c_write(const i2c_bus_t *b, uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        SDA(b, (byte >> i) & 1);
        delay_us(T_US);
        SCL(b, 1); delay_us(T_US);
        SCL(b, 0);
    }
    SDA(b, 1); delay_us(T_US);
    SCL(b, 1); delay_us(T_US);
    int ack = !SDA_READ(b);
    SCL(b, 0); delay_us(T_US);
    return ack;
}

static uint8_t i2c_read_byte(const i2c_bus_t *b, int ack)
{
    uint8_t v = 0;
    SDA(b, 1);
    for (int i = 0; i < 8; i++) {
        delay_us(T_US);
        SCL(b, 1); delay_us(T_US);
        v = (uint8_t)((v << 1) | (SDA_READ(b) ? 1 : 0));
        SCL(b, 0);
    }
    SDA(b, ack ? 0 : 1);
    delay_us(T_US);
    SCL(b, 1); delay_us(T_US);
    SCL(b, 0); delay_us(T_US);
    SDA(b, 1);
    return v;
}

static int i2c_send(const i2c_bus_t *b, uint8_t addr, const uint8_t *d, uint8_t n)
{
    i2c_start(b);
    if (!i2c_write(b, (uint8_t)(addr << 1))) { i2c_stop(b); return -1; }
    for (uint8_t i = 0; i < n; i++) {
        if (!i2c_write(b, d[i])) { i2c_stop(b); return -1; }
    }
    i2c_stop(b);
    return 0;
}

static int i2c_recv(const i2c_bus_t *b, uint8_t addr, uint8_t *buf, uint8_t n)
{
    i2c_start(b);
    if (!i2c_write(b, (uint8_t)((addr << 1) | 1))) { i2c_stop(b); return -1; }
    for (uint8_t i = 0; i < n; i++) {
        buf[i] = i2c_read_byte(b, i < (uint8_t)(n - 1));
    }
    i2c_stop(b);
    return 0;
}

static int i2c_mem_write(const i2c_bus_t *b, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t d[2] = { reg, val };
    return i2c_send(b, addr, d, 2);
}

static int i2c_mem_read(const i2c_bus_t *b, uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n)
{
    i2c_start(b);
    if (!i2c_write(b, (uint8_t)(addr << 1))) { i2c_stop(b); return -1; }
    if (!i2c_write(b, reg)) { i2c_stop(b); return -1; }
    i2c_start(b);
    if (!i2c_write(b, (uint8_t)((addr << 1) | 1))) { i2c_stop(b); return -1; }
    for (uint8_t i = 0; i < n; i++) {
        buf[i] = i2c_read_byte(b, i < (uint8_t)(n - 1));
    }
    i2c_stop(b);
    return 0;
}

/* ---------- ICM20608 IMU (bus A, 0x68) ---------- */
#define ICM_ADDR 0x68

static int imu_init(void)
{
    uint8_t id = 0;
    if (i2c_mem_read(&bus_a, ICM_ADDR, 0x75, &id, 1) != 0) return -1;
    log_printf("ICM20608 WHO_AM_I=0x%02X\r\n", id);
    i2c_mem_write(&bus_a, ICM_ADDR, 0x6B, 0x80);      /* reset */
    vTaskDelay(pdMS_TO_TICKS(100));
    i2c_mem_write(&bus_a, ICM_ADDR, 0x6B, 0x01);      /* auto clock */
    i2c_mem_write(&bus_a, ICM_ADDR, 0x6C, 0x00);      /* all axes on */
    i2c_mem_write(&bus_a, ICM_ADDR, 0x1B, 0x08);      /* gyro +-500 dps */
    i2c_mem_write(&bus_a, ICM_ADDR, 0x1C, 0x08);      /* accel +-4 g */
    i2c_mem_write(&bus_a, ICM_ADDR, 0x1D, 0x03);      /* accel low-pass */
    i2c_mem_write(&bus_a, ICM_ADDR, 0x1A, 0x03);      /* gyro low-pass */
    return (id == 0xAF || id == 0xAE) ? 0 : -2;
}

static int imu_read(int16_t a[3], int16_t g[3], int16_t *t)
{
    uint8_t r[14];
    if (i2c_mem_read(&bus_a, ICM_ADDR, 0x3B, r, 14) != 0) return -1;
    a[0] = (int16_t)((r[0] << 8) | r[1]);
    a[1] = (int16_t)((r[2] << 8) | r[3]);
    a[2] = (int16_t)((r[4] << 8) | r[5]);
    *t   = (int16_t)((r[6] << 8) | r[7]);
    g[0] = (int16_t)((r[8] << 8) | r[9]);
    g[1] = (int16_t)((r[10] << 8) | r[11]);
    g[2] = (int16_t)((r[12] << 8) | r[13]);
    return 0;
}

/* ---------- AHT10/AHT20/AHT21 temperature/humidity (bus B, 0x38) ---------- */
#define AHT_ADDR 0x38

static int aht_init(void)
{
    uint8_t soft = 0xBA;
    i2c_send(&bus_b, AHT_ADDR, &soft, 1);
    vTaskDelay(pdMS_TO_TICKS(40));
    uint8_t cal[3] = { 0xBE, 0x08, 0x00 };
    i2c_send(&bus_b, AHT_ADDR, cal, 3);        /* AHT20/21 init, result ignored */
    vTaskDelay(pdMS_TO_TICKS(20));
    return i2c_send(&bus_b, AHT_ADDR, cal, 0); /* succeeds if the chip ACKs its address */
}

static int aht_trigger(void)
{
    uint8_t c[3] = { 0xAC, 0x33, 0x00 };
    return i2c_send(&bus_b, AHT_ADDR, c, 3);
}

static int aht_read(int32_t *t_x100, int32_t *h_x100)
{
    uint8_t d[6];
    if (i2c_recv(&bus_b, AHT_ADDR, d, 6) != 0) return -1;
    if (d[0] & 0x80) return -2;                       /* still busy */
    uint32_t hraw = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
    uint32_t traw = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
    *h_x100 = (int32_t)(((uint64_t)hraw * 10000) >> 20);
    *t_x100 = (int32_t)(((uint64_t)traw * 20000) >> 20) - 5000;
    return 0;
}

/* ---------- AP3216C light/proximity (bus A, 0x1E) ---------- */
#define ALS_ADDR 0x1E

static int als_init(void)
{
    if (i2c_mem_write(&bus_a, ALS_ADDR, 0x00, 0x04) != 0) return -1;   /* reset */
    vTaskDelay(pdMS_TO_TICKS(50));
    return i2c_mem_write(&bus_a, ALS_ADDR, 0x00, 0x03);                /* ALS+PS+IR on */
}

static int als_read(uint16_t *als, uint16_t *ps, uint16_t *ir)
{
    uint8_t r[6];
    if (i2c_mem_read(&bus_a, ALS_ADDR, 0x0A, r, 6) != 0) return -1;
    *ir  = (uint16_t)(((uint16_t)r[1] << 2) | (r[0] & 0x03));
    *als = (uint16_t)(((uint16_t)r[3] << 8) | r[2]);
    *ps  = (uint16_t)(((uint16_t)(r[5] & 0x3F) << 4) | (r[4] & 0x0F));
    return 0;
}

/* ---------- Sensor task: IMU + light every 100 ms, AHT every 1 s ---------- */
static void sensor_task(void *arg)
{
    (void)arg;
    i2c_init(&bus_a);
    i2c_init(&bus_b);

    int imu_ok = (imu_init() == 0);
    int aht_ok = (aht_init() == 0);
    int als_ok = (als_init() == 0);
    log_printf("init: IMU=%s AHT10=%s AP3216C=%s\r\n",
               imu_ok ? "OK" : "FAIL", aht_ok ? "OK" : "FAIL", als_ok ? "OK" : "FAIL");

    int16_t acc[3] = {0}, gyr[3] = {0}, traw = 0;
    int32_t t100 = 0, h100 = 0;
    uint16_t als = 0, ps = 0, ir = 0;
    int imu_good = 0, aht_good = 0, als_good = 0;
    int aht_rc = -9;
    uint32_t cycle = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        if (imu_ok) imu_good = (imu_read(acc, gyr, &traw) == 0);
        if (als_ok) als_good = (als_read(&als, &ps, &ir) == 0);
        if (aht_ok) {
            if (cycle % 10 == 0) {
                aht_trigger();
            } else if (cycle % 10 == 1) {
                aht_rc = aht_read(&t100, &h100);
                aht_good = (aht_rc == 0);
            }
        }

        if (cycle % 10 == 2) {
            unsigned long now = (unsigned long)xTaskGetTickCount();
            if (imu_good) {
                int ax = (int)((int32_t)acc[0] * 1000 / 8192);
                int ay = (int)((int32_t)acc[1] * 1000 / 8192);
                int az = (int)((int32_t)acc[2] * 1000 / 8192);
                int32_t gx = (int32_t)gyr[0] * 100 / 655;
                int32_t gy = (int32_t)gyr[1] * 100 / 655;
                int32_t gz = (int32_t)gyr[2] * 100 / 655;
                int32_t tt = (int32_t)traw * 100 / 3268 + 250;
                log_printf("[%lu ms] ACC mg: %d %d %d | GYRO dps: %s%d.%d %s%d.%d %s%d.%d | T=%s%d.%dC\r\n",
                           now, ax, ay, az, F1(gx), F1(gy), F1(gz), F1(tt));
            } else {
                log_printf("[%lu ms] IMU read ERROR\r\n", now);
            }

            if (aht_good) {
                int32_t t10 = t100 / 10, h10 = h100 / 10;
                log_printf("[%lu ms] AHT10: %s%d.%dC %d.%d%%RH\r\n",
                           now, F1(t10), (int)(h10 / 10), (int)(h10 % 10));
            } else {
                log_printf("[%lu ms] AHT10 read ERROR rc=%d init_ok=%d\r\n", now, aht_rc, aht_ok);
            }

            if (als_good) {
                log_printf("[%lu ms] AP3216C: ALS=%u (~%u lux) PS=%u IR=%u\r\n",
                           now, als, (unsigned)((uint32_t)als * 36 / 100), ps, ir);
            } else {
                log_printf("[%lu ms] AP3216C read ERROR\r\n", now);
            }
        }

        cycle++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

/* ---------- Buzzer (PB0): beeps when UP is pressed ---------- */
static void buzzer_task(void *arg)
{
    (void)arg;
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_0;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        log_printf("[%lu ms] buzzer: beep\r\n", (unsigned long)xTaskGetTickCount());
        TickType_t t = xTaskGetTickCount();
        for (int i = 0; i < 300; i++) {            /* ~500 Hz for 300 ms */
            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
            vTaskDelayUntil(&t, 1);
        }
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    }
}

/* ---------- RTC (32.768 kHz crystal, falls back to internal LSI) ---------- */
#define RTC_MAGIC 0x32F2

static int month_from_name(const char *m)
{
    static const char *names = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) {
        if (m[0] == names[i * 3] && m[1] == names[i * 3 + 1] && m[2] == names[i * 3 + 2]) return i + 1;
    }
    return 1;
}

static void rtc_init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    uint32_t src = RCC_RTCCLKSOURCE_LSE;
    rtc_src_lse = 1;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc.LSEState = RCC_LSE_OFF;
        osc.LSIState = RCC_LSI_ON;
        HAL_RCC_OscConfig(&osc);
        src = RCC_RTCCLKSOURCE_LSI;
        rtc_src_lse = 0;
    }
    RCC_PeriphCLKInitTypeDef pc = {0};
    pc.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    pc.RTCClockSelection = src;
    HAL_RCCEx_PeriphCLKConfig(&pc);
    __HAL_RCC_RTC_ENABLE();

    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = rtc_src_lse ? 255 : 249;
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    HAL_RTC_Init(&hrtc);

    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_MAGIC) {
        RTC_TimeTypeDef tm = {0};
        RTC_DateTypeDef dt = {0};
        const char *d = __DATE__;   /* "Oct 20 2026" */
        const char *t = __TIME__;   /* "17:20:05"    */
        tm.Hours   = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
        tm.Minutes = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
        tm.Seconds = (uint8_t)((t[6] - '0') * 10 + (t[7] - '0'));
        dt.Month   = (uint8_t)month_from_name(d);
        dt.Date    = (uint8_t)(((d[4] == ' ') ? 0 : (d[4] - '0')) * 10 + (d[5] - '0'));
        dt.Year    = (uint8_t)((d[9] - '0') * 10 + (d[10] - '0'));
        dt.WeekDay = RTC_WEEKDAY_MONDAY;
        HAL_RTC_SetTime(&hrtc, &tm, RTC_FORMAT_BIN);
        HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN);
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_MAGIC);
    }
}

/* ---------- LEDs ---------- */
static void led_task(void *arg)
{
    (void)arg;
    __HAL_RCC_GPIOF_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &g);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_11, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_12, GPIO_PIN_SET);

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_11 | GPIO_PIN_12);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(500));
    }
}

/* ---------- Buttons ---------- */
static const struct { GPIO_TypeDef *port; uint16_t pin; const char *name; } btns[4] = {
    { GPIOC, GPIO_PIN_0, "LEFT"  },
    { GPIOC, GPIO_PIN_1, "DOWN"  },
    { GPIOC, GPIO_PIN_4, "RIGHT" },
    { GPIOC, GPIO_PIN_5, "UP"    },
};

static void button_task(void *arg)
{
    (void)arg;
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    for (int i = 0; i < 4; i++) {
        g.Pin = btns[i].pin;
        HAL_GPIO_Init(btns[i].port, &g);
    }

    int prev[4] = {1, 1, 1, 1};
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        for (int i = 0; i < 4; i++) {
            int now = HAL_GPIO_ReadPin(btns[i].port, btns[i].pin) == GPIO_PIN_SET;
            if (now != prev[i]) {
                prev[i] = now;
                if (i == 3 && !now) xTaskNotifyGive(h_buz);   /* UP pressed -> beep */
                log_printf("[%lu ms] button %s %s\r\n",
                           (unsigned long)xTaskGetTickCount(), btns[i].name,
                           now ? "released" : "pressed");
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
}

/* ---------- Monitor ---------- */
static void monitor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10000));
        RTC_TimeTypeDef tm;
        RTC_DateTypeDef dt;
        HAL_RTC_GetTime(&hrtc, &tm, RTC_FORMAT_BIN);
        HAL_RTC_GetDate(&hrtc, &dt, RTC_FORMAT_BIN);
        log_printf("RTC 20%02u-%02u-%02u %02u:%02u:%02u (%s)\r\n",
                   (unsigned)dt.Year, (unsigned)dt.Month, (unsigned)dt.Date,
                   (unsigned)tm.Hours, (unsigned)tm.Minutes, (unsigned)tm.Seconds,
                   rtc_src_lse ? "LSE" : "LSI");
        log_printf("[%lu ms] heap free=%u B | HWM: led=%u btn=%u sens=%u mon=%u buz=%u\r\n",
                   (unsigned long)xTaskGetTickCount(),
                   (unsigned)xPortGetFreeHeapSize(),
                   (unsigned)uxTaskGetStackHighWaterMark(h_led),
                   (unsigned)uxTaskGetStackHighWaterMark(h_btn),
                   (unsigned)uxTaskGetStackHighWaterMark(h_sens),
                   (unsigned)uxTaskGetStackHighWaterMark(h_mon),
                   (unsigned)uxTaskGetStackHighWaterMark(h_buz));
    }
}

int main(void)
{
    HAL_Init();
    dwt_init();
    uart_init();
    rtc_init();
    uart_mutex = xSemaphoreCreateMutex();

    log_printf("\r\nRT-Spark bottle: sensor bring-up starting\r\n");

    xTaskCreate(led_task,     "led",     192, NULL, 1, &h_led);
    xTaskCreate(buzzer_task,  "buzzer",  384, NULL, 2, &h_buz);
    xTaskCreate(button_task,  "buttons", 384, NULL, 2, &h_btn);
    xTaskCreate(sensor_task,  "sensors", 512, NULL, 3, &h_sens);
    xTaskCreate(monitor_task, "monitor", 384, NULL, 1, &h_mon);

    vTaskStartScheduler();
    for (;;) {}
}