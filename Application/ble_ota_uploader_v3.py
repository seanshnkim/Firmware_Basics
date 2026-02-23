#!/usr/bin/env python3
"""
BLE OTA Firmware Uploader for STM32F429
"""

import asyncio
from bleak import BleakClient, BleakScanner
import struct
import zlib
import sys
import os

# --- CONFIGURATION ---
UART_SERVICE_UUID = "0000FFE0-0000-1000-8000-00805F9B34FB"
UART_TX_CHAR_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB"

HM10_ADDRESS = "68:5E:1C:2B:63:2A"
FIRMWARE_FILE = "Debug/Basic-Bootloader.bin"

# Protocol Constants (must match ota_protocol.h)
OTA_MAGIC_START = 0xAA55AA55
OTA_MAGIC_DATA  = 0x55AA55AA

OTA_PKT_START = 0x01
OTA_PKT_DATA  = 0x02
OTA_PKT_END   = 0x03
OTA_PKT_ACK   = 0x04
OTA_PKT_NACK  = 0x05

OTA_CHUNK_SIZE = 1024

BANK_A = 0x00
BANK_B = 0x01


def create_start_packet(firmware_data, target_bank=BANK_B):
    firmware_size = len(firmware_data)
    firmware_crc = zlib.crc32(firmware_data) & 0xFFFFFFFF
    total_chunks = (firmware_size + OTA_CHUNK_SIZE - 1) // OTA_CHUNK_SIZE
    firmware_version = 0x02000100  # Version 2.0.1

    packet = struct.pack(
        '<I B I I I I B',
        OTA_MAGIC_START,
        OTA_PKT_START,
        firmware_size,
        firmware_version,
        firmware_crc,
        total_chunks,
        target_bank
    )

    print(f"START Packet:")
    print(f"  Firmware Size: {firmware_size} bytes")
    print(f"  Firmware CRC32: 0x{firmware_crc:08X}")
    print(f"  Total Chunks: {total_chunks}")
    print(f"  Target Bank: {'Bank B' if target_bank == BANK_B else 'Bank A'}")
    print(f"  Packet Size: {len(packet)} bytes")

    return packet


def create_data_packet(chunk_number, chunk_data):
    chunk_size = len(chunk_data)
    chunk_crc = zlib.crc32(chunk_data) & 0xFFFFFFFF

    padded_data = chunk_data + b'\xFF' * (OTA_CHUNK_SIZE - chunk_size)

    packet = struct.pack(
        '<I B I H I',
        OTA_MAGIC_DATA,
        OTA_PKT_DATA,
        chunk_number,
        chunk_size,
        chunk_crc
    ) + padded_data

    return packet, chunk_crc


def create_end_packet():
    return struct.pack('<I B', OTA_MAGIC_START, OTA_PKT_END)


def parse_response_packet(data):
    if len(data) < 10:
        return None
    try:
        magic, pkt_type, error_code, last_chunk = struct.unpack('<I B B I', data[:10])
        return {
            'magic': magic,
            'type': pkt_type,
            'error_code': error_code,
            'last_chunk': last_chunk
        }
    except Exception:
        return None


class OTAUploader:
    def __init__(self, client, characteristic_uuid):
        self.client = client
        self.char_uuid = characteristic_uuid
        self.response_data = bytearray()
        self.response_event = asyncio.Event()

    def notification_handler(self, sender, data):
        self.response_data.extend(data)
        if len(self.response_data) >= 10:
            self.response_event.set()

    async def send_packet(self, packet, packet_name, wait_for_ack=False, timeout=10.0):
        MAX_BLE_WRITE_SIZE = 20

        print(f"Sending {packet_name} ({len(packet)} bytes)...", end="", flush=True)

        self.response_data.clear()
        self.response_event.clear()

        offset = 0
        while offset < len(packet):
            chunk = packet[offset:offset + MAX_BLE_WRITE_SIZE]
            await self.client.write_gatt_char(self.char_uuid, chunk, response=False)
            offset += MAX_BLE_WRITE_SIZE
            await asyncio.sleep(0.01)

        print(" [SENT]")

        if wait_for_ack:
            try:
                await asyncio.wait_for(self.response_event.wait(), timeout=timeout)

                response = parse_response_packet(self.response_data)
                if response:
                    if response['type'] == OTA_PKT_ACK:
                        print(f"  ✓ ACK received (last chunk: {response['last_chunk']})")
                        return True
                    elif response['type'] == OTA_PKT_NACK:
                        print(f"  ✗ NACK received (error: {response['error_code']})")
                        return False
                else:
                    print(f"  ? Invalid response: {self.response_data.hex()}")
                    return False
            except asyncio.TimeoutError:
                print(f"  ⏱ Timeout waiting for ACK")
                return False

        return True


