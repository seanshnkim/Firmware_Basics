import asyncio
from bleak import BleakClient, BleakScanner
import struct

# --- CONFIGURATION ---
# The standard HM-10 Service/Characteristic UUIDs
# Verify these with a scanning app (like nRF Connect) if they differ.
UART_SERVICE_UUID = "0000FFE0-0000-1000-8000-00805F9B34FB"
UART_TX_CHAR_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB" # Write & Notify

# The MAC Address of your HM-10
# Run a separate scan script to find this, or use the scanner provided below.
HM10_ADDRESS = "68:5E:1C:2B:63:2A"

# OTA Settings
FIRMWARE_FILE = "Debug/Basic-Bootloader.bin"
PACKET_SIZE = 64    # Must match your STM32 Bootloader's expected chunk size
ACK_BYTE = b'\x06'  # The byte your STM32 sends to say "OK" (ACK)

# --- GLOBAL EVENTS ---
# This is a thread-safe "flag" that lets us pause the code until data arrives
ack_received_event = asyncio.Event()

def notification_handler(sender, data):
    """
    This function is called automatically whenever the HM-10 sends data 
    (which is actually the STM32 sending data to the HM-10 UART).
    """
    # print(f"RX: {data}") # Uncomment for verbose debug
    
    if ACK_BYTE in data:
        # We found the ACK! Unpause the main loop.
        ack_received_event.set()

async def main():
    print(f"Connecting to {HM10_ADDRESS}...")
    
    async with BleakClient(HM10_ADDRESS) as client:
        print(f"Connected: {client.is_connected}")
        
        # 1. Enable Notifications
        # This tells the HM-10: "If you get UART data, send it to me immediately."
        await client.start_notify(UART_TX_CHAR_UUID, notification_handler)
        print("Notifications enabled. Listening for ACKs...")

        # 2. Open the Firmware File
        with open(FIRMWARE_FILE, "rb") as f:
            file_data = f.read()
        
        total_packets = (len(file_data) + PACKET_SIZE - 1) // PACKET_SIZE
        print(f"Firmware Size: {len(file_data)} bytes ({total_packets} packets)")

        # 3. Sending Loop (Stop-and-Wait)
        for i in range(total_packets):
            # Slice the data into chunks
            start = i * PACKET_SIZE
            end = start + PACKET_SIZE
            chunk = file_data[start:end]
            
            # Send the chunk
            # Note: Bleak handles splitting this into 20-byte BLE frames automatically
            print(f"Sending Packet {i+1}/{total_packets} ({len(chunk)} bytes)...", end="", flush=True)
            await client.write_gatt_char(UART_TX_CHAR_UUID, chunk, response=True)
            
            # 4. THE MAGIC: Wait for ACK
            # This pauses this function here. It will NOT proceed until 
            # notification_handler() fires and sets the event.
            try:
                # Wait up to 5 seconds for an ACK
                await asyncio.wait_for(ack_received_event.wait(), timeout=5.0)
                
                # If we get here, we got the ACK. Clear the flag for the next loop.
                ack_received_event.clear()
                print(" [ACK]")
                
            except asyncio.TimeoutError:
                print("\nError: Timed out waiting for ACK from STM32.")
                print("Make sure your HM-10 baud rate matches the STM32.")
                break

        print("Update Finished.")
        await client.stop_notify(UART_TX_CHAR_UUID)

if __name__ == "__main__":
    # Simple check to warn you if you forgot to set the address
    if "XX" in HM10_ADDRESS:
        print("Please run a scanner to find your HM-10 MAC Address first!")
        # Optional: Uncomment this line to run a quick scan
        # asyncio.run(BleakScanner.discover())
    else:
        asyncio.run(main())