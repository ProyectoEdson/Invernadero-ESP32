#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace WifiSetup {

constexpr size_t MAX_NETWORKS = 12;
constexpr size_t MAX_SSID_BYTES = 32;
constexpr size_t MAX_PASSWORD_BYTES = 63;

// Also the entire versioned Preferences record; no pointers or padding.
struct Profile {
  char ssid[MAX_SSID_BYTES + 1];
  char password[MAX_PASSWORD_BYTES + 1];
};

struct Network {
  char ssid[MAX_SSID_BYTES + 1];
  int8_t rssi;
  bool secured;
};

struct ScanResults {
  Network networks[MAX_NETWORKS];
  uint8_t count;
};

inline bool validText(const char *text, size_t size) {
  for (size_t i = 0; i < size;) {
    const uint8_t first = uint8_t(text[i++]);
    if (first == 0) return false;
    if (first < 0x80) continue;
    if (first < 0xC2 || first > 0xF4) return false;
    const size_t remaining = first < 0xE0 ? 1 : first < 0xF0 ? 2 : 3;
    if (remaining > size - i) return false;
    const uint8_t next = uint8_t(text[i]);
    if ((first == 0xE0 && next < 0xA0) || (first == 0xED && next >= 0xA0) ||
        (first == 0xF0 && next < 0x90) || (first == 0xF4 && next >= 0x90)) return false;
    for (size_t j = 0; j < remaining; ++j) {
      if ((uint8_t(text[i++]) & 0xC0) != 0x80) return false;
    }
  }
  return true;
}

inline bool validProfile(const Profile &profile) {
  const size_t ssid = strnlen(profile.ssid, sizeof(profile.ssid));
  const size_t password = strnlen(profile.password, sizeof(profile.password));
  return ssid > 0 && ssid <= MAX_SSID_BYTES && password <= MAX_PASSWORD_BYTES &&
      (password == 0 || password >= 8) && validText(profile.ssid, ssid) &&
      validText(profile.password, password);
}

constexpr int hexDigit(char ch) {
  return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

inline bool readHex(const char *hex, uint8_t *bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const int high = hexDigit(hex[2 * i]);
    const int low = hexDigit(hex[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    bytes[i] = uint8_t((high << 4) | low);
  }
  return true;
}

inline void writeHex(const uint8_t *bytes, size_t count, char *hex) {
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < count; ++i) {
    hex[2 * i] = digits[bytes[i] >> 4];
    hex[2 * i + 1] = digits[bytes[i] & 15];
  }
  hex[2 * count] = '\0';
}

namespace Portal {

enum class Action : uint8_t { None, Scan, Connect };
enum class Status : uint8_t { Closed, Ready, Scanning, ScanFailed, Testing, Failed, Saved };

bool start(const char *deviceId, const char *password);
bool active();
bool stopWhenIdle();
Action takeAction(Profile &profile);
void publish(Status status, const ScanResults *results = nullptr);

}  // namespace Portal
}  // namespace WifiSetup
