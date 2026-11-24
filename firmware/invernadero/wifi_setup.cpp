#include "wifi_setup.h"
#include "wifi_setup_internal.h"

#include <Arduino.h>
#define ARDUINOJSON_POOL_CAPACITY 32
#include <ArduinoJson.h>
#include "response_json_allocator.h"
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <time.h>

#include "cloud.local.h"
#include "cruchi_tls.h"
#if __has_include("wifi.local.h")
#include "wifi.local.h"
#else
#error "Copia wifi.example.h como wifi.local.h y configura tu red de 2,4 GHz."
#endif

#ifndef WIFI_SETUP_PASSWORD
#define WIFI_SETUP_PASSWORD ""
#endif
#ifndef WIFI_REMOTE_KEY
#define WIFI_REMOTE_KEY ""
#endif

namespace WifiSetup {
namespace {

constexpr size_t COMPILED_NETWORKS = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
static_assert(wifiNetworksUsable(WIFI_NETWORKS), "Configura al menos una red de SSID 1-32 bytes y clave de hasta 63.");
constexpr bool AP_ENABLED = sizeof(WIFI_SETUP_PASSWORD) > 1;
constexpr bool REMOTE_ENABLED = sizeof(WIFI_REMOTE_KEY) > 1;
constexpr bool validSetupPassword(const char *text, size_t length) {
  if (length == 0) return true;
  if (length < 12 || length > MAX_PASSWORD_BYTES) return false;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < 0x20 || text[i] > 0x7E) return false;
  }
  return true;
}
constexpr bool validRemoteKey(const char *key, size_t length) {
  if (length == 0) return true;
  if (length != 64) return false;
  for (size_t i = 0; i < length; ++i) {
    if (hexDigit(key[i]) < 0) return false;
  }
  return true;
}
static_assert(validSetupPassword(WIFI_SETUP_PASSWORD, sizeof(WIFI_SETUP_PASSWORD) - 1),
              "WIFI_SETUP_PASSWORD: vacío o 12-63 caracteres ASCII imprimibles.");
static_assert(validRemoteKey(WIFI_REMOTE_KEY, sizeof(WIFI_REMOTE_KEY) - 1),
              "WIFI_REMOTE_KEY: vacío o 64 caracteres hexadecimales en minúscula.");

constexpr uint32_t ATTEMPT_MS = 10000;
constexpr uint32_t INITIAL_RETRY_MS = 15000;
constexpr uint32_t MAX_RETRY_MS = 60000;
constexpr uint32_t CLOUD_OUTAGE_MS = 120000;
constexpr uint32_t LOCAL_SAVED_GRACE_MS = 15000;
constexpr uint32_t SCAN_MS = 8000;
constexpr uint32_t TRIAL_MS = 40000;
constexpr uint32_t POLL_MS = 5000;
constexpr uint32_t PROBE_RETRY_MS = 2000;
constexpr uint32_t MAX_JOB_MS = 90000;
constexpr time_t MIN_TLS_TIME = 1735689600;
constexpr size_t ID_BYTES = 16;
constexpr size_t NONCE_BYTES = 12;
constexpr size_t GCM_TAG_BYTES = 16;
constexpr size_t MAX_PLAINTEXT_BYTES = 1 + MAX_SSID_BYTES + MAX_PASSWORD_BYTES;
constexpr size_t MAX_ENVELOPE_BYTES = NONCE_BYTES + GCM_TAG_BYTES + MAX_PLAINTEXT_BYTES;
constexpr char NVS_NAMESPACE[] = "inv-wifi";
constexpr char NVS_KEY[] = "profile-v1";

std::atomic<bool> usableStation{false};
std::atomic<uint32_t> switchCount{0};
std::atomic<uint32_t> telemetrySuccessCount{0};
std::atomic<int> scanEvent{-1};

bool reached(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }

void scanFinished(void *, esp_event_base_t, int32_t, void *event) {
  const auto *completed = static_cast<const wifi_event_sta_scan_done_t *>(event);
  scanEvent.store(completed->status == 0 ? 1 : 0);
}

bool attached() {
  return WiFi.status() == WL_CONNECTED && WiFi.STA.connected() && WiFi.STA.hasIP();
}

