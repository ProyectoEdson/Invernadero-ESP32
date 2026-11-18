#pragma once

// Copiar a cloud.local.h (privado). Completar antes de compilar.
// URL HTTPS completa del endpoint /api/telemetry; nunca HTTP ni una IP sin SAN.
#define CLOUD_API_URL ""

// Token compartido con el backend. No imprimirlo ni incluirlo en el repositorio.
#define CLOUD_DEVICE_TOKEN ""

// Certificado raíz PEM que valida la cadena TLS del hostname de CLOUD_API_URL.
// Sustituir "" por un literal raw R"PEM(...certificado raíz PEM...)PEM".
#define CLOUD_ROOT_CA ""
