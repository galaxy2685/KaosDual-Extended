# Hardware Needed

## Required

|Item|Why it is needed|
|-|-|
|ESP32 DevKit V1-class board with 4 MB flash|Hosts the access point, WebUI, library, SPIFFS storage, and UART bridge. The supplied partition layout is for 4 MB flash.|
|Raspberry Pi Pico|Presents the USB Skylanders portal HID device.|
|4 jumper wires|Two UART signal wires, wire to connect power and a shared ground.|
|Two USB data cables|One for ESP32 power/flashing and one for the Pico/host portal connection.|
|Windows PC|Used by these flashing instructions.|

## Optional

A PCF8574-backed LCD1602 can show access-point information. It is not needed to use the browser UI.

Do not use charge-only USB cables. The Pico must use a cable/port that carries USB data to the console or PC.
