# Wiring

## ESP32 to Pico UART

| ESP32 | Pico | Purpose |
|---|---|---|
| GPIO17 (TX) | GPIO5 (RX) | ESP32 → Pico UART data |
| GPIO16 (RX) | GPIO4 (TX) | Pico → ESP32 UART data |
| GND | GND | Common ground |
| VIN | PIN40 VBUS | 5Vs |

These are the pins configured in `esp32/main/pico_bridge.c` and `pico/main.c`; UART speed is 921600 baud. TX and RX must be crossed. Both UARTs use 3.3 V logic—never attach them directly to 5 V TTL signals.

## Optional LCD1602 with PCF8574 backpack

| LCD signal | ESP32 GPIO |
|---|---|
| SDA | 21 |
| SCL | 22 |

The configured I²C address is `0x27`; change `LCD_I2C_ADDR` in `esp32/main/main.c` to `0x3F` only if your display uses that address.

Power each board from an appropriate USB source. The required connection between the boards is the three-wire UART/ground link above.
