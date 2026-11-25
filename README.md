# Invernadero ESP32

Invernadero automatizado para lechuga, con un ESP32-S3 conectado por Wi-Fi a una plataforma web pública y controles privados para el administrador.

## Estado actual

**Panel público del curso:** <https://proyecto16.cruchi.com/>. La portada sirve el panel de [`cruchi/public/`](cruchi/public/) desde CruChi; `/invernadero/` también funciona. La telemetría entra por HTTPS y se guarda en MySQL. La portada pública no puede enviar órdenes.

**PCB actual (monitorización):** ESP32‑S3, DHT11 en GPIO4 y GY‑302/BH1750 en I²C (SDA GPIO7, SCL GPIO9, dirección `0x23`). Temperatura, humedad, lux y estados GPIO se reportan por serie y a CruChi. Q1 (Peltier y ventilación), Q2 (bomba) y Q3 (LEDs) quedan en LOW, sin órdenes ni PWM. [Mapa de la PCB y precauciones](docs/HARDWARE.md#3-mapa-de-señales).

**Wi‑Fi:** el firmware ofrece un portal local protegido cuando pierde las redes conocidas. El puente de CruChi está publicado; la pantalla privada de Wi‑Fi en CruChi aún no existe. [`web/`](web/) y `supabase/` conservan la fase histórica de Vercel; no son necesarios para la portada ni para el portal local.

La detección y el funcionamiento real de la PCB nueva, el BH1750, GPIO45 durante el arranque y la recuperación Wi‑Fi requieren pruebas físicas. Los resultados de las fases anteriores no certifican este montaje.

## Punto de entrada para colaboradores y agentes

1. Trabajar en `cruchi/fase-1`. Leer [Hardware y seguridad](docs/HARDWARE.md) antes de tocar los pines y [Entrega del firmware](docs/WIFI_ESP32_ENTREGA.md) antes de conectar la placa.
2. Abrir `firmware/invernadero/invernadero.ino` en Arduino IDE, o compilar desde la raíz con `arduino-cli compile --profile esp32s3 firmware/invernadero`. El perfil fija ESP32‑S3, flash de 16 MB y PSRAM OPI; comprobar el módulo físico. Copiar los `.example.h` a `.local.h` y configurar las credenciales por un canal privado, sin versionarlas ni publicar binarios.
3. Para una base nueva, usar [`cruchi/schema.sql`](cruchi/schema.sql). Para una base existente con el esquema inicial de CruChi, aplicar una vez [`001_pcb_lux.sql`](cruchi/migrations/001_pcb_lux.sql) **antes** de publicar la API y el dashboard nuevos. No reaplicar el esquema de creación sobre una base con lecturas. Subir solo los cambios de `cruchi/public/` bajo `/invernadero/`; no reemplazar la portada PHP ni la configuración privada del hosting.
4. Verificar que `state.php` y `readings.php` devuelven lecturas históricas y que el dashboard abre en `/`. La telemetría de la PCB usa `operating_mode: "monitoring"`, `gpio_levels.leds` e `illuminance_lux`: `0` es una lectura real, `null` significa sin medida. Los reportes anteriores conservan `gpio_levels.fans`.

## Documentación

| Documento | Contenido |
| --- | --- |
| [Plan de desarrollo](docs/PLAN.md) | Arquitectura, seguridad, modos, fases detalladas, entregables, pruebas y decisiones pendientes |
| [Hardware y seguridad](docs/HARDWARE.md) | GPIO, bomba, alimentación con batería, MOSFETs, calibración y protección térmica |
| [Entrega del firmware y Wi‑Fi](docs/WIFI_ESP32_ENTREGA.md) | Configuración privada, compilación, portal local y pruebas con la placa |
| [Diseño de interfaz](docs/DISENO_UI.md) | Dirección botánica premium, dashboard, administración, móvil, accesibilidad y estados reales |
| [Registro de fases 1 y 2](docs/FASES_1_2.md) | Mediciones pendientes, configuración privada, compilación, carga y evidencia real |
| [Fase 3 operativa](docs/FASE_3.md) | Enlace público, configuración, despliegue, protocolo, permisos, pruebas y límites |
| [Fase 4 operativa](docs/FASE_4.md) | Autenticación, órdenes confirmadas, firmware, evidencia y límites |

## Arquitectura

- [`firmware/invernadero/`](firmware/invernadero/): sketch Arduino, sensores, salidas bloqueadas, recuperación Wi‑Fi y cliente HTTPS.
- [`cruchi/public/`](cruchi/public/): dashboard claro/oscuro, consultas periódicas y endpoints PHP de telemetría, lecturas y Wi‑Fi. La configuración privada de PHP permanece fuera de la raíz web.
- [`web/`](web/) y [`supabase/`](supabase/): frontend y backend de la etapa Vercel/Supabase. Las pruebas históricas de órdenes y modos se conservan en [Fase 4](docs/FASE_4.md), pero esta PCB no ejecuta esas órdenes.

El ESP32 inicia conexiones salientes a Internet. Laptop y celular abren la web desde cualquier red; no se abrirán puertos del router ni se usará la laptop como puente obligatorio.

## Comportamiento histórico o futuro

| Función | Decisión |
| --- | --- |
| Enfriamiento automático | Umbrales históricos propuestos: encender a 24 °C y apagar a 22 °C; deshabilitado en esta PCB |
| Riego | Sin activación en la PCB actual. Calibrar caudal y revisar protecciones antes de retomar el control |
| Iluminación | Sin activación en la PCB actual: el BH1750 solo mide iluminancia |
| Fallos de Internet | El panel marca las lecturas desactualizadas; el ESP32 puede abrir su portal local protegido |

Los umbrales ambientales no son los límites de protección térmica del disipador. Una orden aplicada al GPIO tampoco confirma funcionamiento físico de bomba o ventiladores sin sensores de realimentación.

## Hoja de ruta

Estado real por fase:

1. Verificación eléctrica: revisión documental realizada; mediciones físicas pendientes.
2. Firmware base: validado por USB con DHT11, Wi-Fi, memoria esperada y salidas LOW; recuperación de red y pruebas de potencia no realizadas.
3. Nube y dashboard: desplegados y verificados con datos reales del ESP32; detalle en [Fase 3](docs/FASE_3.md).
4. Administrador y órdenes: desplegados y verificados sin cargas; detalle en [Fase 4](docs/FASE_4.md).
5. Calibrar riego e implementar automatización local.
6. Incorporar protección térmica y habilitar Peltier; fase pospuesta.
7. Ensayar evaluación sin laptop, incluidos reinicios y desconexiones.

La nube y el frontend pueden desarrollarse sin alimentar las cargas. Las pruebas de potencia requieren superar la revisión eléctrica, y el enfriamiento no puede declararse operativo si la fase térmica sigue pendiente.

## Antes de implementar

- Contrastar el mapa de GPIO y orientación con la PCB ensamblada.
- Revisar excitación de los IRLZ44N a 3,3 V, alimentación, fusibles y cableado.
- Definir macetas/sustrato, medir caudal y aprobar la programación de riego.
- Configurar la cuenta administradora en fase 4; los proyectos Vercel/Supabase ya existen y sus secretos no se versionan.

Las fases anteriores son registros históricos. Para trabajar en la versión de CruChi y la PCB actual, sigue el punto de entrada de este archivo.