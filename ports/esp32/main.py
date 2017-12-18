
import bluetooth, esp, machine

try:
    bt = bluetooth.LE()
except MemoryError:
    # BT memory hasn't been reserved yet.
    # Set a flag in RTC memory that BT should be enabled at start.
    esp.bluetooth_allocate_memory(True)
    # Reset, BT memory will be enabled on next boot
    machine.reset()
    # unreachable

# Bluetooth has been enabled, use it
bt.ble_adv_enable(True)
# etc

