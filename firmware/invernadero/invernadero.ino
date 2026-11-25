#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Este firmware requiere un ESP32-S3."
#endif

#if __has_include("cloud.local.h")
#include "cloud.local.h"
#else
#error "Copia cloud.example.h como cloud.local.h y configura el token."
#endif

#include "cruchi_tls.h"
#include "wifi_setup.h"

static_assert(sizeof(CLOUD_DEVICE_TOKEN) > 1, "Configura CLOUD_DEVICE_TOKEN en cloud.local.h.");

#define PELTIER_PIN GPIO_NUM_45  // Q1: Peltier (J5) y ventiladores (J8).
#define PUMP_PIN GPIO_NUM_6      // Q2: bomba.
#define LEDS_PIN GPIO_NUM_5      // Q3: luces LED.
#define DHT_PIN 4
#define BH1750_SDA_PIN 7
#define BH1750_SCL_PIN 9
#define BH1750_ADDRESS 0x23

constexpr gpio_num_t OUTPUT_PINS[] = {PELTIER_PIN, PUMP_PIN, LEDS_PIN};
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;
constexpr uint8_t BH1750_ONE_TIME_HIGH_RES = 0x20;
constexpr uint32_t BH1750_CONVERSION_MS = 180;
constexpr uint32_t BH1750_RETRY_MS = 1000;
constexpr uint16_t BH1750_I2C_TIMEOUT_MS = 20;
constexpr uint32_t NETWORK_CHECK_INTERVAL_MS = 250;
constexpr uint32_t INITIAL_RETRY_MS = 15000;
constexpr uint32_t MAX_RETRY_MS = 60000;
constexpr uint32_t EXPECTED_FLASH_BYTES = 16U * 1024U * 1024U;
constexpr uint32_t EXPECTED_PSRAM_BYTES = 8U * 1024U * 1024U;
constexpr uint32_t MAX_SAMPLE_AGE_MS = 15000;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t HTTP_IO_TIMEOUT_MS = 5000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 5;
constexpr time_t MIN_VALID_UNIX_TIME = 1735689600;  // 2025-01-01 UTC.
constexpr size_t SAMPLE_JSON_CAPACITY = 1024;
constexpr size_t RESPONSE_JSON_CAPACITY = 1024;

struct TelemetrySample {
  uint64_t uptimeMs;
  uint32_t sequence;
  float temperature;
  float humidity;
  float illuminanceLux;
  const char *sensorStatus;
  int peltierLevel;
  int pumpLevel;
  int ledsLevel;
  bool valid;
  bool wifiConnected;
  bool outputsSafe;
};

struct CloudReport {
  const char *status;
  int httpStatus;
  int transportCode;
  int accepted;  // -1: no confirmado; 0/1 solo tras respuesta válida.
};

// Declaraciones explícitas para el preprocesador de sketches de Arduino:
// los prototipos con estos tipos deben ir después de definir los structs.
class CloudResponseBuffer;
static size_t formatSample(char *buffer, size_t capacity,
                           const TelemetrySample &sample, bool sending,
                           uint64_t sentUptimeMs = 0);
static bool parseCloudResponse(const CloudResponseBuffer &response,
                               bool &accepted);
static bool sendCloudSample(const TelemetrySample &sample);
static void publishCloudReport(const char *status, int httpStatus = 0,
                               int transportCode = 0, int accepted = -1);

QueueHandle_t cloudSamples = nullptr;
QueueHandle_t cloudReports = nullptr;

DHT dht(DHT_PIN, DHT11);
char deviceId[17];
char bootId[33];
uint32_t sequence = 0;
uint32_t lastSampleAt = 0;
bool memoryConfigurationOk = false;
bool lightBusReady = false;
bool lightConversionPending = false;
uint32_t nextLightActionAt = 0;
float illuminanceLux = NAN;

