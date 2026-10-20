#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdarg.h>
#include <stdio.h>

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t uart_mutex;
static TaskHandle_t h_led, h_btn, h_mon;

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
    char buf[96];
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
    const char *name;
} i2c_bus_t;

static const i2c_bus_t bus_a = { GPIOF, GPIO_PIN_1, GPIOF, GPIO_PIN_0, "A (SCL=PF1 SDA=PF0)" };
static const i2c_bus_t bus_b = { GPIOE, GPIO_PIN_0, GPIOE, GPIO_PIN_1, "B (SCL=PE0 SDA=PE1)" };

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

static int i2c_probe(const i2c_bus_t *b, uint8_t addr)
{
    i2c_start(b);
    int ack = i2c_write(b, (uint8_t)(addr << 1));
    i2c_stop(b);
    return ack;
}

static void scan_bus(const i2c_bus_t *b)
{
    i2c_init(b);
    log_printf("I2C bus %s\r\n", b->name);
    if (!SDA_READ(b)) {
        log_printf("  SDA stuck low: wrong pin or no pull-up\r\n");
    }
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_probe(b, a)) {
            log_printf("  device at 0x%02X\r\n", a);
            found++;
        }
    }
    if (!found) log_printf("  no devices found\r\n");
}

static void i2c_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    scan_bus(&bus_a);
    scan_bus(&bus_b);
    log_printf("Expected: AP3216C=0x1E, ICM20608=0x68/0x69, AHT10=0x38\r\n");
    vTaskDelete(NULL);
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
        log_printf("[%lu ms] heap free=%u B | stack HWM (words): led=%u btn=%u mon=%u\r\n",
                   (unsigned long)xTaskGetTickCount(),
                   (unsigned)xPortGetFreeHeapSize(),
                   (unsigned)uxTaskGetStackHighWaterMark(h_led),
                   (unsigned)uxTaskGetStackHighWaterMark(h_btn),
                   (unsigned)uxTaskGetStackHighWaterMark(h_mon));
    }
}

int main(void)
{
    HAL_Init();
    dwt_init();
    uart_init();
    uart_mutex = xSemaphoreCreateMutex();

    log_printf("\r\nRT-Spark bottle: hardware test starting\r\n");

    xTaskCreate(led_task,     "led",     192, NULL, 1, &h_led);
    xTaskCreate(button_task,  "buttons", 384, NULL, 3, &h_btn);
    xTaskCreate(i2c_task,     "i2cscan", 384, NULL, 2, NULL);
    xTaskCreate(monitor_task, "monitor", 384, NULL, 1, &h_mon);

    vTaskStartScheduler();
    for (;;) {}
}