bool attachedTo(const char *ssid) {
  wifi_ap_record_t ap;
  return attached() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
      strncmp(reinterpret_cast<const char *>(ap.ssid), ssid, MAX_SSID_BYTES + 1) == 0;
}

bool connectStation(const char *ssid, const char *password) {
  wifi_config_t config = {};
  memcpy(config.sta.ssid, ssid, strlen(ssid));
  const size_t passwordLength = password == nullptr ? 0 : strlen(password);
  if (passwordLength != 0) memcpy(config.sta.password, password, passwordLength);
  config.sta.scan_method = WIFI_FAST_SCAN;
  config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  config.sta.threshold.rssi = -127;
  config.sta.threshold.authmode = passwordLength == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  config.sta.pmf_cfg.capable = true;
  const bool configured = esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK;
  mbedtls_platform_zeroize(&config, sizeof(config));
  return configured && esp_wifi_connect() == ESP_OK;
}

void clearStationProfile() {
  wifi_config_t empty = {};
  esp_wifi_set_config(WIFI_IF_STA, &empty);
}

bool loadProfile(Profile &profile) {
  Preferences nvs;
  if (!nvs.begin(NVS_NAMESPACE, true)) return false;
  const bool valid = nvs.getBytesLength(NVS_KEY) == sizeof(profile) &&
      nvs.getBytes(NVS_KEY, &profile, sizeof(profile)) == sizeof(profile) && validProfile(profile);
  if (!valid) mbedtls_platform_zeroize(&profile, sizeof(profile));
  return valid;
}

bool saveProfile(const Profile &profile) {
  Preferences nvs;
  // El registro anterior no se borra: NVS reemplaza la única entrada al confirmar.
  return nvs.begin(NVS_NAMESPACE, false) && nvs.putBytes(NVS_KEY, &profile, sizeof(profile)) == sizeof(profile);
}