static void serviceIlluminance(uint32_t now) {
  if (int32_t(now - nextLightActionAt) < 0) {
    return;
  }
  if (lightConversionPending) {
    lightConversionPending = false;
    if (Wire.requestFrom(BH1750_ADDRESS, size_t(2)) == 2) {
      const uint16_t highByte = Wire.read();
      const uint16_t raw = (highByte << 8) | uint16_t(Wire.read());
      // MTreg por defecto: raw / 1.2. El máximo de 54612.5 es exacto.
      illuminanceLux = float(raw) * 5.0f / 6.0f;
      nextLightActionAt += SAMPLE_INTERVAL_MS - BH1750_CONVERSION_MS;
      return;
    }
  } else {
    if (!lightBusReady) {
      lightBusReady = Wire.begin(BH1750_SDA_PIN, BH1750_SCL_PIN);
      if (lightBusReady) {
        Wire.setTimeOut(BH1750_I2C_TIMEOUT_MS);
      }
    }
    if (lightBusReady) {
      Wire.beginTransmission(BH1750_ADDRESS);
      Wire.write(BH1750_ONE_TIME_HIGH_RES);
      if (Wire.endTransmission() == 0) {
        // Medición única: esperar el máximo sin detener el loop.
        lightConversionPending = true;
        nextLightActionAt = millis() + BH1750_CONVERSION_MS;
        return;
      }
    }
  }
  illuminanceLux = NAN;
  nextLightActionAt = millis() + BH1750_RETRY_MS;
}

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
      "{\"type\":\"boot\",\"firmware\":\"pcb-monitoring\",\"device_id\":\"%s\","
      "\"boot_id\":\"%s\",\"flash_bytes\":%lu,\"psram_bytes\":%lu,"
      "\"memory_config_ok\":%s,\"dht_gpio\":%d,\"peltier_locked\":true}\n",
      deviceId, bootId, (unsigned long)flashBytes, (unsigned long)psramBytes,
      memoryConfigurationOk ? "true" : "false", DHT_PIN);
}

static size_t formatSample(char *buffer, size_t capacity,
                           const TelemetrySample &sample, bool sending,
                           uint64_t sentUptimeMs) {
  char temperatureText[16] = "null";
  char humidityText[16] = "null";
  if (sample.valid) {
    snprintf(temperatureText, sizeof(temperatureText), "%.1f", sample.temperature);
    snprintf(humidityText, sizeof(humidityText), "%.1f", sample.humidity);
  }
  char illuminanceText[16] = "null";
  if (isfinite(sample.illuminanceLux)) {
    snprintf(illuminanceText, sizeof(illuminanceText), "%.2f", sample.illuminanceLux);
  }
  const int length = snprintf(
      buffer, capacity,
      "{\"type\":\"sample\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
      "\"sequence\":%lu,\"uptime_ms\":%llu,\"sensor_status\":\"%s\","
      "\"temperature_c\":%s,\"humidity_pct\":%s,\"illuminance_lux\":%s,"
      "\"wifi_connected\":%s,\"memory_config_ok\":%s,\"peltier_locked\":true,"
      "\"gpio_levels\":{\"peltier\":%d,\"pump\":%d,\"leds\":%d},"
      "\"outputs_safe\":%s,\"operating_mode\":\"monitoring\","
      "\"phase4_control_ready\":false",
      deviceId, bootId, (unsigned long)sample.sequence,
      (unsigned long long)sample.uptimeMs, sample.sensorStatus,
      temperatureText, humidityText, illuminanceText,
      sample.wifiConnected ? "true" : "false",
      memoryConfigurationOk ? "true" : "false", sample.peltierLevel,
      sample.pumpLevel, sample.ledsLevel, sample.outputsSafe ? "true" : "false");
  if (length < 0 || size_t(length) >= capacity) {
    return 0;
  }
  const int tailLength = sending
      ? snprintf(buffer + length, capacity - length,
                 ",\"sent_uptime_ms\":%llu}", (unsigned long long)sentUptimeMs)
      : snprintf(buffer + length, capacity - length, "}");
  if (tailLength < 0 || size_t(tailLength) >= capacity - length) {
    return 0;
  }
  return size_t(length + tailLength);
}

