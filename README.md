# tc001-system-test

Hardware test for the Ulanzi TC001 (`ulanzi_tc001/esp32/procpu`). Prints `[PASS]`/`[FAIL]` per
check on the UART console (115200 baud).

Build and flash from a west workspace:

    west build -b ulanzi_tc001/esp32/procpu tc001-system-test
    west flash --esp-device /dev/cu.usbserial-XXX --esp-baud-rate 115200

The CH340 bridge is unreliable above 115200 baud.

Automatic: TRNG, I2C scan (0x44, 0x68), DS1307 RTC, SHT3x, battery and light-sensor ADC,
Wi-Fi scan, Bluetooth scan, watchdog. The watchdog test lets the timer expire and the board
reboots; the second boot checks the reset cause and continues.

Interactive (watch, listen, press):

1. Buzzer: 2 s silence, then 4 beeps rising in pitch.
2. Matrix: corner markers (red, green, blue, white), a raster sweep, full-panel red/green/blue.
3. Buttons: press left, middle, right (blocks turn green); cover and uncover the light sensor.
4. Bluetooth: first column blue, discoverable as `tc001-zephyr` (BLE only, use a BLE scanner
   app). Press middle to continue.
5. Wi-Fi: first column white, open access point `tc001-zephyr` with a DHCP server on
   192.168.4.1. Press middle to finish.
