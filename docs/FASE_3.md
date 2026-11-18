# Fase 3: dashboard público y telemetría HTTPS

[Inicio](../README.md) · [Plan](PLAN.md) · [Hardware](HARDWARE.md) · [Diseño](DISENO_UI.md)

## 1. Estado y enlace

**Implementada y desplegada.** Dashboard público: <https://invernadero-esp32-delta.vercel.app>.

El ESP32 envía muestras reales por Wi-Fi y HTTPS; la API las valida, Supabase conserva el último estado y Realtime actualiza el navegador. No hay backend en la laptop. En las pruebas el ESP32 recibió alimentación por USB; todavía falta ensayar alimentación independiente con la laptop apagada.

Esta fase es de **monitorización**, no de actuación: no hay login administrador, mandos, riego automático ni Peltier operativa. Los tres GPIO de potencia permanecen LOW. La revisión eléctrica de fase 1 y la protección térmica de fase 6 siguen pendientes.

## 2. Componentes implementados

| Ruta | Responsabilidad |
| --- | --- |
| `web/app/` | Página pública, layout, estilos y API `/api/telemetry` |
| `web/components/` | Invernadero visual, gráfica de sesión, suscripción y estados de error |
| `web/lib/types.ts` | Contrato público, cadencia y umbrales de presentación |
| `web/lib/telemetry.ts` | Validación del payload del dispositivo |
| `web/lib/supabase-server.ts` | Cliente privilegiado exclusivo del servidor, con timeout |
| `web/vercel.json` | Framework Next.js explícito para despliegue |
| `supabase/migrations/202611170001_telemetry.sql` | Tablas, función transaccional, permisos y publicación Realtime |
| `firmware/invernadero/invernadero.ino` | Muestreo, Wi-Fi, salidas seguras y worker HTTPS |
| `firmware/invernadero/cloud.example.h` | Formato de configuración local de destino, token y CA |

### Versiones fijadas

Next.js 16.4.6, React/React DOM 19.3.1, TypeScript 5.9.3, Tailwind 4.3.5, Supabase JS 2.119.0, Recharts 3.11.0 y Manrope variable 5.3.0. `web/package-lock.json` fija las dependencias transitivas. Node requerido: 22.x.

El firmware mantiene el perfil de core ESP32 3.3.13, DHT 1.4.7 y Unified Sensor 1.1.15. HTTPS, SNTP y FreeRTOS proceden del core; no se añadió una librería externa de red.

## 3. Recursos y permisos

Se crearon recursos independientes para este proyecto:

- Vercel: proyecto `invernadero-esp32`, desplegado desde `web/`.
- Supabase: proyecto `Invernadero-ESP32`, Free, región `us-west-2` y compute Nano según el panel al crearlo.
- No se contrataron planes de pago. Los límites y la posible pausa por inactividad del plan gratuito deben revisarse antes de la evaluación.

Al crear Supabase se desactivó la exposición automática de tablas y se activó RLS automática. La migración concede al público únicamente SELECT sobre `public.device_state`; no concede INSERT, UPDATE, DELETE ni ejecución del RPC de ingreso.

`private.ingest_cursor` y `private.known_boots` no son datos públicos. Evitan repetir secuencias o volver a un arranque ya retirado. `public.ingest_telemetry` solo puede ejecutarse como `service_role` y actualiza el estado dentro de una transacción con bloqueo por dispositivo.

La migración se aplicó una vez mediante la API de gestión de Supabase. No se ejecutó `supabase db push` ni se instaló Supabase CLI. El archivo no es un script de reinicio: no volver a ejecutarlo sobre el mismo esquema. Si se adopta Supabase CLI posteriormente, registrar esta versión como ya aplicada antes del primer push para evitar intentar crear las mismas tablas otra vez.

## 4. Configuración privada

### Aplicación web

Copiar `web/.env.example` a `web/.env.local` y completar solo en el equipo de desarrollo:

| Variable | Uso |
| --- | --- |
| `NEXT_PUBLIC_SUPABASE_URL` | URL del proyecto; pública |
| `NEXT_PUBLIC_SUPABASE_ANON_KEY` | Clave pública del cliente, limitada por permisos/RLS |
| `SUPABASE_SERVICE_ROLE_KEY` | Secreto exclusivo de la API; nunca enviarlo al navegador o al ESP32 |
| `DEVICE_ID` | Identidad del ESP32 autorizado; no es una contraseña |
| `DEVICE_TOKEN_SHA256` | SHA-256 hexadecimal del token de 32 bytes del dispositivo |