static void publishCloudReport(const char *status, int httpStatus,
                               int transportCode, int accepted) {
  const CloudReport report = {status, httpStatus, transportCode, accepted};
  xQueueSend(cloudReports, &report, portMAX_DELAY);
}

static void reportCloud() {
  CloudReport report;
  if (cloudReports == nullptr ||
      xQueueReceive(cloudReports, &report, 0) != pdTRUE) {
    return;
  }
  char httpStatus[16] = "null";
  char transportCode[16] = "null";
  if (report.httpStatus > 0) {
    snprintf(httpStatus, sizeof(httpStatus), "%d", report.httpStatus);
  }
  if (report.transportCode < 0) {
    snprintf(transportCode, sizeof(transportCode), "%d", report.transportCode);
  }
  // Solo loop escribe en Serial: ningún worker puede partir una línea JSON.
  Serial.printf(
      "{\"type\":\"cloud\",\"status\":\"%s\",\"http_status\":%s,"
      "\"transport_code\":%s,\"accepted\":%s}\n",
      report.status, httpStatus, transportCode,
      report.accepted < 0 ? "null" : report.accepted ? "true" : "false");
}

class CloudResponseBuffer : public Stream {
 public:
  char data[RESPONSE_JSON_CAPACITY] = {};
  size_t length = 0;
  uint32_t startedAt = millis();

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (uint32_t(millis() - startedAt) >= HTTP_IO_TIMEOUT_MS ||
        size > sizeof(data) - 1 - length) {
      return 0;
    }
    memcpy(data + length, buffer, size);
    length += size;
    data[length] = '\0';
    return size;
  }
};

static void skipJsonSpace(const char *&cursor) {
  while (*cursor == ' ' || *cursor == '\t' ||
         *cursor == '\r' || *cursor == '\n') {
    ++cursor;
  }
}

static bool consumeResponseToken(const char *&cursor, const char *token,
                                 size_t length) {
  if (strncmp(cursor, token, length) != 0) {
    return false;
  }
  cursor += length;
  return true;
}

// CruChi solo entrega estos tres campos. Rechazar duplicados, valores de
// otros tipos, órdenes no nulas y contenido después del cierre del objeto.
static bool parseCloudResponse(const CloudResponseBuffer &response,
                               bool &accepted) {
  if (response.length == 0 ||
      memchr(response.data, '\0', response.length) != nullptr) {
    return false;
  }
  const char *cursor = response.data;
  skipJsonSpace(cursor);
  if (*cursor++ != '{') {
    return false;
  }
  enum { Ok = 1, Accepted = 2, Command = 4 };
  unsigned int seen = 0;
  for (unsigned int field = 0; field < 3; ++field) {
    skipJsonSpace(cursor);
    unsigned int key;
    if (consumeResponseToken(cursor, "\"ok\"", 4)) {
      key = Ok;
    } else if (consumeResponseToken(cursor, "\"accepted\"", 10)) {
      key = Accepted;
    } else if (consumeResponseToken(cursor, "\"command\"", 9)) {
      key = Command;
    } else {
      return false;
    }
    if ((seen & key) != 0) {
      return false;
    }
    seen |= key;
    skipJsonSpace(cursor);
    if (*cursor++ != ':') {
      return false;
    }
    skipJsonSpace(cursor);
    if (key == Command) {
      if (!consumeResponseToken(cursor, "null", 4)) {
        return false;
      }
    } else {
      const bool value = consumeResponseToken(cursor, "true", 4);
      if (!value &&
          (key != Accepted || !consumeResponseToken(cursor, "false", 5))) {
        return false;
      }
      if (key == Accepted) {
        accepted = value;
      }
    }
    skipJsonSpace(cursor);
    if (*cursor++ != (field == 2 ? '}' : ',')) {
      return false;
    }
  }
  skipJsonSpace(cursor);
  return *cursor == '\0';
}

