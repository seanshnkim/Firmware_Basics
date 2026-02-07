import asyncio
from bleak import BleakScanner, BleakClient

async def inspect_device():
    print("Scanning for devices... (this takes ~5 seconds)")
    devices = await BleakScanner.discover()
    
    target_device = None
    
    # 1. Print all found devices
    for d in devices:
        # Filter out devices with no name to reduce clutter
        if d.name:
            print(f"Found: {d.name} | Address: {d.address}")
            # HM-10 usually has these names. If yours is different, look for its specific name.
            if d.name in ["HMSoft", "MLT-BT05", "BT05", "DSD TECH"]:
                target_device = d

    if not target_device:
        print("\nCould not automatically identify the HM-10.")
        print("Please copy the Address of your device from the list above.")
        return

    print(f"\n--- Connecting to {target_device.name} ({target_device.address}) ---")
    
    # 2. Connect and grab UUIDs
    async with BleakClient(target_device.address) as client:
        print("Connected! Fetching Services...")
        for service in client.services:
            print(f"\n[Service] {service.uuid} ({service.description})")
            for char in service.characteristics:
                print(f"  └─ [Char] {char.uuid} (Properties: {','.join(char.properties)})")
                
                # We are looking for the Characteristic that has 'write' and 'notify'
                if "write" in char.properties and "notify" in char.properties:
                    print(f"     *** THIS IS YOUR TARGET UUID ***")

if __name__ == "__main__":
    asyncio.run(inspect_device())