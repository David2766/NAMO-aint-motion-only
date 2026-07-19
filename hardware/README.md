# NAMO Hardware

[한국어](README.ko.md)

This directory contains the reference hardware information and manufacturing files for NAMO.

> [!IMPORTANT]
> The current dual-radar PCB manufacturing package and schematic are now published under [`pcb/dual_sensor/`](pcb/dual_sensor/). The enclosure files and assembled-device photos still belong to the earlier single-radar generation. A dual-radar enclosure is being refined and is not yet published; do not print the legacy enclosure for the current dual-radar board.

## Build Gallery

| 1. PCB | 2. BH1750FVI light sensor |
| --- | --- |
| ![NAMO PCB](../docs/img/hardware/1.jpg) | ![NAMO BH1750FVI light sensor](../docs/img/hardware/2.jpg) |
| 3. FPC-SHT40 temperature and humidity sensor | 4. LD2450 radar |
| ![NAMO FPC-SHT40 sensor](../docs/img/hardware/3.jpg) | ![NAMO LD2450 radar](../docs/img/hardware/4.jpg) |
| 4-1. LD2410C static-presence radar | 5. Rear view before closing the enclosure |
| ![NAMO LD2410C static-presence radar](../docs/img/hardware/4-1.jpg) | ![NAMO assembled hardware before closing the rear enclosure](../docs/img/hardware/5.jpg) |
| 6. Final assembly | |
| ![NAMO final assembly](../docs/img/hardware/6.jpg) | |

Photos 1-6 document the earlier single-radar build. Photo 4-1 shows the optional LD2410C used by the current dual-radar design. Updated dual-radar enclosure and final-assembly photos will be added after the enclosure is finalized.

## Supported Hardware

| Hardware | Required | Notes |
| --- | ---: | --- |
| Seeed Studio XIAO ESP32S3, 8MB Flash / 8MB PSRAM | Yes | Required by the current partition and memory configuration |
| LD2450-series mmWave radar | Yes | Presence acquisition and spatial tracking sensor |
| LD2410C static-presence radar | Optional | Maintains an already established presence session when available |
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
| LD2410C UART TX | D1 | GPIO2 | LD2410C RX |
| LD2410C UART RX | D2 | GPIO3 | LD2410C TX |
| LD2450 UART TX | D6 | GPIO43 | LD2450 RX |
| LD2450 UART RX | D7 | GPIO44 | LD2450 TX |
| Status LED | Built-in LED | GPIO21 | XIAO built-in user LED |
| Reset button | BOOT | GPIO0 | XIAO BOOT button |

## Wiring Notes

The LD2450 and LD2410C TX/RX pins must be crossed with the corresponding XIAO UART pins: XIAO TX goes to radar RX, and XIAO RX goes to radar TX.

GPIO3 is an ESP32-S3 strapping pin. The reference board routes it to LD2410C TX; avoid adding external pull-up or pull-down resistors that can force an invalid boot strap level.

BH1750 and SHT4x share the same I2C bus. Check the operating voltage of each module before wiring power, and connect all module grounds to the XIAO GND.

The LD2450 is required for presence, target tracking, and zone detection. The firmware may boot without it, but those features will not work correctly.

LD2410C support is optional at runtime. When present, it may maintain a presence session that PIR or confirmed LD2450 tracking already started. It cannot start presence by itself and does not create motion, target coordinates, target counts, room state, or zone presence. Raw presence, moving/still distances and energy remain available for diagnostics; replay also records the stabilized canonical detection distance and assist state.

Assistance arms only after LD2410C detection overlaps PIR or confirmed LD2450 presence for 2 seconds. Confirmed exit evidence and filter blocking veto the assistance. Without either veto, an armed LD2410C has a 30-second reentry window for the same recently ended session. Its canonical detection distance uses a three-sample median. Missing hardware is ignored, and the dashboard tuning session enables per-gate engineering telemetry only while the tuning panel is open.

The BH1750, SHT4x, and PIR sensors may be omitted. Their values will not be available and warnings may appear in the logs. If the PIR sensor is omitted, keep GPIO1 electrically stable or remove the PIR-related YAML configuration to prevent false triggers.

## PCB Manufacturing Files

The current dual-radar design adds the optional LD2410C UART connection documented in the pinout above. The previous single-radar board remains available as a legacy reference.

- Current dual-radar board: [Gerber archive](pcb/dual_sensor/Gerber.zip) · [Schematic](pcb/dual_sensor/schematic.png)
- Legacy single-radar board: [Gerber archive](pcb/single_sensor/Gerber.zip) · [Schematic](pcb/single_sensor/schematic.png)

Review the selected manufacturing archive, schematic, supply voltages, connector orientation, and board requirements before ordering. Use the dual-radar package when building the current LD2450 + LD2410C reference hardware.

## Enclosure Models

The following enclosure files fit the earlier single-radar hardware generation. The updated enclosure for the dual-radar layout is still being refined and is not yet published.

- [Top enclosure](enclosure/top.3mf)
- [Bottom enclosure](enclosure/bottom.3mf)

Open the 3MF files in your slicer and verify scale, orientation, supports, and material settings before printing. Printer calibration and material shrinkage can affect the final fit.

These legacy models are not expected to fit the current dual-radar PCB and sensor arrangement without modification.

## DIY Build Notes

You can build a compatible device with the same XIAO ESP32S3 board by following the documented pinout. Confirm every module's supply voltage and pin direction before applying power.

The PCB and enclosure files must be used with the firmware pinout documented above. If the circuit or connector layout changes, update both the firmware configuration and this guide together.

## License

Copyright (c) 2026 David2766.

The hardware design and manufacturing files in this directory are licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International license](LICENSE) (`CC BY-NC-SA 4.0`). Commercial use requires separate permission from the project author.