async def upload_firmware(address, firmware_path):
    print(f"\n{'='*50}")
    print(f"  BLE OTA FIRMWARE UPLOADER")
    print(f"{'='*50}\n")

    if not os.path.exists(firmware_path):
        print(f"ERROR: Firmware file not found: {firmware_path}")
        return False

    with open(firmware_path, 'rb') as f:
        firmware_data = f.read()

    print(f"Loaded firmware: {firmware_path}")
    print(f"Size: {len(firmware_data)} bytes\n")

    print(f"Connecting to HM-10 at {address}...")

    try:
        async with BleakClient(address, timeout=20.0) as client:
            print(f"✓ Connected: {client.is_connected}\n")

            uploader = OTAUploader(client, UART_TX_CHAR_UUID)

            await client.start_notify(UART_TX_CHAR_UUID, uploader.notification_handler)
            print("✓ Notifications enabled\n")

            print("Waiting for STM32 to enter OTA mode...")
            await asyncio.sleep(2)

            # --- START packet with retry ---
            print("--- SENDING START PACKET ---")
            start_packet = create_start_packet(firmware_data, target_bank=BANK_B)

            success = False
            max_retries = 3
            for attempt in range(max_retries):
                success = await uploader.send_packet(
                    start_packet, "START", wait_for_ack=True, timeout=10.0)
                if success:
                    break
                print(f"  ✗ START attempt {attempt + 1}/{max_retries} failed")

            if not success:
                print("✗ Failed to send START packet after all retries")
                return False

            print()

            # --- DATA packets ---
            total_chunks = (len(firmware_data) + OTA_CHUNK_SIZE - 1) // OTA_CHUNK_SIZE
            print(f"--- SENDING DATA PACKETS ({total_chunks} chunks) ---")

            for chunk_num in range(total_chunks):
                start_idx = chunk_num * OTA_CHUNK_SIZE
                end_idx = min(start_idx + OTA_CHUNK_SIZE, len(firmware_data))
                chunk_data = firmware_data[start_idx:end_idx]

                data_packet, chunk_crc = create_data_packet(chunk_num, chunk_data)
                packet_name = f"DATA #{chunk_num + 1}/{total_chunks} (CRC: 0x{chunk_crc:08X})"

                success = await uploader.send_packet(
                    data_packet, packet_name, wait_for_ack=True, timeout=15.0)

                if not success:
                    print(f"\n✗ Failed at chunk {chunk_num + 1}")
                    return False

                progress = (chunk_num + 1) / total_chunks * 100
                print(f"  Progress: {progress:.1f}%")

            print()

            # --- END packet ---
            print("--- SENDING END PACKET ---")
            end_packet = create_end_packet()
            success = await uploader.send_packet(
                end_packet, "END", wait_for_ack=True, timeout=15.0)

            if not success:
                print("\n✗ Failed to send END packet")
                return False

            print()
            print(f"{'='*50}")
            print(f"  ✓ OTA UPDATE COMPLETED SUCCESSFULLY!")
            print(f"{'='*50}")
            print("STM32 will now reset and boot new firmware.")

            await client.stop_notify(UART_TX_CHAR_UUID)
            return True

    except Exception as e:
        print(f"\n✗ Connection error: {e}")
        return False


async def scan_for_hm10():
    print("Scanning for BLE devices...")
    devices = await BleakScanner.discover(timeout=5.0)
    print("\nFound devices:")
    for i, device in enumerate(devices):
        print(f"{i+1}. {device.address} - {device.name or 'Unknown'}")


if __name__ == "__main__":
    if "XX" in HM10_ADDRESS or HM10_ADDRESS == "00:00:00:00:00:00":
        print("ERROR: Please set HM10_ADDRESS to your HM-10's MAC address!")
        asyncio.run(scan_for_hm10())
        sys.exit(1)

    if len(sys.argv) > 1:
        FIRMWARE_FILE = sys.argv[1]

    success = asyncio.run(upload_firmware(HM10_ADDRESS, FIRMWARE_FILE))
    sys.exit(0 if success else 1)
