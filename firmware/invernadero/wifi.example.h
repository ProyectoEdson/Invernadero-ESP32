#pragma once

#include "wifi_config.h"

// Copiar como wifi.local.h y completar localmente. No publicar claves.
// Se prueban en orden, después de cualquier red guardada desde la configuración
// Wi-Fi. Máximo 32 bytes de SSID y 63 de contraseña. Redes de 2,4 GHz.
constexpr WifiNetwork WIFI_NETWORKS[] = {
    {"MI_RED_PRINCIPAL", "CLAVE"},
    {"HOTSPOT_SECUNDARIO", "CLAVE"},
};