// Cliente persistente: a 1 muestra/s, un handshake TLS por envío no cabe.
// La conexión se reutiliza; ante cualquier fallo de transporte se cierra para
// que el siguiente intento negocie de nuevo en lugar de reusar un socket roto.
static WiFiClientSecure &cloudTlsClient() {
  static WiFiClientSecure client;
  static bool configured = false;
  if (!configured) {
    client.setCACert(CRUCHI_ROOT_CA);
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SECONDS);
    configured = true;
  }
  return client;
}

static bool sendCloudSample(const TelemetrySample &sample) {
  WiFiClientSecure &client = cloudTlsClient();
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_IO_TIMEOUT_MS);
  http.setReuse(true);
  // Nunca reenviar el Bearer a otro destino mediante una redirección.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, CRUCHI_API_URL)) {
    publishCloudReport("begin_failed");
    return false;
  }
  char authorization[sizeof(CLOUD_DEVICE_TOKEN) + 7];
  snprintf(authorization, sizeof(authorization), "Bearer %s", CLOUD_DEVICE_TOKEN);
  http.addHeader("Authorization", authorization);
  http.addHeader("Content-Type", "application/json");
  char body[SAMPLE_JSON_CAPACITY];
  const uint64_t sentUptimeMs = uint64_t(esp_timer_get_time() / 1000);
  if (sentUptimeMs < sample.uptimeMs ||
      sentUptimeMs - sample.uptimeMs >= MAX_SAMPLE_AGE_MS) {
    http.end();
    publishCloudReport("sample_expired");
    return false;
  }
  const size_t length = formatSample(body, sizeof(body), sample, true, sentUptimeMs);
  if (length == 0) {
    http.end();
    publishCloudReport("payload_overflow");
    return false;
  }
  // begin(client, URL) conecta por hostname; la CA y mbedTLS validan su identidad.
  const int status = http.POST(reinterpret_cast<uint8_t *>(body), length);
  bool successful = false;
  if (status <= 0) {
    client.stop();
    publishCloudReport("transport_error", 0, status);
  } else if (status != HTTP_CODE_OK) {
    publishCloudReport("http_error", status);
  } else {
    CloudResponseBuffer response;
    bool accepted = false;
    const int declaredLength = http.getSize();
    const int received = declaredLength >= int(sizeof(response.data))
        ? 0 : http.writeToStream(&response);
    if (received < 0 || size_t(received) != response.length ||
        (declaredLength >= 0 && received != declaredLength) ||
        uint32_t(millis() - response.startedAt) >= HTTP_IO_TIMEOUT_MS ||
        !parseCloudResponse(response, accepted)) {
      client.stop();
      publishCloudReport("invalid_response", status, received < 0 ? received : 0);
    } else {
      successful = true;
      publishCloudReport(accepted ? "accepted" : "not_accepted",
                         status, 0, accepted ? 1 : 0);
    }
  }
  http.end();
  return successful;
}

static void cloudWorker(void *) {
  uint32_t retryMs = INITIAL_RETRY_MS;
  uint64_t nextAttemptAt = 0;
  const char *waitingStatus = nullptr;
  uint32_t wifiSwitch = wifiSetupSwitchCount();
  for (;;) {
    const uint32_t currentSwitch = wifiSetupSwitchCount();
    if (currentSwitch != wifiSwitch) {
      wifiSwitch = currentSwitch;
      cloudTlsClient().stop();
      retryMs = INITIAL_RETRY_MS;
      nextAttemptAt = 0;
    }
    const char *blocked = !wifiSetupConnected() ? "waiting_wifi"
        : time(nullptr) < MIN_VALID_UNIX_TIME ? "waiting_time" : nullptr;
    if (blocked != nullptr) {
      if (blocked != waitingStatus) {
        publishCloudReport(blocked);
        waitingStatus = blocked;
      }
      vTaskDelay(pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS));
      continue;
    }
    waitingStatus = nullptr;
    if (uint64_t(esp_timer_get_time() / 1000) < nextAttemptAt) {
      vTaskDelay(pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS));
      continue;
    }
    TelemetrySample sample;
    if (xQueueReceive(cloudSamples, &sample,
                      pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS)) != pdTRUE) {
      continue;
    }
    if (!wifiSetupConnected() || wifiSetupSwitchCount() != wifiSwitch) {
      continue;
    }
    if (sendCloudSample(sample)) {
      wifiSetupReportCloudReachable();
      retryMs = INITIAL_RETRY_MS;
      nextAttemptAt = 0;
    } else {
      nextAttemptAt = uint64_t(esp_timer_get_time() / 1000) + retryMs;
      retryMs = retryMs < MAX_RETRY_MS / 2 ? retryMs * 2 : MAX_RETRY_MS;
    }
  }
}

