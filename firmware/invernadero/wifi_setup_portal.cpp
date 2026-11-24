#include "wifi_setup_internal.h"

#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <lwip/sockets.h>
#include <mbedtls/platform_util.h>

namespace WifiSetup::Portal {
namespace {

constexpr char AP_HOST[] = "192.168.173.1";
constexpr char AP_ORIGIN[] = "http://192.168.173.1";
const IPAddress AP_IP(192, 168, 173, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
constexpr size_t MAX_FORM_BYTES = 512;

httpd_handle_t server = nullptr;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Status status = Status::Closed;
Action pending = Action::None;
Profile pendingProfile = {};
ScanResults visibleNetworks = {};
char nonce[33];  // Inmutable mientras el servidor está activo.

bool socketAddress(int socket, bool peer, uint32_t &address) {
  sockaddr_storage raw = {};
  socklen_t length = sizeof(raw);
  auto *value = reinterpret_cast<sockaddr *>(&raw);
  if ((peer ? getpeername(socket, value, &length) : getsockname(socket, value, &length)) != 0) {
    return false;
  }
  if (raw.ss_family == AF_INET) {
    address = reinterpret_cast<const sockaddr_in *>(&raw)->sin_addr.s_addr;
    return true;
  }
  if (raw.ss_family == AF_INET6) {
    const auto &ipv6 = reinterpret_cast<const sockaddr_in6 *>(&raw)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
      memcpy(&address, ipv6.s6_addr + 12, sizeof(address));
      return true;
    }
  }
  return false;
}

bool fromSetupStation(int socket) {
  uint32_t local, peer;
  if (!socketAddress(socket, false, local) || local != uint32_t(AP_IP) ||
      !socketAddress(socket, true, peer) ||
      (peer & uint32_t(AP_MASK)) != (local & uint32_t(AP_MASK))) return false;
  // No basta que el origen afirme estar en la subred: debe tener una concesión
  // DHCP del AP y seguir asociado a esa interfaz.
  wifi_sta_list_t stations = {};
  wifi_sta_mac_ip_list_t leases = {};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK ||
      esp_wifi_ap_get_sta_list_with_ip(&stations, &leases) != ESP_OK) return false;
  for (int i = 0; i < leases.num; ++i) {
    if (leases.sta[i].ip.addr == peer) return true;
  }
  return false;
}

esp_err_t openSession(httpd_handle_t, int socket) {
  if (!fromSetupStation(socket)) return ESP_FAIL;
  // Restringir también el socket aceptado a la interfaz del AP.
  ifreq interface = {};
  if (esp_netif_get_netif_impl_name(WiFi.AP.netif(), interface.ifr_name) != ESP_OK ||
      setsockopt(socket, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) != 0) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

bool headerEquals(httpd_req_t *request, const char *header, const char *expected) {
  char value[64];
  const size_t length = strlen(expected);
  return httpd_req_get_hdr_value_len(request, header) == length &&
      httpd_req_get_hdr_value_str(request, header, value, sizeof(value)) == ESP_OK &&
      memcmp(value, expected, length) == 0;
}

bool allowed(httpd_req_t *request) {
  return fromSetupStation(httpd_req_to_sockfd(request)) &&
      headerEquals(request, "Host", AP_HOST);
}

void responseHeaders(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(request, "Content-Security-Policy",
                     "default-src 'none'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}

esp_err_t reply(httpd_req_t *request, const char *code, const char *message) {
  responseHeaders(request);
  httpd_resp_set_status(request, code);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(request, message);
}

const char *statusText(Status current) {
  switch (current) {
    case Status::Scanning: return "Buscando redes de 2,4 GHz…";
    case Status::ScanFailed: return "No se pudo buscar redes. Puedes reintentar o introducir el SSID.";
    case Status::Testing: return "Probando la red y la conexión con CruChi. El enlace local puede interrumpirse momentáneamente.";
    case Status::Failed: return "No se guardó la red. Revisa las credenciales y que la red tenga acceso a Internet.";
    case Status::Saved: return "Red guardada y verificada con CruChi. La red de configuración se cerrará en breve.";
    default: return "Elige una red de 2,4 GHz o introduce manualmente su nombre.";
  }
}

void escapeSsid(const char *input, char *output) {
  for (; *input; ++input) {
    const char *entity = nullptr;
    switch (*input) {
      case '&': entity = "&amp;"; break;
      case '<': entity = "&lt;"; break;
      case '>': entity = "&gt;"; break;
      case '"': entity = "&quot;"; break;
      case '\'': entity = "&#39;"; break;
    }
    if (entity != nullptr) {
      const size_t length = strlen(entity);
      memcpy(output, entity, length);
      output += length;
    } else {
      *output++ = *input;
    }
  }
  *output = '\0';
}

esp_err_t page(httpd_req_t *request) {
  if (!allowed(request)) return reply(request, "403 Forbidden", "Acceso no permitido.");
  ScanResults results;
  portENTER_CRITICAL(&lock);
  const Status current = status;
  results = visibleNetworks;
  portEXIT_CRITICAL(&lock);
  if (current == Status::Closed) return reply(request, "503 Service Unavailable", "Configuración no disponible.");
  responseHeaders(request);
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  if (httpd_resp_sendstr_chunk(request,
      "<!doctype html><html lang='es'><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Wi-Fi del invernadero</title>") != ESP_OK) return ESP_FAIL;
  const bool busy = current == Status::Scanning || current == Status::Testing;
  if (busy && httpd_resp_sendstr_chunk(request, "<meta http-equiv='refresh' content='3'>") != ESP_OK) return ESP_FAIL;
  if (httpd_resp_sendstr_chunk(request, "<main><h1>Wi-Fi del invernadero</h1><p>") != ESP_OK ||
      httpd_resp_sendstr_chunk(request, statusText(current)) != ESP_OK ||
      httpd_resp_sendstr_chunk(request, "</p>") != ESP_OK) return ESP_FAIL;
  if (!busy && current != Status::Saved) {
    char part[384];
    snprintf(part, sizeof(part), "<form method='post' action='/scan'><input type='hidden' name='nonce' value='%s'>"
             "<button>Buscar redes</button></form><form method='post' action='/connect'>"
             "<input type='hidden' name='nonce' value='%s'><p><label>Red <select name='network'>"
             "<option value=''>Introducir SSID manualmente</option>", nonce, nonce);
    if (httpd_resp_sendstr_chunk(request, part) != ESP_OK) return ESP_FAIL;
    for (uint8_t i = 0; i < results.count; ++i) {
      const Network &network = results.networks[i];
      char escaped[MAX_SSID_BYTES * 6 + 1];
      char hex[MAX_SSID_BYTES * 2 + 1];
      escapeSsid(network.ssid, escaped);
      writeHex(reinterpret_cast<const uint8_t *>(network.ssid), strlen(network.ssid), hex);
      snprintf(part, sizeof(part), "<option value='%s'>%s (%d dBm%s)</option>",
               hex, escaped, int(network.rssi), network.secured ? ", protegida" : ", abierta");
      if (httpd_resp_sendstr_chunk(request, part) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(request,
        "</select></label></p><p><label>SSID manual <input name='ssid' maxlength='32' autocapitalize='none'></label></p>"
        "<p><label>Contraseña (vacía si la red es abierta) <input type='password' name='password' maxlength='63' "
        "autocomplete='new-password'></label></p><button>Probar y guardar</button></form>") != ESP_OK) return ESP_FAIL;
  }
  if (httpd_resp_sendstr_chunk(request, "</main></html>") != ESP_OK) return ESP_FAIL;
  return httpd_resp_sendstr_chunk(request, nullptr);
}

int formHex(char ch) {
  if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
  return hexDigit(ch);
}

bool decodeValue(char *value) {
  char *output = value;
  for (const char *p = value; *p != '\0'; ++p) {
    char ch = *p;
    if (ch == '+') ch = ' ';
    else if (ch == '%') {
      if (p[1] == '\0' || p[2] == '\0') return false;
      const int high = formHex(*++p);
      const int low = formHex(*++p);
      if (high < 0 || low < 0) return false;
      ch = char((high << 4) | low);
      if (ch == '\0') return false;
    }
    *output++ = ch;
  }
  *output = '\0';
  return true;
}

bool parseForm(char *form, Action action, Profile &profile) {
  const char *submittedNonce = nullptr;
  const char *network = nullptr;
  const char *ssid = nullptr;
  const char *password = nullptr;
  for (char *field = form; field != nullptr;) {
    char *separator = strchr(field, '&');
    if (separator != nullptr) *separator++ = '\0';
    char *value = strchr(field, '=');
    if (value == nullptr) return false;
    *value++ = '\0';
    const char **destination = nullptr;
    if (strcmp(field, "nonce") == 0) destination = &submittedNonce;
    else if (action == Action::Connect) {
      if (strcmp(field, "network") == 0) destination = &network;
      if (strcmp(field, "ssid") == 0) destination = &ssid;
      if (strcmp(field, "password") == 0) destination = &password;
    }
    if (destination == nullptr || *destination != nullptr || !decodeValue(value)) return false;
    *destination = value;
    field = separator;
  }
  if (submittedNonce == nullptr || strcmp(submittedNonce, nonce) != 0) return false;
  if (action == Action::Scan) return true;
  if (network == nullptr || ssid == nullptr || password == nullptr) return false;
  if (*network != '\0') {
    const size_t length = strlen(network);
    if (length > MAX_SSID_BYTES * 2 || length % 2 != 0 ||
        !readHex(network, reinterpret_cast<uint8_t *>(profile.ssid), length / 2) ||
        !validText(profile.ssid, length / 2)) return false;
  } else {
    const size_t length = strlen(ssid);
    if (length > MAX_SSID_BYTES) return false;
    memcpy(profile.ssid, ssid, length);
  }
  const size_t passwordLength = strlen(password);
  if (passwordLength > MAX_PASSWORD_BYTES) return false;
  memcpy(profile.password, password, passwordLength);
  return validProfile(profile);
}

esp_err_t submit(httpd_req_t *request, Action action) {
  if (!allowed(request) || (httpd_req_get_hdr_value_len(request, "Origin") != 0 &&
      !headerEquals(request, "Origin", AP_ORIGIN))) {
    return reply(request, "403 Forbidden", "Acceso no permitido.");
  }
  if (!headerEquals(request, "Content-Type", "application/x-www-form-urlencoded") ||
      request->content_len == 0 || request->content_len > MAX_FORM_BYTES) {
    return reply(request, "400 Bad Request", "Formulario no válido.");
  }
  char form[MAX_FORM_BYTES + 1];
  size_t received = 0;
  const uint32_t start = millis();
  while (received < request->content_len && uint32_t(millis() - start) < 3000) {
    const int count = httpd_req_recv(request, form + received, request->content_len - received);
    if (count <= 0) break;
    received += count;
  }
  form[received] = '\0';
  Profile profile = {};
  const bool valid = received == request->content_len && memchr(form, '\0', received) == nullptr &&
      parseForm(form, action, profile);
  mbedtls_platform_zeroize(form, sizeof(form));
  if (!valid) {
    mbedtls_platform_zeroize(&profile, sizeof(profile));
    return reply(request, "400 Bad Request", "Formulario no válido. Abre la página de configuración e inténtalo nuevamente.");
  }
  portENTER_CRITICAL(&lock);
  const bool available = status == Status::Ready || status == Status::Failed || status == Status::ScanFailed;
  if (available) {
    if (action == Action::Connect) pendingProfile = profile;
    pending = action;
    status = action == Action::Connect ? Status::Testing : Status::Scanning;
  }
  portEXIT_CRITICAL(&lock);
  mbedtls_platform_zeroize(&profile, sizeof(profile));
  if (!available) return reply(request, "409 Conflict", "Ya hay una operación en curso o la configuración se cerró.");
  responseHeaders(request);
  httpd_resp_set_status(request, "303 See Other");
  httpd_resp_set_hdr(request, "Location", "/");
  return httpd_resp_send(request, nullptr, 0);
}

esp_err_t scan(httpd_req_t *request) { return submit(request, Action::Scan); }
esp_err_t connect(httpd_req_t *request) { return submit(request, Action::Connect); }

}  // namespace

bool start(const char *deviceId, const char *password) {
  char name[24];
  snprintf(name, sizeof(name), "Invernadero-%s", deviceId + strlen(deviceId) - 6);
  if (!WiFi.softAP(name, password, 1, false, 2, false, WIFI_AUTH_WPA2_PSK) ||
      !WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK)) {
    WiFi.AP.end();
    return false;
  }
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  writeHex(random, sizeof(random), nonce);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.open_fn = openSession;
  config.max_open_sockets = 3;
  config.max_uri_handlers = 3;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 2;
  config.lru_purge_enable = true;
  const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = page, .user_ctx = nullptr},
    {.uri = "/scan", .method = HTTP_POST, .handler = scan, .user_ctx = nullptr},
    {.uri = "/connect", .method = HTTP_POST, .handler = connect, .user_ctx = nullptr},
  };
  if (httpd_start(&server, &config) == ESP_OK) {
    bool registered = true;
    for (const auto &route : routes) registered &= httpd_register_uri_handler(server, &route) == ESP_OK;
    if (registered) {
      publish(Status::Scanning);
      return true;
    }
    httpd_stop(server);
  }
  server = nullptr;
  WiFi.AP.end();
  return false;
}

bool active() { return server != nullptr; }

bool stopWhenIdle() {
  if (!active()) return true;
  portENTER_CRITICAL(&lock);
  const bool canStop = pending == Action::None;
  if (canStop) {
    status = Status::Closed;
    visibleNetworks.count = 0;
  }
  portEXIT_CRITICAL(&lock);
  if (!canStop) return false;
  httpd_stop(server);
  server = nullptr;
  // Cerrar sin resetear antes a la configuración abierta por defecto.
  WiFi.AP.end();
  return true;
}

Action takeAction(Profile &profile) {
  portENTER_CRITICAL(&lock);
  const Action action = pending;
  if (action == Action::Connect) {
    profile = pendingProfile;
    mbedtls_platform_zeroize(&pendingProfile, sizeof(pendingProfile));
  }
  pending = Action::None;
  portEXIT_CRITICAL(&lock);
  return action;
}

void publish(Status next, const ScanResults *results) {
  portENTER_CRITICAL(&lock);
  status = next;
  if (results != nullptr) visibleNetworks = *results;
  portEXIT_CRITICAL(&lock);
}

}  // namespace WifiSetup::Portal
