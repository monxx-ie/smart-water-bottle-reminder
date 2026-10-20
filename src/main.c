#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct { uint32_t tick; uint32_t count; } sample_t;

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t uart_mutex;
static QueueHandle_t sample_queue;
static TaskHandle_t h_sensor, h_logger, h_monitor;

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

static void sensor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    sample_t s = {0, 0};
    for (;;) {
        s.tick = xTaskGetTickCount();
        s.count++;
        xQueueSend(sample_queue, &s, 0);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

static void logger_task(void *arg)
{
    (void)arg;
    sample_t s;
    for (;;) {
        if (xQueueReceive(sample_queue, &s, portMAX_DELAY) == pdTRUE) {
            if (s.count % 10 == 0) {
                log_printf("[%lu ms] sample #%lu\r\n",
                           (unsigned long)s.tick, (unsigned long)s.count);
            }
        }
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(2000));
        log_printf("[%lu ms] heap free=%u B | stack HWM (words): sensor=%u logger=%u monitor=%u\r\n",
                   (unsigned long)xTaskGetTickCount(),
                   (unsigned)xPortGetFreeHeapSize(),
                   (unsigned)uxTaskGetStackHighWaterMark(h_sensor),
                   (unsigned)uxTaskGetStackHighWaterMark(h_logger),
                   (unsigned)uxTaskGetStackHighWaterMark(h_monitor));
    }
}

int main(void)
{
    HAL_Init();
    uart_init();

    uart_mutex = xSemaphoreCreateMutex();
    sample_queue = xQueueCreate(8, sizeof(sample_t));

    log_printf("\r\nRT-Spark bottle: starting FreeRTOS scheduler\r\n");

    xTaskCreate(sensor_task,  "sensor",  384, NULL, 3, &h_sensor);
    xTaskCreate(logger_task,  "logger",  384, NULL, 2, &h_logger);
    xTaskCreate(monitor_task, "monitor", 384, NULL, 1, &h_monitor);

    vTaskStartScheduler();
    for (;;) {}
}