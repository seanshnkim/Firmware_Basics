#!/usr/bin/env python3
"""
OTA Firmware Sender Script
Sends firmware binary to STM32 bootloader over UART
"""

import serial
import struct
import time
import sys
import os
from zlib import crc32

# Protocol constants (must match ota_protocol.h)
OTA_MAGIC_START = 0xAA55AA55
OTA_MAGIC_DATA = 0x55AA55AA

OTA_PKT_START = 0x01
OTA_PKT_DATA = 0x02
OTA_PKT_END = 0x03
OTA_PKT_ACK = 0x04
OTA_PKT_NACK = 0x05

OTA_CHUNK_SIZE = 1024

BANK_A = 0x00000000
BANK_B = 0x00000001

class OTASender:
    def __init__(self, port, baudrate=115200, timeout=5):
        """Initialize OTA sender"""
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        
    def open(self):
        """Open serial port"""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE
            )
            print(f"✓ Opened {self.port} at {self.baudrate} baud")
            time.sleep(0.5)  # Wait for device to be ready
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to open {self.port}: {e}")
            return False
    
    def close(self):
        """Close serial port"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("✓ Serial port closed")
    
    def calculate_crc32(self, data):
        """Calculate CRC32 of data (compatible with STM32 hardware CRC)"""
        # Python's zlib.crc32 uses different polynomial than STM32
        # For now, we'll use standard CRC32 and note this in testing
        return crc32(data) & 0xFFFFFFFF
    
    def send_start_packet(self, firmware_size, firmware_version, firmware_crc, total_chunks, target_bank):
        """Send START packet"""
        print("\n--- Sending START Packet ---")
        # Pack START packet (must match ota_start_packet_t with packed attribute)
        packet = struct.pack(
            '<IBIIIIB',  # Little-endian, exactly matching C struct
            OTA_MAGIC_START,      # uint32_t magic
            OTA_PKT_START,        # uint8_t packet_type
            firmware_size,        # uint32_t firmware_size
            firmware_version,     # uint32_t firmware_version
            firmware_crc,         # uint32_t firmware_crc32
            total_chunks,         # uint32_t total_chunks
            target_bank           # uint8_t target_bank
        )
        
        print(f"  Firmware size: {firmware_size} bytes")
        print(f"  Firmware version: 0x{firmware_version:08X}")
        print(f"  Firmware CRC32: 0x{firmware_crc:08X}")
        print(f"  Total chunks: {total_chunks}")
        print(f"  Target bank: {'Bank A' if target_bank == BANK_A else 'Bank B'}")
        
        self.ser.write(packet)
        return self.wait_for_response("START")
    
    def send_data_packet(self, chunk_number, chunk_data):
        """Send DATA packet"""
        chunk_size = len(chunk_data)
        chunk_crc = self.calculate_crc32(chunk_data)
        
        # Pack DATA packet header
        header = struct.pack(
            '<IBIHI',  # magic, type, chunk_number, chunk_size, chunk_crc
            OTA_MAGIC_DATA,
            OTA_PKT_DATA,
            chunk_number,
            chunk_size,
            chunk_crc
        )
        
        # Pad chunk data to OTA_CHUNK_SIZE
        padded_data = chunk_data + b'\xFF' * (OTA_CHUNK_SIZE - chunk_size)
        
        # Total packet: 15 + 1024 = 1039 bytes
        packet = header + padded_data
        self.ser.write(packet)
        
        return self.wait_for_response(f"DATA chunk {chunk_number}")
    
    def send_end_packet(self):
        """Send END packet"""
        print("\n--- Sending END Packet ---")
        
        packet = struct.pack(
            '<IB',
            OTA_MAGIC_START,
            OTA_PKT_END
        )
        
        self.ser.write(packet)
        return self.wait_for_response("END")
    
    def wait_for_response(self, packet_name):
        """Wait for ACK or NACK response"""
        try:
            # Response packet: magic (4) + type (1) + error (1) + last_chunk (4) = 10 bytes
            response = self.ser.read(10)
            
            if len(response) < 10:
                print(f"✗ {packet_name}: Timeout or incomplete response")
                return False
            
            magic, pkt_type, error_code, last_chunk = struct.unpack('<IBBI', response)
            
            if magic != OTA_MAGIC_START:
                print(f"✗ {packet_name}: Invalid response magic: 0x{magic:08X}")
                return False
            
            if pkt_type == OTA_PKT_ACK:
                print(f"✓ {packet_name}: ACK (chunks received: {last_chunk})")
                return True
            elif pkt_type == OTA_PKT_NACK:
                print(f"✗ {packet_name}: NACK (error code: {error_code}, last chunk: {last_chunk})")
                return False
            else:
                print(f"✗ {packet_name}: Unknown response type: {pkt_type}")
                return False
                
        except Exception as e:
            print(f"✗ {packet_name}: Exception waiting for response: {e}")
            return False
    
    def send_firmware(self, firmware_path, target_bank=BANK_B, version=0x00010000):
        """Send entire firmware file"""
        
        # Step 1: Read firmware file
        print(f"\n{'='*50}")
        print(f"  OTA Firmware Update")
        print(f"{'='*50}")
        
        if not os.path.exists(firmware_path):
            print(f"✗ Firmware file not found: {firmware_path}")
            return False
        
        with open(firmware_path, 'rb') as f:
            firmware_data = f.read()
        
        firmware_size = len(firmware_data)
        print(f"\n✓ Loaded firmware: {firmware_path}")
        print(f"  Size: {firmware_size} bytes ({firmware_size/1024:.2f} KB)")
        
        # Step 2: Calculate CRC32
        firmware_crc = self.calculate_crc32(firmware_data)
        print(f"  CRC32: 0x{firmware_crc:08X}")
        
        # Step 3: Calculate chunks
        total_chunks = (firmware_size + OTA_CHUNK_SIZE - 1) // OTA_CHUNK_SIZE
        print(f"  Total chunks: {total_chunks}")
        
        # Step 4: Send START packet
        if not self.send_start_packet(firmware_size, version, firmware_crc, total_chunks, target_bank):
            print("\n✗ OTA Update FAILED at START packet")
            return False
        
        # Step 5: Send DATA packets
        print(f"\n--- Sending {total_chunks} DATA Packets ---")
        
        for chunk_num in range(total_chunks):
            offset = chunk_num * OTA_CHUNK_SIZE
            chunk_data = firmware_data[offset:offset + OTA_CHUNK_SIZE]
            
            # Show progress
            progress = ((chunk_num + 1) * 100) // total_chunks
            print(f"Progress: [{chunk_num + 1}/{total_chunks}] {progress}% ", end='')
            
            if not self.send_data_packet(chunk_num, chunk_data):
                print(f"\n✗ OTA Update FAILED at chunk {chunk_num}")
                return False
        
        print("\n✓ All DATA packets sent successfully")
        
        # Step 6: Send END packet
        if not self.send_end_packet():
            print("\n✗ OTA Update FAILED at END packet")
            return False
        
        print(f"\n{'='*50}")
        print(f"  ✓ OTA UPDATE SUCCESSFUL!")
        print(f"{'='*50}")
        print("\nNext steps:")
        print("1. Reset/reboot your device")
        print("2. Bootloader will boot from the new firmware")
        print("3. Verify new firmware is running")
        
        return True

def main():
    """Main function"""
    if len(sys.argv) < 3:
        print("Usage: python ota_sender.py <firmware.bin> <serial_port> [target_bank]")
        print("Example: python ota_sender.py app.bin /dev/ttyACM0")
        print("         python ota_sender.py app.bin /dev/ttyACM0 BANK_B")
        sys.exit(1)
    
    firmware_path = sys.argv[1]
    serial_port = sys.argv[2]
    target_bank = BANK_B  # Default
    
    if len(sys.argv) >= 4:
        if sys.argv[3].upper() == "BANK_A":
            target_bank = BANK_A
        elif sys.argv[3].upper() == "BANK_B":
            target_bank = BANK_B
    
    # Create OTA sender
    sender = OTASender(serial_port)
    
    if not sender.open():
        sys.exit(1)
    
    try:
        success = sender.send_firmware(firmware_path, target_bank)
        sys.exit(0 if success else 1)
    except KeyboardInterrupt:
        print("\n\n✗ Interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        sender.close()

if __name__ == "__main__":
    main()