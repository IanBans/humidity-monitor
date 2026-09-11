#include <Arduino.h>
#include <Adafruit_AHTX0.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>

#include "secrets.h"

constexpr int PIN_CS   = 18;
constexpr int PIN_MOSI = 20;
constexpr int PIN_MISO = 19;
constexpr int PIN_SCK  = 17;

// XIAO ESP32-C6 <-> AHT20 wiring
constexpr int PIN_I2C_SDA = 22;
constexpr int PIN_I2C_SCL = 23;

constexpr uint32_t SPI_CLOCK_HZ = 1'000'000;
constexpr float FULL_SCALE_VOLTS = 4.096f; // ADS1118 PGA setting: +/-4.096 V

SPIClass adcSpi(FSPI);
Adafruit_AHTX0 aht;
bool ahtAvailable = false;
WiFiServer telnetServer(23);
constexpr size_t MAX_TELNET_CLIENTS = 2;
WiFiClient telnetClients[MAX_TELNET_CLIENTS];
bool telnetServerStarted = false;
bool serialReportSent = false;
bool serialNetworkInfoSent = false;
uint32_t lastWiFiRetryMs = 0;

// ADS1118 config fields.  The device converts AINx relative to GND in
// single-ended mode.  AINx must remain between GND and VDD.
uint16_t makeConfig(uint8_t channel) {
  channel &= 0x03;
  const uint16_t mux = static_cast<uint16_t>(0x04 + channel) << 12;
  return 0x8000 |              // OS: start a single conversion
         mux |                 // MUX: AIN0..AIN3 versus GND
         0x0200 |              // PGA: +/-4.096 V
         0x0100 |              // MODE: single-shot (low power between samples)
         0x0080 |              // DR: 128 samples/second
         0x0003;               // NOP[1:0]=01: accept this configuration word
}

// The ADS1118's internal sensor returns a signed 14-bit value with a
// resolution of 0.03125 degrees C per count.  Temperature mode ignores MUX.
uint16_t makeTemperatureConfig() {
  return 0x8000 |              // OS: start a single conversion
         0x0200 |              // PGA: required configuration field
         0x0100 |              // MODE: single-shot (low power between samples)
         0x0080 |              // DR: 128 samples/second
         0x0010 |              // TS_MODE: internal temperature sensor
         0x0003;               // NOP[1:0]=01: accept this configuration word
}

uint16_t transferWord(uint16_t word) {
  adcSpi.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_CS, LOW);
  const uint16_t received = adcSpi.transfer16(word);
  digitalWrite(PIN_CS, HIGH);
  adcSpi.endTransaction();
  return received;
}

// Start one conversion, wait for it to complete, then clock its result out.
int16_t readAdc(uint8_t channel) {
  const uint16_t config = makeConfig(channel);
  transferWord(config);
  delay(9); // 128 SPS conversion takes at most 7.8125 ms
  return static_cast<int16_t>(transferWord(config));
}

float readTemperatureC() {
  const uint16_t config = makeTemperatureConfig();
  transferWord(config);
  delay(9); // 128 SPS conversion takes at most 7.8125 ms

  // The result is left-aligned in bits 15:2. Sign-extend the 14-bit value.
  int32_t counts = transferWord(config) >> 2;
  if (counts & 0x2000) {
    counts -= 0x4000;
  }
  return counts * 0.03125f;
}

void sendJsonError(WiFiClient &client, const char *message) {
  client.printf("{\"error\":\"%s\"}\r\n", message);
}