void collectScan(ScanResults &results) {
  results.count = 0;
  // El driver entrega primero las señales más fuertes. Limitar incluso los
  // registros descartados por duplicados o por no contener un SSID válido.
  for (size_t i = 0; i < MAX_NETWORKS * 4 && results.count < MAX_NETWORKS; ++i) {
    wifi_ap_record_t ap;
    if (esp_wifi_scan_get_ap_record(&ap) != ESP_OK) break;
    const char *ssid = reinterpret_cast<const char *>(ap.ssid);
    const size_t length = strnlen(ssid, sizeof(ap.ssid));
    if (length == 0 || length > MAX_SSID_BYTES || !validText(ssid, length)) continue;
    bool duplicate = false;
    for (size_t j = 0; j < results.count; ++j) {
      if (strcmp(results.networks[j].ssid, ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    Network &network = results.networks[results.count++];
    memcpy(network.ssid, ssid, length + 1);
    network.rssi = ap.rssi;
    network.secured = ap.authmode != WIFI_AUTH_OPEN;
  }
}

enum class Station : uint8_t { Idle, Disconnecting, Associating, Verifying, Online };
enum class Operation : uint8_t { None, LocalScan, RemoteScan, LocalConnect, RemoteConnect };
enum class Result : uint8_t { None, Scanned, Saved, Failed };
enum class Kind : uint8_t { None, Scan, Connect };

struct Command {
  char id[ID_BYTES * 2 + 1];
  char encrypted[MAX_ENVELOPE_BYTES * 2 + 1];
  uint32_t lifetime;
  Kind kind;
};

struct MailboxReply {
  Command command;
  char resultId[ID_BYTES * 2 + 1];
};

constexpr size_t MAX_RESPONSE_BYTES = 512;
struct LimitedResponse {
  char data[MAX_RESPONSE_BYTES + 1];
  size_t length = 0;
};

bool copyId(JsonVariantConst input, char *destination) {
  const JsonString text = input.as<JsonString>();
  if (!text || text.size() != ID_BYTES * 2) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (hexDigit(text.c_str()[i]) < 0) return false;
  }
  memcpy(destination, text.c_str(), ID_BYTES * 2);
  destination[ID_BYTES * 2] = '\0';
  return true;
}

bool parseReply(const LimitedResponse &response, bool poll, MailboxReply &reply) {
  if (response.length == 0 || memchr(response.data, '\0', response.length) != nullptr) return false;
  ResponseJsonAllocator allocator;
  JsonDocument doc(&allocator);
  if (deserializeJson(doc, response.data, response.length, DeserializationOption::NestingLimit(3))) return false;
  const JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.size() != 3 || !root["ok"].is<bool>() || !root["ok"].as<bool>()) return false;
  const auto resultId = root["result_id"];
  const auto command = root["command"];
  if (command.isUnbound() || resultId.isUnbound()) return false;
  if (!poll) return resultId.isNull() && command.isNull();
  if (!resultId.isNull() && !copyId(resultId, reply.resultId)) return false;
  if (command.isNull()) return true;
  if (!command.is<JsonObjectConst>() || !copyId(command["id"], reply.command.id) ||
      !command["expires_in_ms"].is<uint32_t>()) return false;
  const auto object = command.as<JsonObjectConst>();
  reply.command.lifetime = command["expires_in_ms"].as<uint32_t>();
  if (reply.command.lifetime == 0 || reply.command.lifetime > MAX_JOB_MS) return false;
  if (object.size() == 3 && command["kind"] == "scan") {
    reply.command.kind = Kind::Scan;
  } else if (object.size() == 4 && command["kind"] == "connect") {
    const JsonString encrypted = command["encrypted"].as<JsonString>();
    if (!encrypted || encrypted.size() < (NONCE_BYTES + GCM_TAG_BYTES + 2) * 2 ||
        encrypted.size() > MAX_ENVELOPE_BYTES * 2 || encrypted.size() % 2 != 0 ||
        memchr(encrypted.c_str(), '\0', encrypted.size()) != nullptr) return false;
    memcpy(reply.command.encrypted, encrypted.c_str(), encrypted.size());
    reply.command.encrypted[encrypted.size()] = '\0';
    reply.command.kind = Kind::Connect;
  } else {
    return false;
  }
  return true;
}

bool decryptProfile(const Command &command, Profile &profile) {
  if (!REMOTE_ENABLED) return false;
  uint8_t key[32];
  uint8_t envelope[MAX_ENVELOPE_BYTES];
  const size_t envelopeLength = strlen(command.encrypted) / 2;
  if (!readHex(command.encrypted, envelope, envelopeLength) || !readHex(WIFI_REMOTE_KEY, key, sizeof(key))) {
    mbedtls_platform_zeroize(key, sizeof(key));
    return false;
  }
  uint8_t plaintext[MAX_PLAINTEXT_BYTES];
  const size_t length = envelopeLength - NONCE_BYTES - GCM_TAG_BYTES;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool authenticated = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_auth_decrypt(&gcm, length, envelope, NONCE_BYTES,
          reinterpret_cast<const uint8_t *>(command.id), ID_BYTES * 2, envelope + NONCE_BYTES, GCM_TAG_BYTES,
          envelope + NONCE_BYTES + GCM_TAG_BYTES, plaintext) == 0;
  mbedtls_gcm_free(&gcm);
  mbedtls_platform_zeroize(key, sizeof(key));
  bool valid = false;
  if (authenticated) {
    const size_t ssidLength = plaintext[0];
    if (ssidLength > 0 && ssidLength <= MAX_SSID_BYTES && ssidLength < length) {
      const size_t passwordLength = length - 1 - ssidLength;
      const char *ssid = reinterpret_cast<const char *>(plaintext + 1);
      const char *password = ssid + ssidLength;
      valid = passwordLength <= MAX_PASSWORD_BYTES && (passwordLength == 0 || passwordLength >= 8) &&
          validText(ssid, ssidLength) && validText(password, passwordLength);
      if (valid) {
        memcpy(profile.ssid, ssid, ssidLength);
        memcpy(profile.password, password, passwordLength);
      }
    }
  }
  mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
  return valid;
}

// El máximo por SSID es seis bytes JSON por byte de entrada (control ASCII).
// El resto del objeto de red y del encabezado cabe en los márgenes fijos.
constexpr size_t MAX_REQUEST_BYTES = 256 + MAX_NETWORKS * (MAX_SSID_BYTES * 6 + 64);
constexpr int HTTP_TIMEOUT_MS = 3000;

enum class TransferState : uint8_t { Idle, InFlight, Complete };

struct Transfer {
  // El dueño de Wi-Fi puede restaurar la red aunque DNS/TLS siga esperando.
  std::atomic<TransferState> state{TransferState::Idle};
  TaskHandle_t worker = nullptr;
  uint32_t route;
  uint32_t startedAt;
  bool poll;
  bool valid;
  int length;
  char body[REMOTE_ENABLED ? MAX_REQUEST_BYTES : 128];
  MailboxReply reply;
};

Transfer transfer;

bool performRequest() {
  ifreq interface = {};
  if (esp_netif_get_netif_impl_name(WiFi.STA.netif(), interface.ifr_name) != ESP_OK) return false;
  esp_http_client_config_t config = {};
  config.url = CRUCHI_WIFI_API_URL;
  config.cert_pem = CRUCHI_ROOT_CA;  // El hostname del URL se verifica en TLS.
  config.method = HTTP_METHOD_POST;
  config.disable_auto_redirect = true;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.if_name = &interface;
  esp_http_client_handle_t http = esp_http_client_init(&config);
  if (http == nullptr) return false;
  char authorization[sizeof(CLOUD_DEVICE_TOKEN) + 7];
  snprintf(authorization, sizeof(authorization), "Bearer %s", CLOUD_DEVICE_TOKEN);
  const bool headers = esp_http_client_set_header(http, "Authorization", authorization) == ESP_OK &&
      esp_http_client_set_header(http, "Content-Type", "application/json") == ESP_OK;
  mbedtls_platform_zeroize(authorization, sizeof(authorization));

  bool accepted = false;
  if (headers && esp_http_client_open(http, transfer.length) == ESP_OK &&
      esp_http_client_write(http, transfer.body, transfer.length) == transfer.length) {
    const int64_t declared = esp_http_client_fetch_headers(http);
    if (declared >= 0 && declared <= int64_t(MAX_RESPONSE_BYTES) &&
        esp_http_client_get_status_code(http) == 200) {
      LimitedResponse response;
      // fetch_headers puede haber guardado ya todo el cuerpo. Leer incluso si
      // el parser interno ya marcó la respuesta como completa.
      const int length = esp_http_client_read(http, response.data, sizeof(response.data));
      if (length > 0 && length <= int(MAX_RESPONSE_BYTES) && (declared == 0 || declared == length) &&
          esp_http_client_is_complete_data_received(http)) {
        response.length = size_t(length);
        accepted = parseReply(response, transfer.poll, transfer.reply);
      }
    }
  }
  esp_http_client_cleanup(http);
  return accepted;
}

void mailboxWorker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Emparejado con la publicación del request antes de la notificación.
    if (transfer.state.load() != TransferState::InFlight) continue;
    transfer.reply = {};
    transfer.valid = performRequest();
    transfer.state.store(TransferState::Complete);
  }
}

