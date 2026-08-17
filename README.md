# ESP32 NAT Extender

ESP32 dual-core Wi-Fi AP+STA NAT/NAPT extender for testing the router software on a standard 4 MB ESP32 before moving the same application architecture to the ESP32-S3 hardware.

## Core operation

- STA connects to the upstream 2.4 GHz Wi-Fi.
- AP provides the local extender network at `192.168.4.1`.
- NAT/NAPT forwards client traffic through the STA uplink.
- Normal reconnect scans only channels **1 / 6 / 11**.
- Full reconnect mode scans **1–13**.
- Failed recovery uses bounded exponential backoff; a successful connection immediately resets it.
- The AP follows the STA channel because the ESP32 uses one 2.4 GHz radio.

## Dashboard

The local dashboard provides live uplink status, RSSI/signal, channel, clients, traffic, CPU load, heap, temperature availability, fan state, NAT state and reconnect mode. Login unlocks configuration, diagnostics, OTA and maintenance controls.

## Hardware outputs

The standard-ESP32 build uses these default GPIOs:

| Function | GPIO |
|---|---:|
| Red LED | 25 |
| Yellow LED | 26 |
| Green LED | 27 |
| 5 V fan control | 33 |

LEDs indicate poor/medium/good uplink signal. The standard ESP32 test target has no compatible on-chip temperature-sensor API in this firmware, so temperature is reported as unavailable and automatic thermal fan control remains off. The 65 °C / 55 °C hysteresis is reserved for the ESP32-S3 target with its supported temperature sensor.

A 5 V fan must be driven through a suitable transistor/MOSFET driver; never power the fan directly from a GPIO.

## Build

Target: **ESP32**, 4 MB flash, 240 MHz dual core. The CI build uses ESP-IDF `release-v6.1`.

```bash
idf.py set-target esp32
idf.py build
idf.py size
idf.py merge-bin -o build/merged.bin -f raw
```

## Flash configuration

- Flash size: **4 MB**
- Flash mode: **DIO**
- Flash frequency: **40 MHz**
- Flash baud: **460800**

The merged image is flashed at `0x000000`.

```bash
python -m esptool --chip esp32 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 4MB --flash-freq 40m \
  0x0 build/merged.bin
```

Individual images use the addresses generated in `build/flash_args`; the application starts at `0x20000`.

## Partition layout

| Address | Size | Purpose |
|---:|---:|---|
| `0x9000` | 24 KiB | NVS |
| `0xF000` | 8 KiB | OTA data |
| `0x11000` | 4 KiB | PHY data |
| `0x20000` | 1.5 MiB | OTA 0 |
| `0x1A0000` | 1.5 MiB | OTA 1 |
| `0x320000` | 832 KiB | LittleFS |

## Notes

This build is intended for immediate functional testing on a normal ESP32. The ESP32-S3-specific PSRAM configuration is removed; PSRAM is reported as unavailable on this target. GPIO assignments are deliberately changed to pins suitable for the standard ESP32 and must still be checked against the exact development board before wiring.
