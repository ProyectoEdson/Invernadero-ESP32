# Fases 1 y 2: verificación y firmware base

[Inicio](../README.md) · [Plan](PLAN.md) · [Hardware](HARDWARE.md)

## Alcance y estado

La revisión del esquema y la preparación del firmware pueden hacerse desde la estación. La revisión eléctrica de la PCB no puede certificarse por software.

El responsable confirmó para la prueba: alimentación solo por USB, batería/alimentación externa y cargas desconectadas, DHT11 conectado a 3,3 V, GND y GPIO15. También autorizó sobrescribir el firmware actual. Esta confirmación no sustituye mediciones físicas.

## Fase 1: registro de revisión

| Comprobación | Resultado / evidencia |
| --- | --- |
| Mapa del esquema | J1 DATA GPIO15; J2 DATA GPIO6; Q1/R6 GPIO4; Q2/R4 GPIO5; Q3/R3 GPIO7 |
| Herramientas de desarrollo | Arduino CLI 1.6.0, core 3.3.13, DHT 1.4.7 y Unified Sensor 1.1.15 disponibles |
| Puerto de desarrollo | Puente USB serie detectado en `/dev/ttyACM0` |
| Montaje aislado para probar | Confirmado por el responsable; no inspeccionado físicamente por el asistente |
| Continuidad y ausencia de cortocircuitos | Pendiente de medición física |
| Correspondencia símbolo/huella/placa ensamblada | Pendiente de contraste físico |
| Salida real de U2 a 5 V, sin ESP32 conectado | Pendiente de medición física |
| Realimentación entre USB y entrada 5 V | Pendiente de revisión física |
| Excitación y temperatura de MOSFETs bajo carga | Pendiente de medición/revisión eléctrica |
| Fusibles, cables, borneras y batería | Pendiente de revisión física |
| Refrigeración y protección térmica | No instaladas/validadas; mantener Peltier bloqueada |

**Fase 1 no completada:** no conectar batería ni actuadores basándose en una compilación, un log serie o una conexión Wi-Fi.

Para registrar mediciones, indicar instrumento, puntos de medida, valor, alimentación usada y responsable. No medir resistencia/continuidad en un circuito energizado. La selección de fusibles y ensayos de potencia requiere revisar el montaje con una persona competente en electrónica.

## Fase 2: firmware implementado

Ubicación: `firmware/invernadero/`.

- DHT11 en GPIO15, muestreo cada 3 segundos.
- Valores no finitos o fuera del rango admitido se informan como fallo con `null`, nunca como cero o datos inventados.
- GPIO4, GPIO5 y GPIO7 con latch LOW antes de habilitar su salida; entrada habilitada para consultar nivel del pad.
- Sin mandos de encendido: los tres canales permanecen apagados en esta fase. Q1 queda explícitamente bloqueada.
- Comprobación de flash de 16 MB y PSRAM de 8 MB durante arranque. Una discrepancia se publica como fallo, no se oculta.
- Identidad estable del dispositivo, identificador aleatorio de arranque y secuencia por muestra. La identidad no es una credencial de autenticación.
- Wi-Fi STA de 2,4 GHz, asociación sin bucle de espera y reintentos espaciados hasta 60 segundos; muestreo separado del estado de conexión.
- Configuración Wi-Fi local, no guardada por este firmware en NVS y excluida de Git.
- Salida JSON por UART a 115200 para observar arranque, Wi-Fi y muestras.

El código de aplicación evita un bucle que espere a la red. Las llamadas al driver Wi-Fi y al DHT tienen su propio tiempo de ejecución: esto no es una garantía de tiempo real duro. No hay HTTPS, servidor web, modos de actuación ni riego automático todavía; corresponden a fases posteriores.

## Configuración privada

1. Copiar `firmware/invernadero/wifi.example.h` como `firmware/invernadero/wifi.local.h`.
2. Completar `WIFI_SSID` y `WIFI_PASSWORD` localmente; usar una red que admita 2,4 GHz.
3. No publicar ese archivo ni binarios compilados: las credenciales quedan dentro del firmware aunque el archivo esté excluido de Git.

En esta estación se autorizó obtener la configuración de la conexión activa de NetworkManager sin imprimir ni publicar la clave. Otro colaborador debe crear su propia configuración.

## Compilar de forma reproducible

Con Arduino CLI instalado, desde la raíz del repositorio:

```bash
arduino-cli compile --profile esp32s3 --build-path /tmp/invernadero-esp32-build firmware/invernadero
```

