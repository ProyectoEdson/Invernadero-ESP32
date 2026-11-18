# Fase 4: administración y órdenes confirmadas

[Inicio](../README.md) · [Plan](PLAN.md) · [Fase 3](FASE_3.md) · [Hardware](HARDWARE.md)

## 1. Estado

**Implementada, desplegada y probada sobre el montaje sin cargas.** La web pública sigue siendo de solo lectura; el panel privado exige sesión y una cuenta autorizada, y las órdenes solo se consideran aplicadas cuando el ESP32 lo confirma.

Sigue **sin cargas conectadas**: ningún ensayo energizó bomba, ventiladores ni Peltier. La Peltier continúa bloqueada y el modo Automático todavía no riega; la calibración y la programación son fase 5.

Panel: <https://invernadero-esp32-delta.vercel.app/admin>

## 2. Autenticación y autorización

- Supabase Auth con correo y contraseña. El registro público está desactivado en el proyecto; las cuentas se crean por invitación.
- La autorización real se comprueba en el servidor: `auth.getUser()` valida la sesión y su UUID debe coincidir con `ADMIN_USER_ID`. Iniciar sesión no basta.
- Las cookies usan `@supabase/ssr` con `SameSite=Lax`, `Path=/` y `Secure` en HTTPS. No se comparten entre previews.
- El token de servicio de Supabase permanece solo en el servidor. El navegador usa la clave pública con permisos limitados por RLS.
- Las mutaciones exigen `Origin` idéntico al del despliegue. Sin esa cabecera o con otro origen se rechazan.

### Configuración de Auth aplicada

- `site_url` apunta al dominio de producción.
- Lista de redirecciones permitidas: `/auth/confirm` y `/auth/callback`.
- Plantillas de invitación y recuperación apuntan a `/auth/confirm` con `token_hash` y `type`.
- Envío de correo del plan gratuito tiene límite de frecuencia; en la práctica devolvió `429 over_email_send_rate_limit`. Para una operación sostenible conviene configurar SMTP propio.

La contraseña se establece en `/auth/password`, nunca por chat ni en el repositorio. Para desbloquear la primera validación se generó un enlace de recuperación de un solo uso mediante la API de administración y se consumió en la pestaña privada; no se guardó ni se publicó. Esa vía no es un mecanismo cotidiano: caduca y no debe registrarse.

## 3. Rutas y API

| Ruta | Función |
| --- | --- |
| `/admin` | Panel privado: estado, modos, pulso de bomba, ventiladores e historial de 20 órdenes |
| `/auth/password` | Establecimiento de contraseña con sesión válida |
| `/auth/confirm` | Verifica `token_hash` de invitación/recuperación y redirige a un destino interno |
| `/auth/callback` | Intercambio PKCE; mismo destino interno |
| `GET /api/admin/state` | Estado y órdenes recientes para la cuenta autorizada |
| `POST /api/admin/commands` | Crea una orden validada (401/403/404/409/400 según corresponda) |
| `POST /api/telemetry` | Ingreso autenticado del dispositivo; devuelve orden vigente |

`admin_device_state` y `create_device_command` solo se ejecutan como `service_role`. `authenticated` no tiene acceso directo a la tabla de órdenes ni a los cursores privados.

## 4. Modelo de órdenes

`private.device_commands` guarda cada orden con:

- UUID e identidad `sequence` monotónica generada por la base.
- `target_boot_id`: la orden solo puede aplicarse en el arranque que estaba activo al crearla.
- `created_at` y `expires_at` con validez de 15 segundos.
- Estado `pending`, `applied`, `rejected` o `expired`, con motivo acotado.

Restricciones: una única orden pendiente por dispositivo (índice parcial único), acciones cerradas (`set_mode`, `pump_pulse` 1–3000 ms, `set_fans`) y ninguna acción de Peltier. El `ack` solo actualiza órdenes pendientes del mismo arranque; repetirlo no cambia un resultado terminal. Un arranque nuevo rechaza las órdenes pendientes anteriores en lugar de heredarlas.

El servidor exige que la última muestra tenga menos de 15 segundos y que el firmware haya declarado `phase4_control_ready`. Un pulso de bomba se rechaza fuera de modo Manual.

## 5. Firmware

- Arranque en modo Manual con GPIO4, GPIO5 y GPIO7 en bajo.
- La Peltier no tiene ruta de encendido; GPIO4 permanece en bajo.
- Bomba: pulso de 1 a 3000 ms, uno cada vez; un cambio de modo la apaga. Automático no riega.
- Ventiladores: encendido/apagado manual persistente hasta orden contraria o reinicio. No se inventó un temporizador.
- El corte de la bomba usa un temporizador hardware con ISR que solo baja GPIO5; el worker HTTPS nunca escribe GPIO. Es soporte imprescindible para acotar el pulso aunque una lectura DHT o TLS retrasen el loop.
- Deduplicación O(1): marca de agua de `sequence` y último UUID/ack. Un replay no reejecuta ni prolonga plazos.
- El TTL se descuenta desde el inicio del POST, incluido el RTT. Cada primera entrega fija el vencimiento; repetir la respuesta no lo extiende.
- El `ack` vive en RAM hasta que el servidor acepta la muestra que lo contiene; un fallo de transporte no lo consume.
- Worker y loop se comunican por colas; solo el loop escribe el log serie.

