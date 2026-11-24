#pragma once

// Copiar a cloud.local.h (privado). El destino y la CA se fijan en cruchi_tls.h.

// Token compartido con el backend. No imprimirlo ni incluirlo en el repositorio.
#define CLOUD_DEVICE_TOKEN ""

// Opcional: contraseña exclusiva por placa, 12–63 caracteres ASCII imprimibles.
// Usa 16 o más caracteres aleatorios. Vacía (o ausente) desactiva el AP.
// Conéctate a Invernadero-<sufijo de la placa> y abre http://192.168.173.1/.
#define WIFI_SETUP_PASSWORD ""

// Opcional: 64 dígitos hexadecimales en minúscula, iguales a la variable
// privada CRUCHI_WIFI_DEVICE_KEY de Vercel. Vacía desactiva el sondeo remoto.
#define WIFI_REMOTE_KEY ""
