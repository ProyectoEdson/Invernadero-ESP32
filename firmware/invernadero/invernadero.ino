#include <Arduino.h>
#define ARDUINOJSON_POOL_CAPACITY 32
#include <ArduinoJson.h>
#include "response_json_allocator.h"
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <driver/gptimer.h>
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

#if __has_include("wifi.local.h")
#include "wifi.local.h"
#else
#error "Copia wifi.example.h como wifi.local.h y configura tu red de 2,4 GHz."
#endif

#if __has_include("cloud.local.h")
#include "cloud.local.h"
#else
#error "Copia cloud.example.h como cloud.local.h y configura URL, token y CA."
#endif

static_assert(sizeof(WIFI_SSID) > 1, "Configura WIFI_SSID en wifi.local.h.");
static_assert(sizeof(WIFI_SSID) <= 33, "El SSID admite como máximo 32 bytes.");
static_assert(sizeof(CLOUD_API_URL) > 9, "Configura CLOUD_API_URL en cloud.local.h.");
static_assert(sizeof(CLOUD_DEVICE_TOKEN) > 1, "Configura CLOUD_DEVICE_TOKEN en cloud.local.h.");
static_assert(sizeof(CLOUD_ROOT_CA) > 1, "Configura CLOUD_ROOT_CA en cloud.local.h.");

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
constexpr uint32_t MAX_SAMPLE_AGE_MS = 15000;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t HTTP_IO_TIMEOUT_MS = 5000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 5;
constexpr time_t MIN_VALID_UNIX_TIME = 1735689600;  // 2025-01-01 UTC.
constexpr size_t SAMPLE_JSON_CAPACITY = 1024;
constexpr size_t RESPONSE_JSON_CAPACITY = 1024;
constexpr uint32_t MAX_COMMAND_TTL_MS = 15000;
constexpr uint16_t MAX_PUMP_PULSE_MS = 3000;
constexpr uint64_t MAX_COMMAND_SEQUENCE = 9007199254740991ULL;

enum class OperatingMode : uint8_t { Manual, Automatic };
enum class CommandKind : uint8_t { SetMode, PumpPulse, SetFans };

struct CommandAck {
  char id[37];
  bool applied;
  const char *reason;
};

struct CloudCommand {
  char id[37];
  uint64_t sequence;
  uint64_t expiresAtMs;
  CommandKind kind;
  OperatingMode mode;
  uint16_t durationMs;
  bool fansOn;
  bool targetMatches;
};

struct ParsedCloudResponse {
  bool accepted;
  bool hasCommand;
  CloudCommand command;
};