El temporizador acota el máximo configurado, no constituye una garantía física absoluta ante un fallo del MCU o interrupciones deshabilitadas. Un corte físico independiente sigue siendo necesario para la Peltier.

## 6. Evidencia observada

| Comprobación | Resultado |
| --- | --- |
| Build web | Compilación y TypeScript correctos; rutas `/admin`, `/auth/*` y API presentes |
| Migración | Aplicada al proyecto exclusivo; sin errores |
| GET admin sin sesión | 401 |
| Usuario autenticado no administrador | 403 en estado y en órdenes |
| RPC privado con rol `authenticated` | 42501 |
| POST sin `Origin` | 403 |
| POST con `Origin` ajeno | 403 |
| Cuerpo inválido con `Origin` correcto | 400 |
| Pulso 1500 ms | Creado, aplicado y confirmado; GPIO5 volvió a bajo |
| Pulso 3000 ms | `elapsed_us` 3.000.017 entre inicio y corte; `pump_level` 0 tras el corte |
| Orden pendiente + nueva orden | Segunda rechazada con 409 |
| Reinicio con orden pendiente | Rechazada: «Dispositivo reiniciado antes de confirmar la orden»; no se ejecutó |
| Ventiladores | Encendido y apagado aplicados; GPIO7 reportó 1 y luego 0 |
| Cambio a Automático | Aplicado y reportado en la web pública |
| Pulso estando en Automático | 409: la bomba solo admite pulsos en Manual |
| Acción Peltier | 400; el firmware no expone ruta de encendido |
| Duración 3001 ms | 400 |
| Campo extra en la orden | 400 |
| Estado final | Modo Manual, GPIO4/5/7 en bajo, Peltier bloqueada, `phase4_control_ready` verdadero |
| Errores JS en superficies | Ninguno observado en la revisión de admin y público |

### Fallo detectado y corregido durante la integración

La primera transmisión posterior a la migración mostró `invalid_response` con HTTP 200. El servidor aceptaba la muestra, pero el asignador JSON del firmware reservaba un único bloque y ArduinoJson necesitaba varios: `NoMemory` y `overflowed`. Se sustituyó por una arena acotada de 2048 bytes por respuesta, dentro del documento, con realineación y reutilización. Se añadió una prueba nativa con una respuesta sin orden y otra con comando completo; ambas pasan, y tras cargar el cambio se observaron tres respuestas `accepted: true` consecutivas.

### Límite de esta evidencia

- No se forzó el vencimiento de 15 s en hardware: el dispositivo permaneció accesible y las líneas serie no permitieron retenerlo en reset de forma fiable. La caducidad está impuesta por la base y solo se aplica a órdenes pendientes; debe seguir probándose con corte de red o alimentación en el ensayo de campo.
- Los niveles GPIO son lo que reporta el firmware; no certifican caudal, giro de ventiladores ni temperatura. No había cargas conectadas.
- La sesión del panel se validó en la pestaña privada del navegador integrado; no se probó desde un celular real.
- El modo Automático está seleccionable pero no ejecuta riego ni control térmico.

## 7. Operación

### Varias redes Wi-Fi

`wifi.local.h` admite una lista; el firmware las prueba en orden hasta que una responde:

```cpp
constexpr WifiNetwork WIFI_NETWORKS[] = {
    {"RED_HABITUAL", "CLAVE"},
    {"HOTSPOT_1", "CLAVE"},
    {"HOTSPOT_2", "CLAVE"},
};
```

- Máximo 10 segundos por intento fallido; conviene poner primero la red más usada.
- Si ninguna responde, espera entre 15 y 60 segundos y repite el ciclo.
- El log informa solo el índice de red, nunca el SSID ni la clave.
- Siguen siendo redes de 2,4 GHz con contraseña; no resuelve portales cautivos ni 802.1X.
- Se verificó con una red inexistente delante de la real: el equipo pasó a la segunda y transmitió.

Variables nuevas: `ADMIN_USER_ID`. Se mantienen `NEXT_PUBLIC_SUPABASE_URL`, `NEXT_PUBLIC_SUPABASE_ANON_KEY`, `SUPABASE_SERVICE_ROLE_KEY`, `DEVICE_ID` y `DEVICE_TOKEN_SHA256`.

La migración de fase 4 sustituye `ingest_telemetry` por `ingest_telemetry_v4`: el firmware de fase 3 deja de ser compatible, por lo que base, API y firmware deben actualizarse juntos. El despliegue sigue siendo manual por CLI; publicar en GitHub no despliega.

Para revocar acceso: eliminar o deshabilitar la cuenta en Supabase y/o cambiar `ADMIN_USER_ID`. Para revocar el dispositivo: rotar el token, actualizar `DEVICE_TOKEN_SHA256` y recompilar el firmware.

## 8. Siguiente fase

Fase 5: medir caudal real, definir dosis y horarios con el responsable del cultivo, implementar pulsos locales con hora válida y persistencia de eventos, y preparar la demanda térmica 24/22 °C sin habilitar la Peltier. La protección térmica sigue en fase 6.