El perfil de `sketch.yaml` fija core y librerías. Arduino CLI puede descargarlos en su caché aislada aunque ya existan instalaciones globales; no agrega una dependencia distinta a las declaradas.

Perfil: ESP32-S3, flash 16 MB, partición `app3M_fat9M_16MB`, PSRAM OPI, consola por puente UART USB, sin borrado total de flash. El modo OPI debe comprobarse mediante la PSRAM detectada al ejecutar, no solo porque compile.

La configuración local debe existir y contener SSID no vacío; de lo contrario la compilación falla con una instrucción explícita.

## Cargar y observar

Solo con autorización para sobrescribir y con el montaje aislado descrito arriba:

```bash
arduino-cli upload --profile esp32s3 --port /dev/ttyACM0 --input-dir /tmp/invernadero-esp32-build firmware/invernadero
arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
```

El puerto puede cambiar en otro equipo. No ejecutar un borrado completo para resolver un fallo de carga sin revisar primero su causa.

La primera línea JSON de tipo `boot` informa flash/PSRAM y el bloqueo. Las líneas `sample` incluyen estado del sensor, valores o `null`, secuencia, conexión Wi-Fi y niveles de GPIO. Los mensajes de arranque del chip previos al JSON son normales.

`outputs_safe: true` significa que los pads leídos están en LOW en esa muestra. No certifica ausencia de transitorios antes de `setup`, continuidad de la PCB ni estado físico de actuadores. Los pull-down y la revisión eléctrica siguen siendo necesarios.

## Evidencia de ejecución

Se compiló con el perfil `esp32s3` y se cargó mediante Arduino CLI. El cargador verificó los hashes escritos y reinició el chip.

| Comprobación | Resultado observado |
| --- | --- |
| Tamaño de programa | 880.277 bytes, 27 % de la partición de aplicación |
| Variables globales | 45.668 bytes, 13 % de la RAM interna informada por el compilador |
| Carga USB | Correcta, con verificación de datos por esptool |
| Muestras observadas | 11, secuencias 1 a 11 |
| Intervalo entre muestras | 3.005 ms en los diez intervalos observados |
| Wi-Fi | Conectado en las 11 muestras; se recibió evento de conexión con IP local |
| Configuración de memoria | `memory_config_ok: true` en las 11 muestras; comparación contra flash 16 MB y PSRAM 8 MB |
| Niveles de GPIO4/5/7 | LOW en las 11 muestras; no es una medición de potencia ni de transitorios de arranque |
| Bloqueo Peltier | Informado como activo |
| DHT11 | 11 `read_error`; temperatura y humedad `null`; ninguna lectura válida |

La primera prueba no fue aceptada por los errores DHT11. Se recibió una foto del montaje en protoboard, con el conector tapando IO15/IO16 y etiquetas del sensor no legibles; se solicitó revisar ambos extremos sin alimentación. No se cambió el firmware a otro GPIO.

### Comprobación después de revisar el cableado

El responsable confirmó DATA/S en GPIO15, VCC en 3,3 V y GND común, con solo USB conectado, y autorizó repetir la lectura. Se observó el mismo firmware sin recompilar ni cambiar el pin.

| Comprobación | Resultado observado |
| --- | --- |
| Muestras consecutivas | 6, secuencias 1 a 6 |
| Estado del sensor | `ok` en las seis muestras |
| Temperatura | 27,1 °C en las seis muestras |
| Humedad ambiental | Entre 31 y 33 % |
| Wi-Fi y memoria | `wifi_connected: true` y `memory_config_ok: true` en las seis muestras |
| GPIO4/5/7 | LOW y `outputs_safe: true` en las seis muestras |
| Peltier | Bloqueo activo; ninguna carga energizada para esta prueba |

**Firmware base de Fase 2 validado por USB.** Las lecturas son evidencia de comunicación con el sensor, no una calibración contra un instrumento patrón. No se afirma cuál era el defecto físico original del cableado: solo que la prueba posterior a la revisión ya entregó lecturas válidas.

No se comprobaron todavía recuperación tras pérdida de Wi-Fi, funcionamiento con batería, transitorios de arranque ni actuación física. La prueba sí observó la representación explícita de fallo de sensor, sin datos ficticios. Las credenciales y los binarios compilados se mantienen locales.

## Límites pendientes

- Las mediciones eléctricas de Fase 1 requieren intervención física.
- No habilitar las cargas con este registro como única evidencia.
- El perfil fija la capacidad esperada de memoria, no valida la PCB.
- La prueba por USB no sustituye la evaluación con alimentación independiente.
- No hay datos enviados a Vercel/Supabase en Fase 2; solo conexión Wi-Fi y salida serie.
