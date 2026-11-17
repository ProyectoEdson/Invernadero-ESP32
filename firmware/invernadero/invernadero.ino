#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <math.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Este firmware requiere un ESP32-S3."
#endif

#if __has_include("wifi.local.h")
#include "wifi.local.h"
#else
#error "Copia wifi.example.h como wifi.local.h y configura tu red de 2,4 GHz."
#endif

static_assert(sizeof(WIFI_SSID) > 1, "Configura WIFI_SSID en wifi.local.h.");
static_assert(sizeof(WIFI_SSID) <= 33, "El SSID admite como máximo 32 bytes.");

constexpr uint8_t DHT_PIN = 15;
constexpr gpio_num_t PELTIER_PIN = GPIO_NUM_4;
constexpr gpio_num_t PUMP_PIN = GPIO_NUM_5;
constexpr gpio_num_t FANS_PIN = GPIO_NUM_7;
constexpr gpio_num_t OUTPUT_PINS[] = {PELTIER_PIN, PUMP_PIN, FANS_PIN};
constexpr uint32_t SAMPLE_INTERVAL_MS = 3000;
constexpr uint32_t NETWORK_CHECK_INTERVAL_MS = 250;
constexpr uint32_t INITIAL_RETRY_MS = 15000;
constexpr uint32_t MAX_RETRY_MS = 60000;
constexpr uint32_t EXPECTED_FLASH_BYTES = 16U * 1024U * 1024U;
constexpr uint32_t EXPECTED_PSRAM_BYTES = 8U * 1024U * 1024U;

DHT dht(DHT_PIN, DHT11);
char deviceId[17];
char bootId[33];
uint32_t sequence = 0;
uint32_t lastSampleAt = 0;
uint32_t lastNetworkCheckAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t retryDelayMs = INITIAL_RETRY_MS;
bool wasConnected = false;
bool memoryConfigurationOk = false;

static void initializeSafeOutputs() {
  // Primero poner a cero el latch; luego habilitar la salida. Los pull-down
  // físicos siguen siendo necesarios antes de que se ejecute setup().
  for (gpio_num_t pin : OUTPUT_PINS) {
    ESP_ERROR_CHECK(gpio_set_level(pin, 0));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT));
  }
}

static void initializeIdentity() {
  const uint64_t mac = ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL;
  snprintf(deviceId, sizeof(deviceId), "inv-%012llx", (unsigned long long)mac);
  uint8_t randomBytes[16];
  esp_fill_random(randomBytes, sizeof(randomBytes));
  for (size_t i = 0; i < sizeof(randomBytes); ++i) {
    snprintf(bootId + i * 2, 3, "%02x", randomBytes[i]);
  }
}

static void reportBoot() {
  const uint32_t flashBytes = ESP.getFlashChipSize();
  const uint32_t psramBytes = ESP.getPsramSize();
  memoryConfigurationOk = flashBytes == EXPECTED_FLASH_BYTES &&
                          psramBytes == EXPECTED_PSRAM_BYTES;
  Serial.printf(
      "{\"type\":\"boot\",\"firmware\":\"phase2\",\"device_id\":\"%s\","
      "\"boot_id\":\"%s\",\"flash_bytes\":%lu,\"psram_bytes\":%lu,"
      "\"memory_config_ok\":%s,\"dht_gpio\":15,\"peltier_locked\":true}\n",
      deviceId, bootId, (unsigned long)flashBytes, (unsigned long)psramBytes,
      memoryConfigurationOk ? "true" : "false");
}

static void serviceNetwork(uint32_t now) {
  if (uint32_t(now - lastNetworkCheckAt) < NETWORK_CHECK_INTERVAL_MS) {
    return;
  }
  lastNetworkCheckAt = now;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) {
      retryDelayMs = INITIAL_RETRY_MS;
      const IPAddress ip = WiFi.localIP();
      Serial.printf(
          "{\"type\":\"wifi\",\"status\":\"connected\",\"ip\":\"%u.%u.%u.%u\"}\n",
          ip[0], ip[1], ip[2], ip[3]);
    } else {
      lastWifiAttemptAt = now;
      Serial.println("{\"type\":\"wifi\",\"status\":\"disconnected\"}");
    }
  }
  if (!connected && uint32_t(now - lastWifiAttemptAt) >= retryDelayMs) {
    lastWifiAttemptAt = now;
    WiFi.reconnect();
    retryDelayMs = retryDelayMs < MAX_RETRY_MS / 2
                       ? retryDelayMs * 2
                       : MAX_RETRY_MS;
    Serial.println("{\"type\":\"wifi\",\"status\":\"retry_requested\"}");
  }
}

static void readAndReport() {
  // La biblioteca reutiliza la misma adquisición para ambas magnitudes.
  const float humidity = dht.readHumidity();
  const float temperature = dht.readTemperature();
  const bool readOk = isfinite(temperature) && isfinite(humidity);
  const bool valid = readOk && temperature >= 0.0f && temperature <= 50.0f &&
                     humidity >= 0.0f && humidity <= 100.0f;
  const char *sensorStatus = !readOk ? "read_error" : valid ? "ok" : "out_of_range";
  char temperatureText[16] = "null";
  char humidityText[16] = "null";
  if (valid) {
    snprintf(temperatureText, sizeof(temperatureText), "%.1f", temperature);
    snprintf(humidityText, sizeof(humidityText), "%.1f", humidity);
  }
  const int peltierLevel = gpio_get_level(PELTIER_PIN);
  const int pumpLevel = gpio_get_level(PUMP_PIN);
  const int fansLevel = gpio_get_level(FANS_PIN);
  const bool outputsSafe = peltierLevel == 0 && pumpLevel == 0 && fansLevel == 0;
  ++sequence;
  Serial.printf(
      "{\"type\":\"sample\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
      "\"sequence\":%lu,\"uptime_ms\":%llu,\"sensor_status\":\"%s\","
      "\"temperature_c\":%s,\"humidity_pct\":%s,\"wifi_connected\":%s,"
      "\"memory_config_ok\":%s,\"peltier_locked\":true,"
      "\"gpio_levels\":{\"peltier\":%d,\"pump\":%d,\"fans\":%d},"
      "\"outputs_safe\":%s}\n",
      deviceId, bootId, (unsigned long)sequence,
      (unsigned long long)(esp_timer_get_time() / 1000), sensorStatus,
      temperatureText, humidityText, wasConnected ? "true" : "false",
      memoryConfigurationOk ? "true" : "false", peltierLevel, pumpLevel,
      fansLevel, outputsSafe ? "true" : "false");
}

void setup() {
  initializeSafeOutputs();
  Serial.begin(115200);
  dht.begin();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  initializeIdentity();
  reportBoot();
  // Inicia la asociación sin esperar en un while; el muestreo sigue su reloj.
  lastWifiAttemptAt = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastSampleAt = millis();
}

void loop() {
  const uint32_t now = millis();
  serviceNetwork(now);
  if (uint32_t(now - lastSampleAt) >= SAMPLE_INTERVAL_MS) {
    lastSampleAt = now;
    readAndReport();
  }
  delay(10);
}
