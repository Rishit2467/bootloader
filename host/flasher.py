import sys
import time
import zlib
import struct
import argparse
import serial

DEFAULT_PORT = "COM5"
BAUD_RATE    = 115200
CMD_SYNC     = b"\x7F"
RESP_ACK     = b"\x06"
RESP_NACK    = b"\x15"
CHUNK_SIZE   = 256


def wait_ack(ser, stage_name, timeout_s=5.0):
    ser.timeout = timeout_s
    resp = ser.read(1)
    if resp != RESP_ACK:
        raise RuntimeError(f"[{stage_name}] Expected ACK (0x06), got {resp!r}")


def flash_firmware(bin_path, port, corrupt=False):
    with open(bin_path, "rb") as f:
        fw = bytearray(f.read())

    fw_size = len(fw)
    expected_crc = zlib.crc32(fw) & 0xFFFFFFFF

    print(f"Loaded : {bin_path}")
    print(f"Size   : {fw_size} bytes | CRC32: 0x{expected_crc:08X}")

    if corrupt:
        flip_idx = fw_size // 2
        fw[flip_idx] ^= 0x01
        print(f"[FAULT INJECTION] Flipped 1 bit at byte offset 0x{flip_idx:04X} (metadata CRC32 unchanged)")

    ser = serial.Serial(port, BAUD_RATE, timeout=0.25)
    ser.reset_input_buffer()

    try:
        print("\nPress the black RESET button on the Nucleo board now...")
        while True:
            ser.write(CMD_SYNC)
            if ser.read(1) == RESP_ACK:
                print("Bootloader synced!")
                ser.reset_input_buffer()
                break

        t_total_start = time.perf_counter()

        # 1. Send 8-byte metadata: [size (uint32 LE)][crc32 (uint32 LE)]
        t_erase_start = time.perf_counter()
        meta = struct.pack("<II", fw_size, expected_crc)
        ser.write(meta)
        print("Erasing application flash pages...")
        wait_ack(ser, "ERASE", timeout_s=5.0)
        t_erase = (time.perf_counter() - t_erase_start) * 1000.0

        # 2. Send firmware in 256-byte chunks
        t_write_start = time.perf_counter()
        total_chunks = (fw_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        for idx in range(total_chunks):
            chunk = fw[idx * CHUNK_SIZE : (idx + 1) * CHUNK_SIZE]
            ser.write(chunk)
            wait_ack(ser, f"CHUNK {idx + 1}/{total_chunks}", timeout_s=3.0)
            pct = ((idx + 1) * 100) // total_chunks
            print(f"\rFlashing: {pct:3d}% ({idx + 1}/{total_chunks} chunks)", end="", flush=True)
        t_write = time.perf_counter() - t_write_start

        # 3. Wait for final CRC32 verification response
        print("\nVerifying CRC32 on STM32 Flash...")
        ser.timeout = 5.0
        resp = ser.read(1)
        t_total = time.perf_counter() - t_total_start
        throughput_kbps = (fw_size / 1024.0) / t_write if t_write > 0 else 0.0

        if corrupt:
            if resp == RESP_NACK:
                print("\n[PASS] Bootloader detected 1-bit corruption and returned NACK (0x15)!")
                print("       Refused to jump to corrupted firmware and stayed in bootloader mode.")
                return
            else:
                raise RuntimeError(f"[FAIL] Expected NACK (0x15) for corrupted image, got {resp!r}")

        if resp != RESP_ACK:
            raise RuntimeError(f"[CRC32_VERIFY] Expected ACK (0x06), got {resp!r}")

        print("CRC32 verified! Bootloader is jumping to 0x08004000.")
        print("\n--- Transfer Metrics ---")
        print(f"Erase Time     : {t_erase:.1f} ms")
        print(f"Flash Time     : {t_write:.3f} s")
        print(f"Total Time     : {t_total:.3f} s")
        print(f"Throughput     : {throughput_kbps:.2f} KB/s\n")

        # 4. Stay connected to show live output from the newly booted app
        print("--- Live App Output (Ctrl+C to exit) ---")
        ser.timeout = 1.0
        while True:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line:
                print(line)

    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="STM32 Custom UART Bootloader Host Flasher")
    parser.add_argument("bin_path", help="Path to compiled firmware .bin file")
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--corrupt", action="store_true", help="Inject a 1-bit fault to test CRC32 rejection")
    args = parser.parse_args()

    flash_firmware(args.bin_path, args.port, corrupt=args.corrupt)