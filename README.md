# Custom UART Bootloader on STM32

A two-stage bare-metal/HAL firmware update system for the **STM32 Nucleo-F103RB** (ARM Cortex-M3, 128 KB Flash) supporting over-UART firmware flashing, IEEE 802.3 **CRC32** integrity verification, vector table relocation (`SCB->VTOR`), and fault-tolerant boot fallback—requiring no external hardware beyond the onboard ST-LINK Virtual COM Port.

---

## Flash Memory Architecture

The STM32F103RB's 128 KB Flash (128 × 1 KB pages) is partitioned via custom linker scripts (`bootloader.ld` and `app.ld`):

| Region | Start Address | End Address | Size | Flash Pages | Role |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Bootloader** | `0x08000000` | `0x08003FFF` | 16 KB | Pages 0 – 15 | 3 s UART listener, flash erase/write, CRC32 check, jump |
| **Application** | `0x08004000` | `0x0801FFFF` | 112 KB | Pages 16 – 127 | Relocated user firmware (`SCB->VTOR = 0x08004000`) |

---

## UART Update Protocol (`115200 8N1`)

Flow-controlled half-duplex protocol over `USART2` (`PA2`/`PA3`) to prevent UART overrun during Flash page erase/programming:

1. **Sync (3 s boot window):** Bootloader blinks `LD2` (`PA5`) and listens for `0x7F`. If received, it replies `ACK` (`0x06`). If 3 seconds elapse with no sync byte, it validates the application's initial Main Stack Pointer (`0x20000000`–`0x20005000`) and jumps directly to `0x08004000`.
2. **Metadata:** Host sends 8 bytes: `[4-byte firmware size (uint32 LE)]` + `[4-byte IEEE CRC32 (uint32 LE)]`.
3. **Erase:** Bootloader validates `size <= 112 KB`, erases the required 1 KB Flash pages starting at `0x08004000`, and replies `ACK` (`0x06`).
4. **Chunked Programming:** Host transmits firmware in **256-byte chunks**. Bootloader programs each chunk as 16-bit half-words (`FLASH_TYPEPROGRAM_HALFWORD`) and replies `ACK` (`0x06`) before the next chunk is sent.
5. **Verification & Jump:** Bootloader computes CRC32 directly over the written Flash region (`0x08004000` to `0x08004000 + size`).
   * **Match:** Replies `ACK` (`0x06`), de-initializes peripherals and SysTick, relocates `SCB->VTOR`, and switches `MSP` + branches to the application's `Reset_Handler` via inline assembly (`msr msp` / `bx`).
   * **Mismatch:** Replies `NACK` (`0x15`) and remains safely in bootloader mode without booting the corrupted image.

---

## Measured Performance & Verification

Tested with `app` firmware (`4,600 bytes`, `CRC32: 0x640CD343`) over `USART2` at `115200` baud:

| Metric | Measured Result |
| :--- | :--- |
| **Firmware Size** | 4,600 bytes (18 × 256-byte chunks) |
| **Flash Erase Time (5 pages)** | **112.0 ms** |
| **UART Transfer + Half-Word Flash Time** | **0.605 s** |
| **Total Update Time (Erase + Flash + CRC32)** | **0.762 s** |
| **Effective Throughput** | **7.42 KB/s** |
| **1-Bit Fault Injection (`--corrupt` at `0x08FC`)** | **PASS** — Detected mismatch, returned `NACK (0x15)`, refused boot |

### Clean Firmware Flash & Live Jump
![Clean Flash](assets/flash_success.png)

### 1-Bit Fault-Injection Test (`--corrupt`)
![Fault Injection](assets/fault_injection.png)

---

## Project Structure

```text
├── bootloader/
│   ├── platformio.ini      # STM32Cube config with custom 16 KB linker script
│   ├── bootloader.ld       # FLASH ORIGIN = 0x08000000, LENGTH = 16K
│   └── src/main.c          # UART protocol, Flash erase/write, CRC32, asm jump
├── app/
│   ├── platformio.ini      # STM32Cube config with custom 112 KB linker script
│   ├── app.ld              # FLASH ORIGIN = 0x08004000, LENGTH = 112K
│   └── src/main.c          # Relocated app (SCB->VTOR = 0x08004000)
├── host/
│   └── flasher.py          # Python CLI flasher with timing metrics & --corrupt mode
└── assets/
    ├── flash_success.png
    └── fault_injection.png
```

---

## Build & Usage

1. **Flash the bootloader once via ST-LINK:**
   ```bash
   cd bootloader
   pio run -t upload
   ```

2. **Compile the application binary (do not upload via ST-LINK):**
   ```bash
   cd ../app
   pio run
   ```

3. **Flash the application over UART** (replace `COM5` with your Nucleo port and press the black **RESET** button when prompted):
   ```bash
   cd ..
   python host/flasher.py app/.pio/build/nucleo_f103rb/firmware.bin --port COM5
   ```

4. **Run the 1-bit fault-injection test:**
   ```bash
   python host/flasher.py app/.pio/build/nucleo_f103rb/firmware.bin --port COM5 --corrupt
   ```