char *appendJsonSsid(char *out, const char *ssid) {
  *out++ = '"';
  for (const char *p = ssid; *p != '\0'; ++p) {
    const uint8_t byte = uint8_t(*p);
    if (byte == '"' || byte == '\\') {
      *out++ = '\\';
      *out++ = *p;
    } else if (byte < 0x20) {
      memcpy(out, "\\u00", 4);
      out += 4;
      writeHex(&byte, 1, out);
      out += 2;
    } else {
      *out++ = *p;
    }
  }
  *out++ = '"';
  return out;
}

class Manager {
 public:
  void run(const char *device, const char *boot);

 private:
  static constexpr size_t SAVED_INDEX = 0;
  static constexpr size_t TRIAL_INDEX = COMPILED_NETWORKS + 1;
  static constexpr size_t NO_NETWORK = COMPILED_NETWORKS + 2;

  void tick(uint32_t now);
  void beginStation(size_t index, uint32_t now);
  void serviceStation(uint32_t now);
  void networkUp(size_t index, uint32_t now);
  void associationFailed(uint32_t now);
  WifiNetwork credentials(size_t index) const;
  size_t firstNetwork() const { return hasSaved ? SAVED_INDEX : 1; }
  void startPortal(uint32_t now);
  void beginScan(Operation requested, uint32_t now);
  void finishScan(bool success, uint32_t now);
  void beginTrial(Operation requested, uint32_t deadline, uint32_t now);
  void finishTrial(bool success, uint32_t now);
  bool startRequest(bool poll, uint32_t now);
  void serviceTransfer(uint32_t now);
  void handlePoll(const MailboxReply &reply, uint32_t started, uint32_t received);
  void finishJob(Result result);
  bool newCommand(const char *id);

