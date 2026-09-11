# ADS1118 reader for Seeed Studio XIAO ESP32-C6

This PlatformIO project samples `AIN0` of an ADS1118 and prints the raw 16-bit
ADC count and voltage, the ADS1118's internal temperature reading, and AHT20
temperature/humidity to the serial monitor at 115200 baud.

## Wiring

| ADS1118 | XIAO ESP32-C6 GPIO |
| --- | --- |
| CS | GPIO 18 |
| DIN / MOSI | GPIO 20 |
| DOUT / MISO | GPIO 19 |
| SCLK | GPIO 17 |
| GND | GND |
| VDD | 3.3 V |

| AHT20 | XIAO ESP32-C6 GPIO |
| --- | --- |
| SDA | GPIO 22 |
| SCL | GPIO 23 |
| GND | GND |
| VCC | 3.3 V |

The signal is read as `AIN0` relative to GND. The default PGA range is
plus/minus 4.096 V, but with a 3.3 V-powered ADS1118 the input must never exceed
the device supply rails. Change `readAdc(0)` to channels 1 through 3 to read a
different input.

The temperature reported by the sketch is the ADS1118 IC's internal sensor,
not an external probe. Its resolution is 0.03125 C; use it mainly to monitor
the ADC's own temperature rather than as a precision ambient measurement.

The AHT20 uses I2C address `0x38`. It provides the ambient-temperature and
relative-humidity values printed as `AHT20` in the serial monitor.

## Telnet JSON API

The ESP32-C6 joins the Wi-Fi network configured in `include/secrets.h` and
listens for Telnet/TCP connections on port 23. The credentials file is ignored
by Git; copy `include/secrets.example.h` to `include/secrets.h` when setting up
another checkout. Up to two simultaneous Telnet sessions are supported.
When a USB serial monitor connects, the firmware prints the assigned Wi-Fi IP
address and the ESP32-C6 Wi-Fi MAC address once.

Each received character returns one JSON object terminated by a newline:

| Character | Response |
| --- | --- |
| `1` to `4` | ADC result for AIN0 to AIN3, respectively |
| `t` | AHT20 temperature and relative humidity |
| `s` | AHT20 temperature in Fahrenheit, humidity, and AIN3 voltage |

For example, `t` returns `{"aht20":{"temperature_c":24.50,"temperature_f":76.10,"humidity_rh":42.3}}`.
The combined `s` command returns `{"temperature_f":76.10,"humidity_rh":42.3,"ain3_volts":1.234500, host:<MAC_ADDRESS>}`.

Build and upload from this directory with:

```sh
pio run --target upload
pio device monitor
```
