# Flashing the Pico

1. Install the Pico SDK and ARM toolchain. See [Useful Software Links](Useful%20Software%20Links.md).
2. Open PowerShell and configure the SDK path for the current window:

   ```powershell
   cd "C:\path\to\KaosDual-Extended\pico"
   $env:PICO_SDK_PATH = "C:\path\to\pico-sdk"
   cmake -S . -B build -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

3. Use `build\kaos_pico.uf2` for a Raspberry Pi Pico. For a Waveshare RP2040-Zero configure with `-DPICO_BOARD=waveshare_rp2040_zero`; its expected output may be renamed after the build if you keep both files.
4. Unplug the Pico. Hold **BOOTSEL**, connect it to the Windows PC, then release BOOTSEL.
5. Windows should show a removable drive named `RPI-RP2`. Copy the matching UF2 to that drive.
6. The board reboots automatically. Reconnect it to the portal host after flashing.

If `RPI-RP2` does not appear, try a known-good data cable and a direct USB port. Do not flash the Pico from ESP-IDF commands.