  const char *deviceId = nullptr;
  const char *bootId = nullptr;
  bool scanHandlerReady = false;
  Station station = Station::Idle;
  Operation operation = Operation::None;
  size_t target = NO_NETWORK;
  size_t lastStation = NO_NETWORK;
  size_t restore = NO_NETWORK;
  bool restoring = false;
  uint32_t route = 0;
  uint32_t attemptEnd = 0;
  uint32_t operationEnd = 0;
  uint32_t retryAt = 0;
  uint32_t retryDelay = INITIAL_RETRY_MS;
  uint32_t pollAt = 0;
  uint32_t probeAt = 0;
  uint32_t portalAt = 0;
  uint32_t portalCloseAt = 0;
  bool cloudConfirmed = false;
  uint32_t lastCloudReachable = 0;
  uint32_t observedTelemetry = 0;
  bool hasSaved = false;
  Profile saved = {};
  Profile candidate = {};

  // Exacto durante este boot. Si se agota, no se aceptan nuevos comandos:
  // desplazar un ID permitiría repetir una operación anterior.
  uint8_t seenIds[REMOTE_ENABLED ? 256 : 1][ID_BYTES];
  size_t seenCount = 0;
  char jobId[ID_BYTES * 2 + 1] = {};
  uint32_t jobEnd = 0;
  Result pendingResult = Result::None;
  ScanResults remoteNetworks = {};
};

Manager manager;
const char *startIds[2];

WifiNetwork Manager::credentials(size_t index) const {
  if (index == SAVED_INDEX) return {saved.ssid, saved.password};
  if (index == TRIAL_INDEX) return {candidate.ssid, candidate.password};
  return WIFI_NETWORKS[index - 1];
}

void Manager::beginStation(size_t index, uint32_t now) {
  ++route;
  target = index;
  station = Station::Disconnecting;
  attemptEnd = now + ATTEMPT_MS;
  if (index != TRIAL_INDEX) {
    Serial.printf("{\"type\":\"wifi\",\"status\":\"trying\",\"network\":%u}\n", unsigned(index));
  }
  esp_wifi_disconnect();
}

void Manager::networkUp(size_t index, uint32_t now) {
  station = Station::Online;
  lastStation = index;
  restoring = false;
  retryDelay = INITIAL_RETRY_MS;
  pollAt = now;
  cloudConfirmed = false;
  lastCloudReachable = now;
  observedTelemetry = telemetrySuccessCount.load();
  usableStation.store(true);
  const IPAddress ip = WiFi.localIP();
  Serial.printf("{\"type\":\"wifi\",\"status\":\"connected\",\"network\":%u,"
                "\"ip\":\"%u.%u.%u.%u\"}\n", unsigned(index), ip[0], ip[1], ip[2], ip[3]);
}

void Manager::associationFailed(uint32_t now) {
  esp_wifi_disconnect();
  if (target == TRIAL_INDEX) {
    finishTrial(false, now);
  } else if (restoring) {
    restoring = false;
    beginStation(firstNetwork(), now);
  } else if (target < COMPILED_NETWORKS) {
    beginStation(target + 1, now);
  } else {
    station = Station::Idle;
    retryAt = now + retryDelay;
    retryDelay = retryDelay < MAX_RETRY_MS / 2 ? retryDelay * 2 : MAX_RETRY_MS;
    Serial.println("{\"type\":\"wifi\",\"status\":\"cycle_failed\"}");
    startPortal(now);
  }
}