El token del dispositivo es un valor aleatorio de 64 caracteres hexadecimales. No reutilizar la clave de servicio de Supabase como token. En esta estación también se conserva localmente la contraseña generada para la base; la aplicación no necesita publicarla ni recibirla desde el navegador.

En Vercel se configuraron las dos variables `NEXT_PUBLIC_` como Config y las demás como Secret, en Production. Las variables públicas deben existir **durante la compilación**; cambiarlas requiere un nuevo despliegue. La CLI usada admite `--environment production --type config` o `--type secret`; no asumir que un código de salida cero confirma todas las preguntas interactivas.

### ESP32

Mantener `wifi.local.h` con SSID/contraseña y crear `cloud.local.h` a partir de `cloud.example.h`:

- `CLOUD_API_URL`: URL completa HTTPS terminada en `/api/telemetry`.
- `CLOUD_DEVICE_TOKEN`: token original cuyo hash está en Vercel.
- `CLOUD_ROOT_CA`: certificado raíz PEM que valida la cadena del hostname.

Los archivos locales están ignorados por Git. Los binarios contienen las credenciales compiladas y tampoco deben publicarse.

Se obtuvo la CA desde la cadena **verificada** del dominio público usando el almacén de confianza del sistema. El emisor observado del servidor fue Google Trust Services WR1. No confiar en un certificado recibido sin verificar, ni usar `setInsecure`. El ESP32 necesita hora SNTP válida para validar la vigencia del certificado.

Si Vercel cambia a una cadena cuya raíz no está en `CLOUD_ROOT_CA`, habrá que actualizar la confianza y recompilar/cargar el firmware. No resolverlo desactivando TLS. Si se sospecha exposición del token, generar otro, actualizar su hash y volver a configurar el dispositivo.

## 5. Desarrollo, despliegue y carga

Desde la raíz del repositorio, configurar antes los secretos anteriores. Para la web:

```bash
cd web
npm ci
npm run dev
```

Comprobación de producción:

```bash
npm run build
npm run start
```

La aplicación se desplegó mediante Vercel CLI, desde `web/`:

```bash
vercel link --yes --project invernadero-esp32 --scope edsons-projects-acb923d7
vercel deploy --prod --yes
```

La cuenta y el proyecto deben ser accesibles al operador. El despliegue actual es por CLI; no se configuró despliegue automático por cada push a GitHub. No sustituir Supabase por una base temporal para mostrar una web aparentemente funcional.

Para firmware, desde la raíz y solo con USB y cargas desconectadas:

```bash
arduino-cli compile --profile esp32s3 --build-path /tmp/invernadero-esp32-build firmware/invernadero
arduino-cli upload --profile esp32s3 --port /dev/ttyACM0 --input-dir /tmp/invernadero-esp32-build firmware/invernadero
arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
```

Elegir el puerto real en otro equipo. Cargar requiere permiso para sobrescribir. La caché de compilación y configuración privada usada en esta estación tiene permisos restringidos; no compartirla como artefacto público.

## 6. Protocolo y comportamiento

### Ingreso

`POST /api/telemetry`, `Content-Type: application/json`, `Authorization: Bearer` con el token del dispositivo. La API compara su hash en tiempo constante y permite únicamente la identidad configurada.

El cuerpo incluye la muestra real de fase 2 y `sent_uptime_ms`. La API limita el cuerpo a 4096 bytes, valida tipos/rangos y exige que las salidas sigan LOW y la Peltier bloqueada. Los fallos del sensor se transmiten con valores `null`, no como una temperatura cero.

La identidad de arranque, secuencia y tiempos monotónicos permanecen privados. La fila pública tiene un ID fijo `invernadero`, identificador de muestra, recepción, edad de cola, mediciones, calidad y estado reportado. Una muestra duplicada o fuera de orden no refresca su fecha.

| Respuesta | Significado |
| --- | --- |
| 200 y `accepted: true` | Muestra aceptada y estado actualizado |
| 200 y `accepted: false` | Duplicada/fuera de orden; estado no reemplazado |
| 400 | Payload o valores inválidos |
| 401 | Credencial ausente o incorrecta |
| 413 | Cuerpo excesivo |
| 415 | Tipo de contenido incorrecto |
| 503 | Servicio sin configurar o datos no disponibles |

### Firmware

La lectura y la salida serie permanecen en el loop; TLS/HTTP se ejecutan en un worker FreeRTOS. Una cola de una muestra conserva la más reciente y evita acumular un histórico offline. Se rechazan envíos con 15 segundos o más de antigüedad local.

