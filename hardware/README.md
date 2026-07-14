# NAMO Hardware

[한국어](README.ko.md)

This directory contains the reference hardware information and manufacturing files for NAMO.

## Build Gallery

| 1. PCB | 2. BH1750FVI light sensor |
| --- | --- |
| ![NAMO PCB](../docs/img/hardware/1.jpg) | ![NAMO BH1750FVI light sensor](../docs/img/hardware/2.jpg) |
| 3. FPC-SHT40 temperature and humidity sensor | 4. LD2450 radar |
| ![NAMO FPC-SHT40 sensor](../docs/img/hardware/3.jpg) | ![NAMO LD2450 radar](../docs/img/hardware/4.jpg) |
| 5. Rear view before closing the enclosure | 6. Final assembly |
| ![NAMO assembled hardware before closing the rear enclosure](../docs/img/hardware/5.jpg) | ![NAMO final assembly](../docs/img/hardware/6.jpg) |

## Supported Hardware

| Hardware | Required | Notes |
| --- | ---: | --- |
| Seeed Studio XIAO ESP32S3, 8MB Flash / 8MB PSRAM | Yes | Required by the current partition and memory configuration |
| LD2450-series mmWave radar | Yes | Primary presence and spatial tracking sensor |
| Panasonic EKMC1603111 PIR sensor | Optional | Uses GPIO1; an omitted input must remain electrically stable |
| BH1750 light sensor | Optional | Shares the I2C bus |
| SHT4x temperature and humidity sensor | Optional | Shares the I2C bus |

The firmware assumes an 8MB Flash / 8MB PSRAM model and a custom partition layout. ESP32-S3 boards with smaller flash, no PSRAM, or other ESP32 chip families such as ESP32-C6 may not work as-is.

## Pinout

The reference firmware and PCB use the following Seeed Studio XIAO ESP32S3 pin layout.

| Function | XIAO Label | ESP32 GPIO | Connected To |
| --- | --- | --- | --- |
| PIR input | D0 | GPIO1 | PIR sensor OUT |
| I2C SDA | D4 | GPIO5 | BH1750 SDA, SHT4x SDA |
| I2C SCL | D5 | GPIO6 | BH1750 SCL, SHT4x SCL |
| LD2450 UART TX | D6 | GPIO43 | LD2450 RX |
| LD2450 UART RX | D7 | GPIO44 | LD2450 TX |
| Status LED | Built-in LED | GPIO21 | XIAO built-in user LED |
| Reset button | BOOT | GPIO0 | XIAO BOOT button |

## Wiring Notes

The LD2450 TX/RX pins must be crossed with the XIAO UART pins: XIAO TX goes to LD2450 RX, and XIAO RX goes to LD2450 TX.

BH1750 and SHT4x share the same I2C bus. Check the operating voltage of each module before wiring power, and connect all module grounds to the XIAO GND.

The LD2450 is required for presence, target tracking, and zone detection. The firmware may boot without it, but those features will not work correctly.

The BH1750, SHT4x, and PIR sensors may be omitted. Their values will not be available and warnings may appear in the logs. If the PIR sensor is omitted, keep GPIO1 electrically stable or remove the PIR-related YAML configuration to prevent false triggers.

## PCB Manufacturing Files

- [Gerber archive](pcb/Gerber.zip)
- [Schematic](pcb/schematic.png)

Review the manufacturing archive and board requirements before ordering. The Gerber files are provided as reference manufacturing outputs for the documented pinout.

## Enclosure Models

- [Top enclosure](enclosure/top.3mf)
- [Bottom enclosure](enclosure/bottom.3mf)

Open the 3MF files in your slicer and verify scale, orientation, supports, and material settings before printing. Printer calibration and material shrinkage can affect the final fit.

## DIY Build Notes

You can build a compatible device with the same XIAO ESP32S3 board by following the documented pinout. Confirm every module's supply voltage and pin direction before applying power.

The PCB and enclosure files must be used with the firmware pinout documented above. If the circuit or connector layout changes, update both the firmware configuration and this guide together.

## License

Copyright (c) 2026 David2766.

The hardware design and manufacturing files in this directory are licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International license](LICENSE) (`CC BY-NC-SA 4.0`). Commercial use requires separate permission from the project author.
