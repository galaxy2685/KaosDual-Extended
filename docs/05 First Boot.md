# First Boot

1. Confirm the UART wiring in [Wiring](02%20Wiring.md).
2. Flash the Pico and ESP32. Power both boards.
3. On the phone or PC, join Wi-Fi **KAOS-Portal** using password **skylands1**.
4. Browse to `http://192.168.4.1`.
5. Wait for the library count. If the page shows **Library unavailable**, press **Rebuild Library**.

The access point name, password, and IP are configured in `esp32/main/main.c`. Change the name/password before building if you do not want to use the defaults. An empty count is expected in a clean public checkout until you add authorised dumps or upload User Added files.
