import sys
import zlib
import struct
import serial

SERIAL_PORT = "COM5"
BAUD_RATE   = 115200
CMD_SYNC    = b"\x7F"
RESP_ACK    = b"\x06"
CHUNK_SIZE  = 256

def wait_ack(ser, stage_name, timeout_s=5.0):
    ser.timeout = timeout_s
    resp = ser.read(1)
    if resp != RESP_ACK:
        raise RuntimeError(f"[{stage_name}] Expected ACK (0x06), got {resp!r}")

def flash_firmware(bin_path):
    with open(bin_path, "rb") as f:
        fw = f.read()

    fw_size = len(fw)
    crc32 = zlib.crc32(fw) & 0xFFFFFFFF
    print(f"Loaded: {bin_path}")
    print(f"Size  : {fw_size} bytes | CRC32: 0x{crc32:08X}")

    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.25)
    ser.reset_input_buffer()

    try:
        print("\nPress the black RESET button on the Nucleo board now...")
        while True:
            ser.write(CMD_SYNC)
            if ser.read(1) == RESP_ACK:
                print("Bootloader synced!")
                ser.reset_input_buffer()
                break

        # 1. Send 8-byte metadata: [size (uint32 LE)][crc32 (uint32 LE)]
        meta = struct.pack("<II", fw_size, crc32)
        ser.write(meta)
        print("Erasing application flash pages...")
        wait_ack(ser, "ERASE", timeout_s=5.0)

        # 2. Send firmware in 256-byte chunks
        total_chunks = (fw_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        for idx in range(total_chunks):
            chunk = fw[idx * CHUNK_SIZE : (idx + 1) * CHUNK_SIZE]
            ser.write(chunk)
            wait_ack(ser, f"CHUNK {idx + 1}/{total_chunks}", timeout_s=3.0)
            pct = ((idx + 1) * 100) // total_chunks
            print(f"\rFlashing: {pct:3d}% ({idx + 1}/{total_chunks} chunks)", end="", flush=True)

        # 3. Wait for final CRC32 verification ACK
        print("\nVerifying CRC32 on STM32 Flash...")
        wait_ack(ser, "CRC32_VERIFY", timeout_s=5.0)
        print("CRC32 verified! Bootloader is jumping to 0x08004000.\n")

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
    if len(sys.argv) < 2:
        print("Usage: python flasher.py <path_to_firmware.bin>")
        sys.exit(1)
    flash_firmware(sys.argv[1])