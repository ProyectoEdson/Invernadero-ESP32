# Entrega: configuración Wi‑Fi del Invernadero ESP32-S3

Este documento y la carpeta `firmware/invernadero/` son la entrega para quien tiene la placa. La tarea de su agente es **integrar este código fuente, compilarlo para la placa existente y hacer las pruebas de aceptación**. No copiar solo el `.ino`, crear otro sketch ni reemplazar los ajustes privados del compañero.

## Antes de cargar el firmware

- CruChi ya tiene publicado el endpoint que verifica la red nueva. El portal local no necesita Vercel. La pantalla privada para cambiar el Wi‑Fi a distancia desde CruChi todavía está pendiente.
- Usa la [asignación de pines de la PCB actual](HARDWARE.md#3-mapa-de-señales), no el firmware anterior. Confirma el ESP32‑S3, su flash de 16 MB y PSRAM OPI si usas el perfil `esp32s3`; detecta el puerto con `arduino-cli board list`. Antes de cargar, revisa GPIO45 y desconecta físicamente las cargas de 12 V. Q1, Q2 y Q3 deben permanecer en LOW.
- Mantener `wifi.local.h` y `cloud.local.h` fuera de cualquier paquete o commit. Si faltan, partir de `wifi.example.h` y `cloud.example.h`. En `wifi.local.h`, conservar al menos una red conocida de 2,4 GHz como respaldo. No pegar ninguna clave en el chat del agente, la consola serie ni el repositorio.

## Configuración privada de la placa

En `firmware/invernadero/cloud.local.h`:

| Configuración | Qué poner |
| --- | --- |
| `CLOUD_DEVICE_TOKEN` | Token de telemetría existente para esta placa. No reutilizarlo como contraseña del punto de acceso ni como clave del panel. |
| `WIFI_SETUP_PASSWORD` | Contraseña única de esta placa para `Invernadero-<últimos 6 dígitos hexadecimales del ID>`. Usa entre 12 y 63 caracteres ASCII imprimibles, preferiblemente 16 o más. Si queda vacía no se abrirá la red de rescate. |
| `WIFI_REMOTE_KEY` | Opcional para la prueba local. Si ya existe, consérvala. Para habilitar la gestión remota, la futura pantalla privada de CruChi deberá usar esta misma clave de 32 bytes representada como 64 dígitos hexadecimales en minúscula. No la subas al repositorio. |

No compartir el binario compilado: incluye credenciales. Los archivos de ejemplo no contienen valores privados.

Desde la raíz del repo, o desde la raíz del paquete si solo contiene el firmware:

```sh
arduino-cli board list
arduino-cli compile --profile esp32s3 firmware/invernadero
arduino-cli upload --profile esp32s3 --port PUERTO_DE_LA_PLACA firmware/invernadero
arduino-cli monitor --port PUERTO_DE_LA_PLACA --config baudrate=115200
```

La carga por USB debe hacerla quien tiene la placa, después de confirmar el puerto y coordinar el cambio. No usar `erase-flash` para una actualización normal.

## Qué comprobar con la placa

1. **Inicio normal:** con la red conocida disponible y las cargas desconectadas, confirmar telemetría nueva en [CruChi](https://proyecto16.cruchi.com/). En el monitor serie a 115200 comprobar temperatura, humedad, lux y GPIO de Q1, Q2 y Q3 en `0`.
2. **BH1750:** cubrir y descubrir el sensor; comprobar que cambien los lux en serie y en el dashboard. Con la placa apagada, desconectar el BH1750, volver a arrancar y confirmar que indica ausencia de lectura de luz sin ocultar temperatura y humedad. Una lectura de `0 lx` sí es válida.
3. **Credencial incorrecta:** volver a probar con una contraseña incorrecta o una red sin acceso a CruChi. Confirmar que el cambio falla, que se recupera la red anterior y que vuelven las lecturas.
4. **Local sin Internet:** apagar temporalmente las redes conocidas de la placa, esperar a que se agoten los intentos y conectar el celular a la red de configuración del ESP32 con `WIFI_SETUP_PASSWORD`. Si el celular advierte que esa red no tiene Internet, mantener la conexión y abrir `http://192.168.173.1/`. Buscar redes, elegir una de 2,4 GHz y probarla. No usar las credenciales del curso ni de Vercel aquí.
5. **Persistencia:** reiniciar la placa tras guardar una red válida y comprobar que la usa sin introducir la contraseña de nuevo. La red compilada conocida sigue disponible como respaldo. El control de bomba, ventiladores y Peltier no forma parte de la prueba de configuración.

Si una red conocida se asocia pero no tiene salida a CruChi, el punto de acceso de rescate también se abre tras dos minutos sin respuesta del servicio. Una red que requiera aceptar una página de acceso (portal cautivo) no supera la prueba HTTPS del ESP32.

En la entrega, anotar únicamente resultado, fecha, modelo y síntoma si falla. No adjuntar contraseñas, tokens, cabeceras HTTP, archivos `.local.h` ni imágenes de la flash.

## Coordinación del lado servidor

- **CruChi:** publicar `admin-wifi.php`, `device-wifi.php`, `wifi-common.php` y la actualización de `.htaccess` desde `cruchi/public/api/` en `/invernadero/api/`. En `.invernadero-cruchi.php`, fuera de la raíz pública, configurar `wifi_admin_token_sha256` con SHA‑256 de un token administrativo nuevo. Usar un token de 32 bytes aleatorios representado como 64 caracteres hexadecimales en minúscula. No cambiar ni reutilizar `device_token_sha256`. El proceso PHP debe poder crear el buzón privado `.invernadero-wifi-*` junto a esa configuración, no dentro de la carpeta pública.
- **Gestión remota:** no desplegar ni configurar Vercel para esta prueba. El puente PHP ya existe, pero para que lo opere una persona falta implementar y publicar la pantalla privada en CruChi.
- El canal Wi‑Fi no guarda contraseñas en MySQL. CruChi solo recibe el mensaje cifrado; una orden caducada no se entrega y su contenido se borra del buzón en el siguiente acceso autenticado que procese la cola.