struct TelemetrySample {
  uint64_t uptimeMs;
  uint32_t sequence;
  float temperature;
  float humidity;
  const char *sensorStatus;
  int peltierLevel;
  int pumpLevel;
  int fansLevel;
  bool valid;
  bool wifiConnected;
  bool outputsSafe;
  OperatingMode operatingMode;
  bool controlReady;
  bool hasAck;
  CommandAck ack;
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
static bool parseCloudResponse(CloudResponseBuffer &response,
                               uint64_t requestStartedAtMs,
                               ParsedCloudResponse &parsed);
static bool jsonStringEquals(JsonVariantConst value, const char *expected);
static bool parseCommandId(JsonVariantConst value, char *id);
static bool startPump(uint16_t durationMs);
static void applyCloudCommand(const CloudCommand &command, CommandAck &ack);
static bool sendCloudSample(const TelemetrySample &sample);
static void publishCloudReport(const char *status, int httpStatus = 0,
                               int transportCode = 0, int accepted = -1);

QueueHandle_t cloudSamples = nullptr;
QueueHandle_t cloudReports = nullptr;
QueueHandle_t cloudCommands = nullptr;
QueueHandle_t commandAcks = nullptr;

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
OperatingMode operatingMode = OperatingMode::Manual;
bool phase4ControlReady = false;
gptimer_handle_t pumpTimer = nullptr;
portMUX_TYPE pumpTimerMux = portMUX_INITIALIZER_UNLOCKED;
bool pumpActive = false;
uint64_t pumpDeadlineMs = 0;
uint64_t pumpAlarmCount = 0;  // Solo bajo pumpTimerMux, también en ISR.
uint64_t pumpStartedAtUs = 0;
uint64_t pumpStoppedAtUs = 0;
char pumpCommandId[37] = {};
uint64_t pumpCommandSequence = 0;
uint64_t commandWatermark = 0;
uint64_t lastCommandSequence = 0;
uint64_t lastCommandDeadlineMs = 0;
CommandAck lastCommandAck = {};

static uint64_t monotonicMillis() {
  return uint64_t(esp_timer_get_time() / 1000);
}

static bool IRAM_ATTR stopPumpAtAlarm(
    gptimer_handle_t timer, const gptimer_alarm_event_data_t *, void *) {
  // El contador nunca se reinicia. Una interrupción vieja no corta un pulso
  // nuevo: se compara/rearma siempre el deadline actualmente autorizado.
  portENTER_CRITICAL_ISR(&pumpTimerMux);
  if (pumpAlarmCount != 0) {
    uint64_t count = 0;
    if (gptimer_get_raw_count(timer, &count) != ESP_OK ||
        count >= pumpAlarmCount) {
      gpio_set_level(PUMP_PIN, 0);
      pumpStoppedAtUs = uint64_t(esp_timer_get_time());
      pumpAlarmCount = 0;
    } else {
      gptimer_alarm_config_t alarm = {};
      alarm.alarm_count = pumpAlarmCount;
      if (gptimer_set_alarm_action(timer, &alarm) != ESP_OK) {
        gpio_set_level(PUMP_PIN, 0);
        pumpStoppedAtUs = uint64_t(esp_timer_get_time());
        pumpAlarmCount = 0;
      }
    }
  }
  portEXIT_CRITICAL_ISR(&pumpTimerMux);
  return false;
}

static void initializePumpTimer() {
  gptimer_config_t config = {};
  config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  config.direction = GPTIMER_COUNT_UP;
  config.resolution_hz = 1000000;
  if (gptimer_new_timer(&config, &pumpTimer) != ESP_OK) {
    pumpTimer = nullptr;
    return;
  }
  gptimer_event_callbacks_t callbacks = {};
  callbacks.on_alarm = stopPumpAtAlarm;
  uint32_t resolution = 0;
  if (gptimer_register_event_callbacks(pumpTimer, &callbacks, nullptr) != ESP_OK ||
      gptimer_get_resolution(pumpTimer, &resolution) != ESP_OK ||
      resolution != 1000000 || gptimer_enable(pumpTimer) != ESP_OK) {
    gptimer_del_timer(pumpTimer);
    pumpTimer = nullptr;
    return;
  }
  if (gptimer_start(pumpTimer) != ESP_OK) {
    gptimer_disable(pumpTimer);
    gptimer_del_timer(pumpTimer);
    pumpTimer = nullptr;
  }
}

static bool stopPump() {
  portENTER_CRITICAL(&pumpTimerMux);
  pumpAlarmCount = 0;
  const esp_err_t result = gpio_set_level(PUMP_PIN, 0);
  if (pumpActive && pumpStoppedAtUs == 0) {
    pumpStoppedAtUs = uint64_t(esp_timer_get_time());
  }
  portEXIT_CRITICAL(&pumpTimerMux);
  pumpDeadlineMs = 0;
  const bool alarmDisabled = pumpTimer == nullptr ||
      gptimer_set_alarm_action(pumpTimer, nullptr) == ESP_OK;
  return alarmDisabled && result == ESP_OK && gpio_get_level(PUMP_PIN) == 0;
}

static bool startPump(uint16_t durationMs) {
  if (pumpTimer == nullptr || pumpActive || durationMs < 1 ||
      durationMs > MAX_PUMP_PULSE_MS) {
    return false;
  }
  gptimer_alarm_config_t alarm = {};
  portENTER_CRITICAL(&pumpTimerMux);
  uint64_t count = 0;
  esp_err_t result = gptimer_get_raw_count(pumpTimer, &count);
  if (result == ESP_OK) {
    alarm.alarm_count = count + uint64_t(durationMs) * 1000;
    pumpAlarmCount = alarm.alarm_count;
    pumpStartedAtUs = uint64_t(esp_timer_get_time());
    pumpStoppedAtUs = 0;
    result = gptimer_set_alarm_action(pumpTimer, &alarm);
    if (result == ESP_OK) {
      result = gpio_set_level(PUMP_PIN, 1);
      if (result == ESP_OK && gpio_get_level(PUMP_PIN) != 1) {
        result = ESP_FAIL;
      }
    }
  }
  if (result != ESP_OK) {
    pumpAlarmCount = 0;
    gpio_set_level(PUMP_PIN, 0);
  }
  portEXIT_CRITICAL(&pumpTimerMux);
  if (result != ESP_OK) {
    stopPump();
    return false;
  }
  pumpActive = true;
  pumpDeadlineMs = (pumpStartedAtUs + uint64_t(durationMs) * 1000 + 999) / 1000;
  return true;
}

static void servicePump() {
  if (!pumpActive) {
    return;
  }
  if (pumpDeadlineMs != 0 && monotonicMillis() >= pumpDeadlineMs) {
    stopPump();
  }
  portENTER_CRITICAL(&pumpTimerMux);
  const uint64_t stoppedAtUs = pumpStoppedAtUs;
  const uint64_t startedAtUs = pumpStartedAtUs;
  portEXIT_CRITICAL(&pumpTimerMux);
  if (stoppedAtUs != 0) {
    pumpActive = false;
    pumpDeadlineMs = 0;
    Serial.printf(
        "{\"type\":\"pump_finished\",\"id\":\"%s\",\"sequence\":%llu,"
        "\"started_uptime_us\":%llu,\"stopped_uptime_us\":%llu,"
        "\"elapsed_us\":%llu,\"pump_level\":%d,\"fans_level\":%d}\n",
        pumpCommandId, (unsigned long long)pumpCommandSequence,
        (unsigned long long)startedAtUs, (unsigned long long)stoppedAtUs,
        (unsigned long long)(stoppedAtUs - startedAtUs),
        gpio_get_level(PUMP_PIN), gpio_get_level(FANS_PIN));
  }
}

static void applyCloudCommand(const CloudCommand &command, CommandAck &ack) {
  memcpy(ack.id, command.id, sizeof(ack.id));
  ack.applied = false;
  ack.reason = nullptr;
  if (!command.targetMatches) {
    ack.reason = "target_boot_mismatch";
  } else if (monotonicMillis() >= command.expiresAtMs) {
    ack.reason = "expired";
  } else if (!phase4ControlReady) {
    ack.reason = "control_unavailable";
  } else if (command.kind == CommandKind::SetMode) {
    if ((command.mode != operatingMode ||
         command.mode == OperatingMode::Automatic) && !stopPump()) {
      ack.reason = "gpio_error";
    } else {
      operatingMode = command.mode;
      ack.applied = true;
    }
  } else if (operatingMode != OperatingMode::Manual) {
    ack.reason = "manual_required";
  } else if (command.kind == CommandKind::PumpPulse) {
    if (pumpActive) {
      ack.reason = "pump_busy";
    } else {
      ack.applied = startPump(command.durationMs);
      ack.reason = ack.applied ? nullptr : "pump_timer_error";
      if (ack.applied) {
        memcpy(pumpCommandId, command.id, sizeof(pumpCommandId));
        pumpCommandSequence = command.sequence;
      }
    }
  } else {
    const int level = command.fansOn ? 1 : 0;
    ack.applied = gpio_set_level(FANS_PIN, level) == ESP_OK &&
                  gpio_get_level(FANS_PIN) == level;
    ack.reason = ack.applied ? nullptr : "gpio_error";
  }
}

static void serviceCommands() {
  if (cloudCommands == nullptr || commandAcks == nullptr ||
      uxQueueMessagesWaiting(commandAcks) != 0) {
    return;
  }
  CloudCommand command;
  if (xQueueReceive(cloudCommands, &command, 0) != pdTRUE) {
    return;
  }
  CommandAck ack = {};
  if (command.sequence <= commandWatermark) {
    if (command.sequence == lastCommandSequence &&
        strcmp(command.id, lastCommandAck.id) == 0) {
      // Conservar resultado y deadline originales, sin volver a actuar.
      ack = lastCommandAck;
    } else {
      memcpy(ack.id, command.id, sizeof(ack.id));
      ack.reason = "stale_sequence";
    }
  } else {
    if (strcmp(command.id, lastCommandAck.id) == 0) {
      memcpy(ack.id, command.id, sizeof(ack.id));
      ack.reason = "command_identity_mismatch";
    } else {
      applyCloudCommand(command, ack);
    }
    commandWatermark = command.sequence;
    lastCommandSequence = command.sequence;
    lastCommandDeadlineMs = command.expiresAtMs;
    lastCommandAck = ack;
  }
  // Solo loop produce y solo worker retira, tras confirmar entrega aceptada.
  xQueueSend(commandAcks, &ack, 0);
  Serial.printf(
      "{\"type\":\"command\",\"id\":\"%s\",\"sequence\":%llu,"
      "\"status\":\"%s\",\"reason\":%s%s%s,\"expires_at_uptime_ms\":%llu,"
      "\"pump_level\":%d,\"fans_level\":%d}\n",
      ack.id, (unsigned long long)command.sequence,
      ack.applied ? "applied" : "rejected", ack.reason ? "\"" : "",
      ack.reason ? ack.reason : "null", ack.reason ? "\"" : "",
      (unsigned long long)(command.sequence == lastCommandSequence
          ? lastCommandDeadlineMs : command.expiresAtMs),
      gpio_get_level(PUMP_PIN), gpio_get_level(FANS_PIN));
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
      "{\"type\":\"boot\",\"firmware\":\"phase4\",\"device_id\":\"%s\","
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

static size_t formatSample(char *buffer, size_t capacity,
                           const TelemetrySample &sample, bool sending,
                           uint64_t sentUptimeMs) {
  char temperatureText[16] = "null";
  char humidityText[16] = "null";
  if (sample.valid) {
    snprintf(temperatureText, sizeof(temperatureText), "%.1f", sample.temperature);
    snprintf(humidityText, sizeof(humidityText), "%.1f", sample.humidity);
  }
  char ackText[192] = "null";
  if (sample.hasAck) {
    const int ackLength = snprintf(
        ackText, sizeof(ackText),
        "{\"id\":\"%s\",\"status\":\"%s\",\"reason\":%s%s%s}",
        sample.ack.id, sample.ack.applied ? "applied" : "rejected",
        sample.ack.reason ? "\"" : "",
        sample.ack.reason ? sample.ack.reason : "null",
        sample.ack.reason ? "\"" : "");
    if (ackLength < 0 || size_t(ackLength) >= sizeof(ackText)) {
      return 0;
    }
  }
  const int length = snprintf(
      buffer, capacity,
      "{\"type\":\"sample\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
      "\"sequence\":%lu,\"uptime_ms\":%llu,\"sensor_status\":\"%s\","
      "\"temperature_c\":%s,\"humidity_pct\":%s,\"wifi_connected\":%s,"
      "\"memory_config_ok\":%s,\"peltier_locked\":true,"
      "\"gpio_levels\":{\"peltier\":%d,\"pump\":%d,\"fans\":%d},"
      "\"outputs_safe\":%s,\"operating_mode\":\"%s\","
      "\"phase4_control_ready\":%s,\"ack\":%s",
      deviceId, bootId, (unsigned long)sample.sequence,
      (unsigned long long)sample.uptimeMs, sample.sensorStatus,
      temperatureText, humidityText, sample.wifiConnected ? "true" : "false",
      memoryConfigurationOk ? "true" : "false", sample.peltierLevel,
      sample.pumpLevel, sample.fansLevel, sample.outputsSafe ? "true" : "false",
      sample.operatingMode == OperatingMode::Manual ? "manual" : "automatic",
      sample.controlReady ? "true" : "false", ackText);
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

// ArduinoJson normaliza claves repetidas y permite terminar tras el primer
// objeto. Este pase cerrado rechaza duplicados, claves desconocidas, objetos
// adicionales, arrays y profundidad arbitraria antes de deserializar.
static bool skipResponseScalar(const char *&cursor) {
  skipJsonSpace(cursor);
  if (*cursor == '"') {
    ++cursor;
    while (*cursor != '\0' && *cursor != '"') {
      if (uint8_t(*cursor) < 0x20) {
        return false;
      }
      if (*cursor++ == '\\') {
        if (*cursor == '\0' || uint8_t(*cursor) < 0x20) {
          return false;
        }
        ++cursor;
      }
    }
    if (*cursor != '"') {
      return false;
    }
    ++cursor;
    return true;
  }
  if (strncmp(cursor, "true", 4) == 0) {
    cursor += 4;
    return true;
  }
  if (strncmp(cursor, "false", 5) == 0) {
    cursor += 5;
    return true;
  }
  if (strncmp(cursor, "null", 4) == 0) {
    cursor += 4;
    return true;
  }
  if (*cursor == '-') {
    ++cursor;
  }
  if (*cursor == '0') {
    ++cursor;
  } else if (*cursor >= '1' && *cursor <= '9') {
    do {
      ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');
  } else {
    return false;
  }
  if (*cursor == '.') {
    ++cursor;
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    do {
      ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');
  }
  if (*cursor == 'e' || *cursor == 'E') {
    ++cursor;
    if (*cursor == '+' || *cursor == '-') {
      ++cursor;
    }
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    do {
      ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');
  }
  return true;
}

static bool scanResponseObject(const char *&cursor, bool commandObject) {
  static const char *const envelopeKeys[] = {"ok", "accepted", "command"};
  static const char *const commandKeys[] = {
      "id", "sequence", "kind", "value", "duration_ms", "expires_in_ms",
      "target_boot_id"};
  const char *const *keys = commandObject ? commandKeys : envelopeKeys;
  const unsigned int keyCount = commandObject ? 7 : 3;
  skipJsonSpace(cursor);
  if (*cursor++ != '{') {
    return false;
  }
  unsigned int seen = 0;
  for (unsigned int field = 0; field < keyCount; ++field) {
    skipJsonSpace(cursor);
    if (*cursor++ != '"') {
      return false;
    }
    const char *key = cursor;
    while ((*cursor >= 'a' && *cursor <= 'z') || *cursor == '_') {
      ++cursor;
    }
    if (*cursor != '"') {
      return false;
    }
    const size_t keyLength = size_t(cursor - key);
    unsigned int index = 0;
    for (; index < keyCount; ++index) {
      if (strlen(keys[index]) == keyLength &&
          memcmp(keys[index], key, keyLength) == 0) {
        break;
      }
    }
    if (index == keyCount || (seen & (1U << index)) != 0) {
      return false;
    }
    seen |= 1U << index;
    ++cursor;
    skipJsonSpace(cursor);
    if (*cursor++ != ':') {
      return false;
    }
    skipJsonSpace(cursor);
    if (!commandObject && index == 2 && *cursor == '{') {
      if (!scanResponseObject(cursor, true)) {
        return false;
      }
    } else if (!skipResponseScalar(cursor)) {
      return false;
    }
    skipJsonSpace(cursor);
    if (*cursor++ != (field + 1 == keyCount ? '}' : ',')) {
      return false;
    }
  }
  return seen == (1U << keyCount) - 1;
}


static bool jsonStringEquals(JsonVariantConst value, const char *expected) {
  if (!value.is<const char *>()) {
    return false;
  }
  const JsonString text = value.as<JsonString>();
  return text.size() == strlen(expected) &&
         memcmp(text.c_str(), expected, text.size()) == 0;
}

static bool parseCommandId(JsonVariantConst value, char *id) {
  if (!value.is<const char *>()) {
    return false;
  }
  const JsonString text = value.as<JsonString>();
  if (text.size() != 36) {
    return false;
  }
  for (size_t i = 0; i < 36; ++i) {
    const char c = text.c_str()[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
      id[i] = '-';
    } else if (c >= '0' && c <= '9') {
      id[i] = c;
    } else if (c >= 'a' && c <= 'f') {
      id[i] = c;
    } else if (c >= 'A' && c <= 'F') {
      id[i] = c + ('a' - 'A');
    } else {
      return false;
    }
  }
  id[36] = '\0';
  return true;
}

static bool parseCloudResponse(CloudResponseBuffer &response,
                               uint64_t requestStartedAtMs,
                               ParsedCloudResponse &parsed) {
  parsed = {};
  if (response.length == 0 ||
      memchr(response.data, '\0', response.length) != nullptr) {
    return false;
  }
  const char *cursor = response.data;
  if (!scanResponseObject(cursor, false)) {
    return false;
  }
  skipJsonSpace(cursor);
  if (*cursor != '\0') {
    return false;
  }
  ResponseJsonAllocator allocator;
  JsonDocument document(&allocator);
  if (deserializeJson(document, response.data, response.length,
                      DeserializationOption::NestingLimit(2)) ||
      document.overflowed() || !document.is<JsonObject>() ||
      !document["ok"].is<bool>() || !document["ok"].as<bool>() ||
      !document["accepted"].is<bool>()) {
    return false;
  }
  parsed.accepted = document["accepted"].as<bool>();
  const JsonVariantConst commandValue = document["command"];
  if (commandValue.isNull()) {
    return true;
  }
  if (!parsed.accepted || !commandValue.is<JsonObjectConst>()) {
    return false;
  }
  const JsonObjectConst command = commandValue.as<JsonObjectConst>();
  if (!parseCommandId(command["id"], parsed.command.id) ||
      !command["sequence"].is<uint64_t>() ||
      command["sequence"].as<uint64_t>() < 1 ||
      command["sequence"].as<uint64_t>() > MAX_COMMAND_SEQUENCE ||
      !command["expires_in_ms"].is<uint32_t>() ||
      command["expires_in_ms"].as<uint32_t>() > MAX_COMMAND_TTL_MS ||
      !command["target_boot_id"].is<const char *>()) {
    return false;
  }
  const JsonString target = command["target_boot_id"].as<JsonString>();
  if (target.size() != 32) {
    return false;
  }
  for (size_t i = 0; i < target.size(); ++i) {
    const char c = target.c_str()[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  parsed.command.targetMatches = memcmp(target.c_str(), bootId, 32) == 0;
  parsed.command.sequence = command["sequence"].as<uint64_t>();
  // Restar TODO el RTT, incluido TLS/POST/cuerpo, es conservador. Cada primera
  // entrega fija este deadline; un replay nunca lo sustituye en loop.
  parsed.command.expiresAtMs =
      requestStartedAtMs + command["expires_in_ms"].as<uint32_t>();
  if (jsonStringEquals(command["kind"], "set_mode")) {
    parsed.command.kind = CommandKind::SetMode;
    if (!command["duration_ms"].isNull()) {
      return false;
    }
    if (jsonStringEquals(command["value"], "manual")) {
      parsed.command.mode = OperatingMode::Manual;
    } else if (jsonStringEquals(command["value"], "automatic")) {
      parsed.command.mode = OperatingMode::Automatic;
    } else {
      return false;
    }
  } else if (jsonStringEquals(command["kind"], "pump_pulse")) {
    parsed.command.kind = CommandKind::PumpPulse;
    if (!command["value"].isNull() ||
        !command["duration_ms"].is<uint16_t>() ||
        command["duration_ms"].as<uint16_t>() < 1 ||
        command["duration_ms"].as<uint16_t>() > MAX_PUMP_PULSE_MS) {
      return false;
    }
    parsed.command.durationMs = command["duration_ms"].as<uint16_t>();
  } else if (jsonStringEquals(command["kind"], "set_fans")) {
    parsed.command.kind = CommandKind::SetFans;
    if (!command["value"].is<bool>() ||
        !command["duration_ms"].isNull()) {
      return false;
    }
    parsed.command.fansOn = command["value"].as<bool>();
  } else {
    return false;
  }
  parsed.hasCommand = true;
  return true;
}

static bool sendCloudSample(const TelemetrySample &sample) {
  WiFiClientSecure client;
  client.setCACert(CLOUD_ROOT_CA);
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SECONDS);
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_IO_TIMEOUT_MS);
  http.setReuse(false);
  // Nunca reenviar el Bearer a otro destino mediante una redirección.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, CLOUD_API_URL)) {
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
  const uint64_t requestStartedAtMs = monotonicMillis();
  const int status = http.POST(reinterpret_cast<uint8_t *>(body), length);
  bool successful = false;
  if (status <= 0) {
    publishCloudReport("transport_error", 0, status);
  } else if (status != HTTP_CODE_OK) {
    publishCloudReport("http_error", status);
  } else {
    CloudResponseBuffer response;
    ParsedCloudResponse parsed = {};
    const int declaredLength = http.getSize();
    const int received = declaredLength >= int(sizeof(response.data))
        ? 0 : http.writeToStream(&response);
    if (received < 0 || size_t(received) != response.length ||
        (declaredLength >= 0 && received != declaredLength) ||
        !parseCloudResponse(response, requestStartedAtMs, parsed)) {
      publishCloudReport("invalid_response", status, received < 0 ? received : 0);
    } else {
      successful = true;
      if (parsed.accepted && sample.hasAck) {
        CommandAck pendingAck;
        if (xQueuePeek(commandAcks, &pendingAck, 0) == pdTRUE &&
            strcmp(pendingAck.id, sample.ack.id) == 0 &&
            pendingAck.applied == sample.ack.applied &&
            pendingAck.reason == sample.ack.reason) {
          // La muestra y su ack fueron aceptados juntos. Un fallo HTTP,
          // respuesta malformada o accepted:false nunca consume este ack.
          xQueueReceive(commandAcks, &pendingAck, 0);
        }
      }
      if (parsed.hasCommand &&
          xQueueSend(cloudCommands, &parsed.command, 0) != pdTRUE) {
        // No sobrescribir una orden/deadline todavía pendiente en loop.
        publishCloudReport("command_queue_full", status);
      }
      publishCloudReport(parsed.accepted ? "accepted" : "not_accepted",
                         status, 0, parsed.accepted ? 1 : 0);
    }
  }
  http.end();
  return successful;
}

static void cloudWorker(void *) {
  // SNTP trabaja de forma asíncrona; jamás esperar la hora en setup/loop.
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  uint32_t retryMs = INITIAL_RETRY_MS;
  uint64_t nextAttemptAt = 0;
  const char *waitingStatus = nullptr;
  for (;;) {
    const char *blocked = WiFi.status() != WL_CONNECTED ? "waiting_wifi"
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
    if (sendCloudSample(sample)) {
      retryMs = INITIAL_RETRY_MS;
      nextAttemptAt = 0;
    } else {
      nextAttemptAt = uint64_t(esp_timer_get_time() / 1000) + retryMs;
      retryMs = retryMs < MAX_RETRY_MS / 2 ? retryMs * 2 : MAX_RETRY_MS;
    }
  }
}

static void initializeCloud() {
  if (strncmp(CLOUD_API_URL, "https://", 8) != 0) {
    Serial.println("{\"type\":\"cloud\",\"status\":\"https_required\"}");
    return;
  }
  cloudSamples = xQueueCreate(1, sizeof(TelemetrySample));
  cloudReports = xQueueCreate(4, sizeof(CloudReport));
  cloudCommands = xQueueCreate(1, sizeof(CloudCommand));
  commandAcks = xQueueCreate(1, sizeof(CommandAck));
  if (cloudSamples == nullptr || cloudReports == nullptr ||
      cloudCommands == nullptr || commandAcks == nullptr ||
      xTaskCreate(cloudWorker, "cloud_https", 12288, nullptr, 1, nullptr) != pdPASS) {
    if (cloudSamples != nullptr) {
      vQueueDelete(cloudSamples);
      cloudSamples = nullptr;
    }
    if (cloudReports != nullptr) {
      vQueueDelete(cloudReports);
      cloudReports = nullptr;
    }
    if (cloudCommands != nullptr) {
      vQueueDelete(cloudCommands);
      cloudCommands = nullptr;
    }
    if (commandAcks != nullptr) {
      vQueueDelete(commandAcks);
      commandAcks = nullptr;
    }
    Serial.println("{\"type\":\"cloud\",\"status\":\"worker_start_failed\"}");
    return;
  }
  phase4ControlReady = pumpTimer != nullptr;
  if (!phase4ControlReady) {
    Serial.println("{\"type\":\"cloud\",\"status\":\"pump_timer_unavailable\"}");
  }
}

static void readAndReport() {
  // La biblioteca reutiliza la misma adquisición para ambas magnitudes.
  TelemetrySample sample = {};
  sample.humidity = dht.readHumidity();
  sample.temperature = dht.readTemperature();
  const bool readOk = isfinite(sample.temperature) && isfinite(sample.humidity);
  sample.valid = readOk && sample.temperature >= 0.0f &&
                 sample.temperature <= 50.0f && sample.humidity >= 0.0f &&
                 sample.humidity <= 100.0f;
  sample.sensorStatus = !readOk ? "read_error"
      : sample.valid ? "ok" : "out_of_range";
  sample.peltierLevel = gpio_get_level(PELTIER_PIN);
  sample.pumpLevel = gpio_get_level(PUMP_PIN);
  sample.fansLevel = gpio_get_level(FANS_PIN);
  sample.outputsSafe = sample.peltierLevel == 0;
  sample.operatingMode = operatingMode;
  sample.controlReady = phase4ControlReady;
  sample.hasAck = commandAcks != nullptr &&
      xQueuePeek(commandAcks, &sample.ack, 0) == pdTRUE;
  sample.sequence = ++sequence;
  sample.uptimeMs = uint64_t(esp_timer_get_time() / 1000);
  sample.wifiConnected = wasConnected;
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
  initializePumpTimer();
  // Inicia la asociación sin esperar en un while; el muestreo sigue su reloj.
  lastWifiAttemptAt = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastSampleAt = millis();
  initializeCloud();
}

void loop() {
  servicePump();
  serviceCommands();
  servicePump();
  const uint32_t now = millis();
  serviceNetwork(now);
  reportCloud();
  if (uint32_t(now - lastSampleAt) >= SAMPLE_INTERVAL_MS) {
    lastSampleAt = now;
    readAndReport();
    servicePump();
  }
  delay(10);
}