void respondToTelnetCommand(WiFiClient &client, char command) {
  if (command >= '1' && command <= '4') {
    const uint8_t channel = command - '1';
    const int16_t counts = readAdc(channel);
    const float volts = counts * (FULL_SCALE_VOLTS / 32768.0f);
    client.printf(
        "{\"adc\":{\"channel\":%u,\"counts\":%d,\"volts\":%.6f}}\r\n",
        channel, counts, volts);
    return;
  }

  if (command == 't' || command == 'T') {
    if (!ahtAvailable) {
      sendJsonError(client, "AHT20 unavailable");
      return;
    }

    sensors_event_t humidity;
    sensors_event_t temperature;
    aht.getEvent(&humidity, &temperature);
    client.printf(
        "{\"aht20\":{\"temperature_c\":%.2f,\"temperature_f\":%.2f,\"humidity_rh\":%.1f}}\r\n",
        temperature.temperature, temperature.temperature * 1.8f + 32.0f,
        humidity.relative_humidity);
    return;
  }

  if (command == 's' || command == 'S') {
    if (!ahtAvailable) {
      sendJsonError(client, "AHT20 unavailable");
      return;
    }

    sensors_event_t humidity;
    sensors_event_t temperature;
    aht.getEvent(&humidity, &temperature);

    const int16_t adcCounts = readAdc(3);
    const float adcVolts = adcCounts * (FULL_SCALE_VOLTS / 32768.0f);
    client.printf(
      "{\"temperature_f\":%.2f,\"humidity_rh\":%.1f,\"ain3_volts\":%.6f, \"host\":\"%s\"}\r\n",
        temperature.temperature * 1.8f + 32.0f, humidity.relative_humidity,
        adcVolts, WiFi.macAddress().c_str());
    return;
  }

  sendJsonError(client, "commands: 1, 2, 3, 4, t, s");
}

void serviceTelnet() {
  if (!telnetServerStarted) {
    return;
  }

  // Release disconnected slots before accepting new sessions.
  for (WiFiClient &client : telnetClients) {
    if (client && !client.connected()) {
      client.stop();
    }
  }

  WiFiClient newClient = telnetServer.accept();
  if (newClient) {
    bool accepted = false;
    for (WiFiClient &client : telnetClients) {
      if (!client) {
        client = newClient;
        client.println("{\"ready\":true,\"commands\":[\"1\",\"2\",\"3\",\"4\",\"t\",\"s\"]}");
        accepted = true;
        break;
      }
    }
    if (!accepted) {
      newClient.println("{\"error\":\"maximum of two Telnet sessions reached\"}");
      newClient.stop();
    }
  }

  for (WiFiClient &client : telnetClients) {
    while (client && client.available()) {
      const char command = static_cast<char>(client.read());
      if (command != '\r' && command != '\n') {
        respondToTelnetCommand(client, command);
      }
    }
  }
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!telnetServerStarted) {
      telnetServer.begin();
      telnetServerStarted = true;
    }
    return;
  }

  serialNetworkInfoSent = false;
  if (millis() - lastWiFiRetryMs >= 10'000) {
    lastWiFiRetryMs = millis();
    WiFi.reconnect();
  }
}

// Reprint this one line when a USB serial monitor reconnects, but do not
// continuously log network information.
void serviceSerialNetworkInfo() {
  // USB Serial/JTAG does not reliably expose terminal open/close state on all
  // hosts, so report as soon as Wi-Fi is ready rather than gating on Serial.
  if (!serialNetworkInfoSent && WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi IP: %s | MAC: %s\n", WiFi.localIP().toString().c_str(),
                  WiFi.macAddress().c_str());
    serialNetworkInfoSent = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  adcSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  ahtAvailable = aht.begin(&Wire);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiRetryMs = millis();

}

void loop() {
  serviceWiFi();
  serviceSerialNetworkInfo();
  serviceTelnet();

  if (!serialReportSent) {
    // Change 0 to 1, 2, or 3 to select the corresponding ADS1118 AIN pin.
    const int16_t counts = readAdc(0);
    const float volts = counts * (FULL_SCALE_VOLTS / 32768.0f);
    const float adsTemperatureC = readTemperatureC();
    Serial.printf("AIN0: %d counts, %.4f V | ADS1118: %.2f C (%.2f F)\n",
                  counts, volts, adsTemperatureC, adsTemperatureC * 1.8f + 32.0f);

    if (ahtAvailable) {
    sensors_event_t humidity;
    sensors_event_t temperature;
    aht.getEvent(&humidity, &temperature);
    Serial.printf("AHT20: %.2f C (%.2f F), %.1f %%RH\n",
                  temperature.temperature, temperature.temperature * 1.8f + 32.0f,
                  humidity.relative_humidity);
    } else {
      Serial.println("AHT20 unavailable");
    }
    serialReportSent = true;
  }

  delay(500);
}
