#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * 1. MEMORY MAP & PROTOCOL CONSTANTS (STM32F103RB: 128 KB Flash, 1 KB/Page)
 * ============================================================================ */
#define APP_START_ADDR      0x08004000UL            // Page 16
#define APP_MAX_SIZE        (112UL * 1024UL)        // 112 KB (Pages 16..127)
#define FLASH_PAGE_SZ       1024UL

#define CMD_SYNC            0x7F
#define RESP_ACK            0x06
#define RESP_NACK           0x15

#define BOOT_WAIT_MS        3000UL                  // 3-second UART window
#define CHUNK_SIZE          256UL

UART_HandleTypeDef huart2;

typedef void (*pFunction)(void);

/* ============================================================================
 * 2. HARDWARE INITIALIZATION (HSI 8 MHz default, USART2 @ 115200, LD2 @ PA5)
 * ============================================================================ */
void SysTick_Handler(void) {
    HAL_IncTick();
}

static void GPIO_Init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = GPIO_PIN_5;             // Onboard LD2 LED
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

static void USART2_Init(void) {
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = GPIO_PIN_2;             // USART2_TX
    GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin   = GPIO_PIN_3;             // USART2_RX
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

/* ============================================================================
 * 3. CRC32 (STANDARD IEEE 802.3 — MATCHES PYTHON zlib.crc32)
 * ============================================================================ */
static uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/* ============================================================================
 * 4. FLASH ERASE & HALF-WORD PROGRAMMING
 * ============================================================================ */
static HAL_StatusTypeDef Flash_Erase_App(uint32_t fw_size) {
    uint32_t num_pages = (fw_size + FLASH_PAGE_SZ - 1UL) / FLASH_PAGE_SZ;

    FLASH_EraseInitTypeDef erase_init = {0};
    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase_init.PageAddress = APP_START_ADDR;
    erase_init.NbPages     = num_pages;

    uint32_t page_error = 0;
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();

    return status;
}

static HAL_StatusTypeDef Flash_Write_Chunk(uint32_t addr, const uint8_t *buf, uint32_t len) {
    HAL_StatusTypeDef status = HAL_OK;
    HAL_FLASH_Unlock();

    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t halfword = buf[i];
        if ((i + 1) < len) {
            halfword |= ((uint16_t)buf[i + 1] << 8);
        } else {
            halfword |= 0xFF00U;                    // Pad odd trailing byte with 0xFF
        }

        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, halfword);
        if (status != HAL_OK) {
            break;
        }
    }

    HAL_FLASH_Lock();
    return status;
}

/* ============================================================================
 * 5. CLEAN JUMP FROM BOOTLOADER TO APPLICATION (0x08004000)
 * ============================================================================ */
static uint8_t Is_App_Valid(void) {
    uint32_t app_msp = *(volatile uint32_t *)APP_START_ADDR;
    // STM32F103RB SRAM is 20 KB: 0x20000000 .. 0x20005000
    return (app_msp >= 0x20000000UL && app_msp <= 0x20005000UL);
}

static void Jump_To_App(void) {
    uint32_t app_msp   = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4UL);
    pFunction app_entry = (pFunction)app_reset;

    // 1. Turn off LED and tear down bootloader peripherals
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    HAL_UART_DeInit(&huart2);
    HAL_DeInit();

    // 2. Disable interrupts and stop SysTick before relocating vector table
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    // 3. Relocate vector table to application flash base
    SCB->VTOR = APP_START_ADDR;

    // 4. Load application Main Stack Pointer and branch to Reset_Handler
    __set_MSP(app_msp);
    __enable_irq();
    app_entry();
}

/* ============================================================================
 * 6. UART FIRMWARE UPDATE HANDLER
 * ============================================================================ */
static void UART_SendByte(uint8_t b) {
    HAL_UART_Transmit(&huart2, &b, 1, 100);
}

static uint8_t Handle_UART_Update(void) {
    uint8_t meta[8];
    uint8_t chunk[CHUNK_SIZE];

    // Acknowledge SYNC byte
    UART_SendByte(RESP_ACK);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); // Solid LED = flashing

    // 1. Receive 8-byte metadata: [4B size LE][4B CRC32 LE]
    if (HAL_UART_Receive(&huart2, meta, 8, 3000) != HAL_OK) {
        UART_SendByte(RESP_NACK);
        return 0;
    }

    uint32_t fw_size = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8) |
                       ((uint32_t)meta[2] << 16) | ((uint32_t)meta[3] << 24);
    uint32_t expected_crc = (uint32_t)meta[4] | ((uint32_t)meta[5] << 8) |
                            ((uint32_t)meta[6] << 16) | ((uint32_t)meta[7] << 24);

    if (fw_size == 0 || fw_size > APP_MAX_SIZE) {
        UART_SendByte(RESP_NACK);
        return 0;
    }

    // 2. Erase required application flash pages
    if (Flash_Erase_App(fw_size) != HAL_OK) {
        UART_SendByte(RESP_NACK);
        return 0;
    }
    UART_SendByte(RESP_ACK);

    // 3. Receive and program firmware in 256-byte chunks
    uint32_t bytes_written = 0;
    while (bytes_written < fw_size) {
        uint32_t remaining = fw_size - bytes_written;
        uint32_t to_read   = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;

        if (HAL_UART_Receive(&huart2, chunk, (uint16_t)to_read, 3000) != HAL_OK) {
            UART_SendByte(RESP_NACK);
            return 0;
        }

        if (Flash_Write_Chunk(APP_START_ADDR + bytes_written, chunk, to_read) != HAL_OK) {
            UART_SendByte(RESP_NACK);
            return 0;
        }

        bytes_written += to_read;
        UART_SendByte(RESP_ACK);
    }

    // 4. Verify CRC32 directly over written Flash
    uint32_t calc_crc = CRC32_Calculate((const uint8_t *)APP_START_ADDR, fw_size);
    if (calc_crc != expected_crc) {
        UART_SendByte(RESP_NACK);
        return 0;
    }

    UART_SendByte(RESP_ACK);
    HAL_Delay(20); // Let final ACK shift out of USART2 before DeInit
    return 1;
}

/* ============================================================================
 * 7. MAIN ENTRY POINT
 * ============================================================================ */
int main(void) {
    HAL_Init();
    GPIO_Init();
    USART2_Init();

    // 3-second boot window: blink LD2 while listening for 0x7F SYNC byte
    uint32_t start_tick = HAL_GetTick();
    uint32_t last_blink = start_tick;
    uint8_t  rx_byte    = 0;

    while ((HAL_GetTick() - start_tick) < BOOT_WAIT_MS) {
        if ((HAL_GetTick() - last_blink) >= 100) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            last_blink = HAL_GetTick();
        }

        if (HAL_UART_Receive(&huart2, &rx_byte, 1, 10) == HAL_OK) {
            if (rx_byte == CMD_SYNC) {
                if (Handle_UART_Update()) {
                    Jump_To_App();
                }
                // If update failed, reset timer and keep listening
                start_tick = HAL_GetTick();
            }
        }
    }

    // Timeout expired: jump to existing application if valid
    if (Is_App_Valid()) {
        Jump_To_App();
    }

    // No valid app in Flash yet: stay in bootloader and wait for SYNC
    while (1) {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
        if (HAL_UART_Receive(&huart2, &rx_byte, 1, 500) == HAL_OK) {
            if (rx_byte == CMD_SYNC && Handle_UART_Update()) {
                Jump_To_App();
            }
        }
    }
}