void Manager::serviceStation(uint32_t now) {
  switch (station) {
    case Station::Idle:
      if (reached(now, retryAt)) beginStation(firstNetwork(), now);
      break;
    case Station::Online:
      if (!attached()) {
        usableStation.store(false);
        lastStation = NO_NETWORK;
        Serial.println("{\"type\":\"wifi\",\"status\":\"disconnected\"}");
        if (Portal::active() && !reached(now, portalCloseAt)) {
          portalCloseAt = now;
          Portal::publish(Portal::Status::Ready);
        }
        beginStation(firstNetwork(), now);
      }
      break;
    case Station::Disconnecting:
    case Station::Associating: {
      if (reached(now, attemptEnd)) {
        associationFailed(now);
        break;
      }
      const WifiNetwork network = credentials(target);
      if (station == Station::Disconnecting) {
        // No confundir el IP/DHCP anterior (también si el SSID coincide) con
        // una asociación nueva. Esperar a que el netif notifique la baja.
        if (WiFi.STA.connected() || WiFi.STA.hasIP()) break;
        if (connectStation(network.ssid, network.password)) station = Station::Associating;
        else associationFailed(now);
      } else if (attachedTo(network.ssid)) {
        // Si SNTP agotó reintentos en otra red, reiniciarlo ya sobre esta ruta.
        if (time(nullptr) < MIN_TLS_TIME) configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        if (target == TRIAL_INDEX) {
          station = Station::Verifying;
          probeAt = now;
        } else {
          networkUp(target, now);
        }
      }
      break;
    }
    case Station::Verifying:
      if (!attachedTo(candidate.ssid)) {
        finishTrial(false, now);
      } else if (reached(now, probeAt) && time(nullptr) >= MIN_TLS_TIME && startRequest(false, now)) {
        probeAt = now + PROBE_RETRY_MS;
      }
      break;
  }
}

void Manager::startPortal(uint32_t now) {
  if (!AP_ENABLED || transfer.worker == nullptr || Portal::active() || !reached(now, portalAt)) return;
  portalAt = now + INITIAL_RETRY_MS;
  portalCloseAt = now;
  if (Portal::start(deviceId, WIFI_SETUP_PASSWORD)) beginScan(Operation::LocalScan, millis());
}

void Manager::beginScan(Operation requested, uint32_t now) {
  operation = requested;
  operationEnd = now + SCAN_MS;
  // Una búsqueda manual manda sobre el intento automático todavía sin IP.
  if (station != Station::Online && station != Station::Idle) {
    esp_wifi_disconnect();
    station = Station::Idle;
    retryAt = now;
  }
  scanEvent.store(-1);
  wifi_scan_config_t config = {};
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  config.scan_time.active.min = 100;
  config.scan_time.active.max = 220;
  if (!scanHandlerReady || esp_wifi_scan_start(&config, false) != ESP_OK) finishScan(false, now);
}

void Manager::finishScan(bool success, uint32_t now) {
  if (operation == Operation::LocalScan) {
    ScanResults results = {};
    if (success) collectScan(results);
    Portal::publish(success ? Portal::Status::Ready : Portal::Status::ScanFailed, &results);
  } else {
    if (success) collectScan(remoteNetworks);
    finishJob(success ? Result::Scanned : Result::Failed);
    pollAt = now;
  }
  esp_wifi_clear_ap_list();
  operation = Operation::None;
}

void Manager::beginTrial(Operation requested, uint32_t deadline, uint32_t now) {
  operation = requested;
  operationEnd = deadline;
  restore = station == Station::Online ? lastStation : NO_NETWORK;
  restoring = false;
  usableStation.store(false);
  ++switchCount;
  beginStation(TRIAL_INDEX, now);
}

