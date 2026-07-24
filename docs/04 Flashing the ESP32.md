# Flashing the ESP32

Install ESP-IDF, then start **ESP-IDF PowerShell**. Find the ESP32 port in Device Manager under **Ports (COM & LPT)**.

```powershell
cd "C:\path\to\KaosDual-Extended\esp32"
idf.py reconfigure
idf.py build
idf.py -p COM3 flash
```

Replace `COM3` with your port. The project targets `esp32` and uses a 4 MB partition layout with a 2 MB SPIFFS partition.

## Flash versus app-flash

- `idf.py -p COM3 flash` writes the application and SPIFFS. Use it for the first install and after changing files in `esp32\skylandersDumps`.
- `idf.py -p COM3 app-flash` writes only the application. It preserves runtime SPIFFS content such as saves, User Added files, and favourites.

Before a full flash, download any saves you care about. Full flash replaces SPIFFS with the library packaged during the build.