static void initializeCloud() {
  if (strncmp(CRUCHI_API_URL, "https://", 8) != 0) {
    Serial.println("{\"type\":\"cloud\",\"status\":\"https_required\"}");
    return;
  }
  cloudSamples = xQueueCreate(1, sizeof(TelemetrySample));
  cloudReports = xQueueCreate(4, sizeof(CloudReport));
  if (cloudSamples == nullptr || cloudReports == nullptr ||
      xTaskCreate(cloudWorker, "cloud_https", 12288, nullptr, 1, nullptr) != pdPASS) {
    if (cloudSamples != nullptr) {
      vQueueDelete(cloudSamples);
      cloudSamples = nullptr;
    }
    if (cloudReports != nullptr) {
      vQueueDelete(cloudReports);
      cloudReports = nullptr;
    }
    Serial.println("{\"type\":\"cloud\",\"status\":\"worker_start_failed\"}");
    return;
  }
}

static void readAndReport() {
  // La biblioteca reutiliza la misma adquisición para ambas magnitudes.
  TelemetrySample sample = {};
  sample.humidity = dht.readHumidity();
  sample.temperature = dht.readTemperature();
  sample.illuminanceLux = illuminanceLux;
  const bool readOk = isfinite(sample.temperature) && isfinite(sample.humidity);
  sample.valid = readOk && sample.temperature >= 0.0f &&
                 sample.temperature <= 50.0f && sample.humidity >= 0.0f &&
                 sample.humidity <= 100.0f;
  sample.sensorStatus = !readOk ? "read_error"
      : sample.valid ? "ok" : "out_of_range";
  sample.peltierLevel = gpio_get_level(PELTIER_PIN);
  sample.pumpLevel = gpio_get_level(PUMP_PIN);
  sample.ledsLevel = gpio_get_level(LEDS_PIN);
  sample.outputsSafe =
      sample.peltierLevel == 0 && sample.pumpLevel == 0 && sample.ledsLevel == 0;
  sample.sequence = ++sequence;
  sample.uptimeMs = uint64_t(esp_timer_get_time() / 1000);
  sample.wifiConnected = wifiSetupConnected();
  char body[SAMPLE_JSON_CAPACITY];
  if (formatSample(body, sizeof(body), sample, false) != 0) {
    Serial.println(body);
  } else {
    Serial.println("{\"type\":\"cloud\",\"status\":\"payload_overflow\"}");
  }
  if (cloudSamples != nullptr) {
    // Una única ranura: reemplazar la pendiente, nunca acumular historia.
    xQueueOverwrite(cloudSamples, &sample);
  }
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
  if (!wifiSetupStart(deviceId, bootId)) {
    Serial.println("{\"type\":\"wifi\",\"status\":\"worker_start_failed\"}");
  }
  lastSampleAt = millis();
  nextLightActionAt = lastSampleAt;
  initializeCloud();
}

void loop() {
  const uint32_t now = millis();
  serviceIlluminance(now);
  reportCloud();
  if (uint32_t(now - lastSampleAt) >= SAMPLE_INTERVAL_MS) {
    lastSampleAt = now;
    readAndReport();
  }
  delay(10);
}