void Manager::finishTrial(bool success, uint32_t now) {
  const bool local = operation == Operation::LocalConnect;
  operation = Operation::None;
  if (success) {
    saved = candidate;
    hasSaved = true;
    networkUp(SAVED_INDEX, now);
    cloudConfirmed = true;
  } else {
    esp_wifi_disconnect();
    clearStationProfile();
    restoring = restore != NO_NETWORK && restore != firstNetwork();
    beginStation(restore != NO_NETWORK ? restore : firstNetwork(), now);
  }
  mbedtls_platform_zeroize(&candidate, sizeof(candidate));
  if (local) {
    if (success) portalCloseAt = now + LOCAL_SAVED_GRACE_MS;
    Portal::publish(success ? Portal::Status::Saved : Portal::Status::Failed);
  } else {
    finishJob(success ? Result::Saved : Result::Failed);
    pollAt = now;
  }
}

bool Manager::startRequest(bool poll, uint32_t now) {
  if (transfer.worker == nullptr || transfer.state.load() != TransferState::Idle) return false;
  char *out = transfer.body + sprintf(transfer.body, "{\"device_id\":\"%s\",\"boot_id\":\"%s\",\"accept_command\":%s",
                             deviceId, bootId, poll ? "true" : "false");
  if (REMOTE_ENABLED && poll && pendingResult != Result::None) {
    const char *status = pendingResult == Result::Scanned ? "scanned" : pendingResult == Result::Saved ? "saved" : "failed";
    out += sprintf(out, ",\"result\":{\"id\":\"%s\",\"status\":\"%s\"", jobId, status);
    if (pendingResult == Result::Scanned) {
      memcpy(out, ",\"networks\":[", 13);
      out += 13;
      for (uint8_t i = 0; i < remoteNetworks.count; ++i) {
        const Network &network = remoteNetworks.networks[i];
        if (i != 0) *out++ = ',';
        memcpy(out, "{\"ssid\":", 8);
        out = appendJsonSsid(out + 8, network.ssid);
        out += sprintf(out, ",\"rssi\":%d,\"secured\":%s}", int(network.rssi), network.secured ? "true" : "false");
      }
      *out++ = ']';
    }
    *out++ = '}';
  }
  *out++ = '}';

  transfer.route = route;
  transfer.startedAt = now;
  transfer.poll = poll;
  transfer.length = int(out - transfer.body);
  transfer.state.store(TransferState::InFlight);
  xTaskNotifyGive(transfer.worker);
  return true;
}

bool Manager::newCommand(const char *id) {
  uint8_t binary[ID_BYTES];
  if (seenCount == sizeof(seenIds) / sizeof(seenIds[0]) || !readHex(id, binary, sizeof(binary))) return false;
  for (size_t i = 0; i < seenCount; ++i) {
    if (memcmp(seenIds[i], binary, sizeof(binary)) == 0) return false;
  }
  memcpy(seenIds[seenCount++], binary, sizeof(binary));
  return true;
}

void Manager::finishJob(Result result) { pendingResult = result; }

void Manager::serviceTransfer(uint32_t now) {
  if (transfer.state.load() != TransferState::Complete) return;
  transfer.state.store(TransferState::Idle);
  if (transfer.route != route) return;
  if (transfer.poll) {
    if (operation != Operation::None || station != Station::Online || !attached()) return;
    pollAt = now + POLL_MS;
    if (transfer.valid) handlePoll(transfer.reply, transfer.startedAt, now);
  } else if (station == Station::Verifying && transfer.valid &&
             !reached(now, operationEnd) && attachedTo(candidate.ssid)) {
    // La asociación no cambió mientras la petición estaba en vuelo.
    const bool saved = saveProfile(candidate);
    finishTrial(saved, millis());
  }
}

void Manager::handlePoll(const MailboxReply &reply, uint32_t started, uint32_t received) {
  const bool hasResult = pendingResult != Result::None;
  const bool acknowledged = hasResult && strcmp(jobId, reply.resultId) == 0;
  if (reply.resultId[0] != '\0' && !acknowledged) return;
  const bool superseded = reply.command.kind != Kind::None && strcmp(jobId, reply.command.id) != 0;
  cloudConfirmed = true;
  lastCloudReachable = received;
  if (hasResult) {
    if (!acknowledged && !superseded && !reached(received, jobEnd)) return;
    finishJob(Result::None);
  }
  if (reply.command.kind == Kind::None) return;
  const uint32_t deadline = started + reply.command.lifetime;
  if (reached(received, deadline) ||
      (Portal::active() && (!reached(received, portalCloseAt) || !Portal::stopWhenIdle()))) return;
  received = millis();
  if (reached(received, deadline) || !newCommand(reply.command.id)) return;
  memcpy(jobId, reply.command.id, sizeof(jobId));
  jobEnd = deadline;
  if (reply.command.kind == Kind::Scan) {
    beginScan(Operation::RemoteScan, received);
  } else if (decryptProfile(reply.command, candidate)) {
    const uint32_t trialDeadline = received + TRIAL_MS;
    beginTrial(Operation::RemoteConnect, reached(trialDeadline, deadline) ? deadline : trialDeadline, received);
  } else {
    finishJob(Result::Failed);
    pollAt = received;
  }
}