El worker espera Wi-Fi y hora válida, valida CA/hostname y no sigue redirecciones que puedan filtrar el Bearer. Conexión, handshake y lectura tienen límites de 5 segundos por operación; esto no equivale a un deadline global de todo el POST. Los reintentos se espacian entre 15 y 60 segundos.

Los logs distinguen espera de red/hora, error de transporte, error HTTP y aceptación confirmada. Un HTTP 200 sin respuesta válida no se presenta como muestra aceptada.

### Navegador

Consulta la última fila al abrir y al recuperar conexión/visibilidad; escucha cambios con Supabase Realtime. La gráfica conserva hasta 1200 muestras de esta visita en memoria, sin rellenar periodos anteriores ni unir huecos de lectura.

A partir de 15 segundos de edad estimada se marca la lectura como desactualizada. La edad suma el tiempo desde recepción y el tiempo que la muestra llevaba en cola en el ESP32; no mide exactamente la latencia de Internet. Se distinguen fallo del sensor, consulta, Realtime y antigüedad del dispositivo.

Las salidas son niveles reportados de GPIO, no prueba de caudal o giro de ventiladores. La interfaz no ofrece controles falsos: administración y actuaciones pertenecen a fase 4.

## 7. Evidencia observada

| Comprobación | Resultado |
| --- | --- |
| Build web local | Compilación de producción y TypeScript correctos |
| Build y despliegue Vercel | Next.js desplegado; dominio de producción accesible sin sesión |
| Lectura anónima Supabase | HTTP 200; inicialmente vacío, después fila real del dispositivo |
| Escritura anónima | HTTP 401, código PostgreSQL `42501` |
| RPC privilegiado desde cliente público | HTTP 401, código `42501` |
| Credencial incorrecta en API | HTTP 401 |
| Temperatura inválida | HTTP 400 |
| Cuerpo de 4097 bytes | HTTP 413 |
| Reenvío de una muestra real antigua | HTTP 200, `accepted: false` |
| Compilación ESP32 | 1.025.621 bytes de programa; 47.612 bytes de variables globales |
| Carga ESP32 | Escritura y hashes verificados por esptool |
| Ventana inicial observada | 8 muestras válidas y 7 confirmaciones HTTPS 200 aceptadas durante esa ventana |
| Valores iniciales de esa ventana | 28,9 °C y 33 % de humedad; no son valores fijados ni garantizados |
| Salidas en esa ventana | GPIO4/5/7 LOW en todas las muestras |
| Actualización sin recargar | Contador pasó de 20 a 21 muestras mediante Realtime |
| Escritorio | 1440 px, sin desbordamiento horizontal; captura revisada |
| Móvil | 360 px, sin desbordamiento horizontal; captura revisada |
| Ayuda accesible | Foco muestra tooltip; Escape lo oculta; controles táctiles 44 × 44 px |
| Tabla de lecturas | Se expande y muestra datos recibidos |
| Errores JS observados | Ninguno en escritorio y móvil durante las comprobaciones |
| Interrupción del ESP32 | Manteniéndolo en reset, la web indicó lectura desactualizada a partir del umbral |
| Recuperación | Al liberar reset, nuevas lecturas llegaron a la web sin recargar |

La interrupción ensayada fue detener el ESP32 mediante reset, no apagar el router ni simular una caída de Wi-Fi con datos inventados. El dispositivo volvió a enviar muestras después de esa prueba.

Se usaron las skills `frontend-design` y `webapp-testing` como guía. La verificación visual se hizo con el navegador automatizado disponible; no se instaló Playwright Python, ausente en la estación. No se sustituyó la prueba de navegador por una mera compilación.

## 8. Límites y siguiente fase

- No se ha verificado físicamente la etapa de potencia ni se han conectado actuadores para esta fase.
- Falta prueba de campo desde un celular real en otra red y con alimentación independiente del ESP32. El acceso sin autenticación sí se comprobó sobre el dominio público.
- La prueba de reset no cubre pérdida prolongada de Wi-Fi, bloqueo de NTP ni todas las formas de fallo TLS; esas pruebas de campo siguen en la evaluación final.
- No hay histórico persistente de todas las muestras: solo último estado y ventana en memoria del navegador.
- No hay administrador ni control remoto todavía. La siguiente fase añadirá autenticación y órdenes confirmadas, sin anular el bloqueo térmico.
- Los servicios Free tienen cuotas y pueden tener condiciones de inactividad. No se promete disponibilidad o consumo gratuito ilimitados.
