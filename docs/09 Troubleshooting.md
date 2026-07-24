# Troubleshooting

## Build and USB

- Use a data-capable USB cable. Charge-only cables cause missing COM ports and a missing `RPI-RP2` drive.
- If the ESP32 COM port is unclear, unplug/replug it and compare Device Manager’s Ports list.
- If a build reports Ninja locks or stale configuration, close other terminals. For ESP32 use `idf.py fullclean`; for Pico remove only the repository’s `pico\build` folder and configure again.

## WebUI and library

- Join `KAOS-Portal` and browse to `http://192.168.4.1`. Temporarily disabling mobile data can help a phone stay on a local access point without internet.
- **Library unavailable:** press Rebuild Library. If it repeats, inspect ESP32 serial output and verify SPIFFS mounted.
- **Empty library:** valid in a clean checkout. Add authorised 1,024-byte `.sky` files under `esp32\skylandersDumps` and run full `flash`, or upload through User Added.
- **File rejected:** check the extension and exact 1,024-byte size.

## Portal and editor

- Check crossed UART wiring: ESP32 GPIO17→Pico GPIO5, ESP32 GPIO16←Pico GPIO4, and common ground.
- If the editor is unavailable, the entry may be unsupported or loaded. Unload it; unsupported special types remain intentionally read-only.
- A loaded file cannot be deleted, edited, restored, or replaced. Unload it first.
- If runtime content disappeared after flashing, you likely ran full `flash`, which rewrites SPIFFS. Restore from a raw download or backup.