void Manager::tick(uint32_t now) {
  serviceTransfer(now);
  now = millis();
  if (operation == Operation::None && Portal::active()) {
    switch (Portal::takeAction(candidate)) {
      case Portal::Action::Scan:
        beginScan(Operation::LocalScan, now);
        break;
      case Portal::Action::Connect:
        beginTrial(Operation::LocalConnect, now + TRIAL_MS, now);
        break;
      case Portal::Action::None:
        break;
    }
  }
  if (operation == Operation::LocalScan || operation == Operation::RemoteScan) {
    const int complete = scanEvent.load();
    const bool expired = reached(now, operationEnd) || (operation == Operation::RemoteScan && reached(now, jobEnd));
    if (complete >= 0 || expired) {
      if (complete < 0) esp_wifi_scan_stop();
      finishScan(complete == 1 && !expired, now);
    }
    return;
  }
  if (operation == Operation::LocalConnect || operation == Operation::RemoteConnect) {
    if (reached(now, operationEnd)) finishTrial(false, now);
    else serviceStation(now);
    return;
  }

  serviceStation(now);
  if (station != Station::Online || !attached()) return;
  const uint32_t successes = telemetrySuccessCount.load();
  if (successes != observedTelemetry) {
    observedTelemetry = successes;
    cloudConfirmed = true;
    lastCloudReachable = now;
  }
  if (uint32_t(now - lastCloudReachable) >= CLOUD_OUTAGE_MS) {
    startPortal(now);
  } else if (cloudConfirmed && Portal::active() && reached(now, portalCloseAt)) {
    Portal::stopWhenIdle();
  }
  if (REMOTE_ENABLED && operation == Operation::None && reached(now, pollAt) && time(nullptr) >= MIN_TLS_TIME) {
    startRequest(true, now);
  }
}

void Manager::run(const char *device, const char *boot) {
  deviceId = device;
  bootId = boot;
  if ((AP_ENABLED || REMOTE_ENABLED) &&
      xTaskCreate(mailboxWorker, "wifi_https", 8192, nullptr, 1, &transfer.worker) != pdPASS) {
    transfer.worker = nullptr;
    Serial.println("{\"type\":\"wifi\",\"status\":\"setup_https_worker_start_failed\"}");
  }
  scanHandlerReady = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scanFinished, nullptr) == ESP_OK;
  hasSaved = loadProfile(saved);
  const uint32_t now = millis();
  retryAt = now;
  portalAt = now;
  beginStation(firstNetwork(), now);
  for (;;) {
    tick(millis());
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setupWorker(void *) { manager.run(startIds[0], startIds[1]); }

}  // namespace
}  // namespace WifiSetup

bool wifiSetupStart(const char *deviceId, const char *bootId) {
  // Únicamente Preferences escribe el perfil verificado; las pruebas son RAM.
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
  WifiSetup::startIds[0] = deviceId;
  WifiSetup::startIds[1] = bootId;
  return xTaskCreate(WifiSetup::setupWorker, "wifi_setup", 5120, nullptr, 1, nullptr) == pdPASS;
}

bool wifiSetupConnected() { return WifiSetup::usableStation.load() && WifiSetup::attached(); }
uint32_t wifiSetupSwitchCount() { return WifiSetup::switchCount.load(); }
void wifiSetupReportCloudReachable() {
  if (WifiSetup::AP_ENABLED) ++WifiSetup::telemetrySuccessCount;
}
