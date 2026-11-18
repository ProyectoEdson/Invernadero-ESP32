#pragma once

#include <cstddef>

// Cada entrada se intenta en orden hasta que una red responde. La primera
// debería ser la habitual: un intento fallido cuesta hasta 10 segundos.
struct WifiNetwork {
  const char *ssid;
  const char *password;
};

constexpr size_t wifiTextLength(const char *text) {
  return text == nullptr ? 0 : *text == '\0' ? 0 : 1 + wifiTextLength(text + 1);
}

// Vive en la cabecera para no añadir una función al sketch antes de sus tipos:
// el generador de prototipos de Arduino las colocaría antes de los structs.
template <size_t N>
constexpr bool wifiNetworksUsable(const WifiNetwork (&networks)[N]) {
  for (size_t i = 0; i < N; ++i) {
    const size_t ssidLength = wifiTextLength(networks[i].ssid);
    if (ssidLength < 1 || ssidLength > 32 ||
        wifiTextLength(networks[i].password) > 63) {
      return false;
    }
  }
  return N > 0;
}
