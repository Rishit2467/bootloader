#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

#define APP_START_ADDR  0x08004000UL
#define APP_VERSION     "v1.0"

UART_HandleTypeDef huart2;

void SysTick_Handler(void) {
    HAL_IncTick();
}

static void GPIO_Init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = GPIO_PIN_5;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

static void USART2_Init(void) {
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = GPIO_PIN_2;
    GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin   = GPIO_PIN_3;
    GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
}

int main(void) {
    // Ensure vector table points to 0x08004000 after SystemInit()
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    GPIO_Init();
    USART2_Init();

    char msg[96];
    uint32_t counter = 0;

    while (1) {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
        int len = snprintf(msg, sizeof(msg),
            "[APP %s @ 0x08004000] Running... tick=%lu\r\n",
            APP_VERSION, (unsigned long)counter++);
        HAL_UART_Transmit(&huart2, (uint8_t *)msg, (uint16_t)len, 100);
        HAL_Delay(1000);
    }
}