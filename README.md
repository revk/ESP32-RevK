# ESP32RevK

Library of tools used for ESP32 development.

This includes functions for management of settins in NVS using MQTT controls.

This includes Kconfig settings to enable/disable key functions, such as AP mode config, Multiple WiFi handling, MQTT handling, etc.

User guide: [Details of using devices that use this library](Manuals/revk-user.md)

Dev guide: [Details for apps using this library](Manuals/revk-dev.md)

## Flashing code

<img width="25%" align=right src="https://github.com/user-attachments/assets/0f6722e2-ea72-44d5-bd8a-17f9f7011313" />

The library is the basis for all of my ESP32 code, and provides a common infrastructure for settings, and software upgades, and even the basic building process usinmg `make` built around the `idf.py` (cmake) build system in ESP IDF.

Most of my designs have USB leads, but they also have a TC2030 port (see image on right) for a [TC2030-USB-NL](https://www.tag-connect.com/product/tc2030-usb-nl) lead.

My various code typically has a `release` and `betarelease` directory. In these are several `.bin` files.

- You can bnuild code with the whole ESP IDF environment, and use `idf.py` to flash.
- You can also use `esptool` to flash, but there is a simpler way for most people using a web page and Chrome browser.
- Or, there is a simpler way using Chrome!  

[https://adafruit.github.io/Adafruit_WebSerial_ESPTool/](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/)

The code to load, lets call the app `MyApp`, and assume we want to flash for the ESP32-S3-MINI-N4-R2 chip...

|Offset|File|
|----|----|
|0x0|MyApp-S3-MINI-N4-R2-bootloader.bin|
|0x8000|MyApp-S3-MINI-N4-R2-partition-table.bin|
|0xD000|MyApp-S3-MINI-N4-R2-ota_data_initial.bin|
|0x10000|MyApp-S3-MINI-N4-R2.bin|

