# Hardware y seguridad

[Inicio](../README.md) · [Plan de desarrollo](PLAN.md) · [Diseño de interfaz](DISENO_UI.md)

## 1. Fuentes y grado de verificación

Esta ficha reúne el esquemático, la imagen de PCB, la fotografía de la bomba y la detección USB compartidos durante la preparación. Que la PCB se haya fabricado a partir del esquema no demuestra continuidad, orientación ni capacidad eléctrica bajo carga.

Los archivos originales de EasyEDA, las fotografías y la ficha del fabricante de la Peltier todavía no se han incorporado al repositorio. El mapa siguiente es lectura del esquema, no certificación de la placa ensamblada.

## 2. Inventario conocido

| Elemento | Dato conocido | Pendiente |
| --- | --- | --- |
| Microcontrolador | ESP32-S3, placa de 44 pines | Contrastar pinout y orientación con PCB física |
| Detección USB observada | ESP32-S3 rev. v0.2, flash 16 MB, PSRAM integrada 8 MB | Confirmar configuración de PSRAM durante arranque |
| Sensor ambiental | DHT11 | Verificar alimentación y pull-up del módulo real |
| Peltier | TEC1-12706, alimentación prevista de 12 V | Fabricante, ficha, corriente real, disipador y protección |
| Bomba | AD20P-1230A brushless, 12 V, 4,8 W, IP68 según etiqueta | Caudal instalado, consumo de arranque y modo de instalación permitido |
| Ventiladores | Previstos de 12 V | Aún no instalados; corriente y caudal no definidos |
| Alimentación | Batería de automóvil | Tensión medida, estado, protecciones y condiciones de uso |
| Regulación | Módulo LM2596 para 5 V | Ajuste y medición antes de conectar el ESP32 |
| PCB | Descrita como 99 × 83 mm, dos capas | Verificación física y eléctrica |

El símbolo del esquema identifica una variante N8R8, mientras que la detección del equipo conectado indicó 16 MB de flash. No inferir de ese nombre que la placa física tenga 8 MB: contrastar módulo, pinout y configuración antes de cargar firmware.

## 3. Mapa de señales

| Función prevista | Cadena según esquema | GPIO |
| --- | --- | --- |
| Sensor ambiental J1 | Pin 2 de J1 → U1 IO15 | 15 |
| Entrada disponible J2 | Pin 2 de J2 → U1 IO6 | 6 |
| Peltier | U1 IO4 → R6 de 220 Ω → Q1 → J5 | 4 |
| Bomba | U1 IO5 → R4 de 220 Ω → Q2 → J6 | 5 |
| Ventiladores | U1 IO7 → R3 de 220 Ω → Q3 → J7 | 7 |

En el esquema, los conectores de sensor llevan alimentación de 3,3 V y GND. Verificar número de pin y orientación física antes de enchufar un módulo; la forma o ubicación del pad no sustituye esa verificación.

**GPIO4 no es el DHT11 en esta PCB.** Ese GPIO controla Q1. Un sketch genérico que lo use para el sensor puede conmutar la salida de la Peltier. Para el DHT11 conectado a J1 se utilizará GPIO15.

J2 queda libre porque no se usará sensor de suelo. Podría emplearse para un sensor térmico futuro, previa revisión de alimentación, GPIO, pull-up y cableado.

## 4. Canales de potencia

Se prevé conmutación low-side con MOSFET N: el positivo de la carga va al positivo de potencia y el MOSFET conmuta su retorno. Las salidas son Active-HIGH; LOW solicita apagado.

El esquema incluye resistencias serie de compuerta de 220 Ω y pull-down de 10 kΩ. Ayudan a mantener el apagado durante arranque, pero no sustituyen firmware seguro, mediciones ni corte físico. Los diodos D1, D2 y D3 se identifican como 1N4007W en el esquema; confirmar el componente realmente montado.

### IRLZ44N y señal de 3,3 V

La denominación logic-level no garantiza una resistencia de conducción adecuada a 3,3 V. La ficha del IRLZ44N no ofrece una garantía de RDS(on) a esa tensión de compuerta. El voltaje de umbral tampoco significa que esté completamente encendido.

Antes de probar cargas, revisar excitación, disipación y pérdida de tensión bajo la corriente prevista. Si hace falta driver o cambiar componente, será una decisión explícita de hardware, no un arreglo de firmware.

### PWM y polaridad

- No habilitar PWM de alta frecuencia con este montaje sin revisar MOSFET, driver, diodos, carga y calentamiento.
- Para la primera implementación se plantea encendido/apagado, no regulación PWM de potencia.
- Un MOSFET low-side no invierte polaridad: no permite alternar frío/calor de la Peltier.
- La bomba es brushless y puede contener electrónica propia; no asumir que admite PWM externo ni que se comporta como un motor de escobillas simple.
- Los ventiladores conectados al mismo Q3 se controlarán juntos. No podrán tener órdenes independientes sin modificar el hardware.

## 5. Alimentación desde batería

La batería no entrega una corriente fija: cada carga demanda corriente. Sin embargo, puede suministrar una corriente de cortocircuito muy elevada.

Requisitos antes de energizar:

1. Fusible cercano al positivo de batería y protección de ramas según cables y cargas.
2. Terminales aislados, polaridad comprobada y cableado/borneras adecuados.
3. Medir tensión real. La denominación 12 V no garantiza exactamente 12 V; con cargador o vehículo en marcha puede superar el valor nominal de las cargas.
4. Ajustar y medir U2 a 5 V antes de conectar el ESP32. Nunca conectar la batería directamente a 5 V o 3,3 V del módulo.
5. Comprobar cómo evitar realimentación entre la entrada 5 V de la placa y USB antes de conectar ambas fuentes.
6. Mantener referencia GND común según esquema, revisando retornos de potencia y ruido.
7. Respetar los requisitos de ventilación y manejo del tipo real de batería; no colocarla en el recinto húmedo y cerrado del cultivo.

No se ha definido un calibre de fusible: requiere conocer corriente total, arranques, cableado y capacidad de pistas/conectores. El ancho declarado de pistas de 1,5 mm por sí solo no demuestra que soporten la corriente; importan espesor de cobre, longitud, entorno y temperatura admisible.

Las primeras pruebas se recomiendan con fuente de laboratorio limitada en corriente. No conectar al sistema eléctrico de un vehículo en marcha ni a un cargador sin revisar tensión y transitorios.

## 6. Bomba y calibración

### Datos de etiqueta

| Magnitud | Valor anunciado |
| --- | --- |
| Modelo | AD20P-1230A |
| Tensión | DC 12 V |
| Potencia | 4,8 W |
| Corriente calculada P/V | 0,4 A nominal aproximada; arranque no medido |
| Qmax | 240 L/h, equivalente a 4 L/min |
| Hmax | 300 cm |
| Protección indicada | IP68 |

Qmax y Hmax son extremos de la curva, no condiciones simultáneas. No se deben presupuestar 4 L/min a 3 m de altura ni usar el valor de etiqueta como caudal de riego real. IP68 no convierte conexiones eléctricas externas en aptas para mojarse.

### Procedimiento de calibración pendiente

1. Instalar mangueras, altura y distribución definitivas; asegurar el uso permitido por el fabricante y evitar funcionamiento en seco.
2. Medir un volumen recogido durante un tiempo conocido con la tensión real de operación.
3. Calcular caudal = volumen / tiempo; repetir para observar variación y reparto entre salidas.
4. Elegir volumen por riego según cultivo, macetas, sustrato y drenaje.
5. Calcular una duración inicial y comprobar el volumen realmente entregado en pulsos cortos. El arranque puede hacer que el cálculo lineal no sea exacto.
6. Aprobar frecuencia, máximo por pulso y máximo diario. Mantener el riego automático sin habilitar hasta completar estos datos.

Sin sensor de suelo no hay medición después de esperar infiltración: el algoritmo será temporizado y requerirá observación del cultivo.

## 7. Refrigeración: bloqueo vigente

Actualmente no hay ventilador ni sensor en la cara caliente. No se ha confirmado un montaje de disipación completo. Por decisión del responsable, su incorporación se pospone y **Q1 debe permanecer deshabilitada**.

La Peltier descarga en la cara caliente el calor extraído más la energía eléctrica consumida. Esa cara necesita un disipador adecuado y evacuar calor fuera de la caja. Una caja aislada no puede enfriarse globalmente si ambas caras descargan todo su calor dentro.

### Requisitos antes de habilitar Q1

- Disipación y ventilación dimensionadas, montaje térmico y fuente revisados.
- Sensor térmico acoplado al disipador caliente, separado del DHT11 de ambiente. DS18B20 es una propuesta todavía no comprada ni instalada; necesitaría el montaje y pull-up correspondientes.
- Corte térmico físico independiente, con capacidad de interrupción DC adecuada para la corriente de Peltier. Un fusible eléctrico normal no sustituye esta protección térmica.
- Límite por sobretemperatura, fallo de sensor y lectura desactualizada, con rearme controlado.
- Ventilación previa y posterior según el comportamiento térmico medido. Encender un GPIO de ventilación no confirma que el ventilador esté girando.
- Mantener protección frente a un MOSFET averiado en cortocircuito: el firmware no puede abrir un componente que quedó conduciendo.

No se fija todavía temperatura de corte, tiempo máximo térmico ni duración de postventilación: requieren datos del conjunto y pruebas. Los umbrales ambientales 24/22 °C no son límites de protección de la cara caliente.

Las bombas de este proyecto son de riego. No se supondrá refrigeración por agua ni se impondrá encender la bomba de riego para habilitar la Peltier.

## 8. Caja, agua y radio

- Separar agua, batería y electrónica; prever drenaje y condensación.
- Mantener zonas calientes alejadas de madera, EPS y cables susceptibles al calor.
- Confirmar material y montaje adecuados para contacto con el entorno de cultivo.
- Verificar la zona libre de cobre/componentes recomendada para la antena del módulo: la imagen de PCB sugiere revisar el área bajo el extremo de antena, pero no sustituye los archivos CAD.
- Circulación interna de aire no equivale a renovación de aire ni garantiza control de humedad. Estos límites deben reconocerse al observar el cultivo en una caja sellada.

## 9. Herramientas observadas en la estación de desarrollo

| Herramienta | Versión observada |
| --- | --- |
| Arduino CLI | 1.6.0 |
| Core esp32:esp32 | 3.3.13 |
| DHT sensor library | 1.4.7 |
| Adafruit Unified Sensor | 1.1.15 |
| ArduinoJson | 7.4.3 |
| esptool incluido en el core | 5.3.1 |

Se detectó el chip por USB y se compiló un sketch temporal: 309.962 bytes con la partición utilizada en esa prueba. Eso no verifica ejecución ni capacidad térmica. No se cargó ese sketch y ya se eliminó. No reutilizar su asignación antigua de GPIO4 para DHT11 con esta PCB.

El firmware del repositorio ya se compiló y cargó con el perfil de 16 MB y PSRAM OPI. La primera prueba mostró fallo DHT11; tras revisar el cableado, seis muestras consecutivas fueron válidas, con 27,1 °C y 31–33 % de humedad. En esas muestras, `memory_config_ok: true` confirmó la comparación con flash 16 MB y PSRAM 8 MB; Wi-Fi conectado y pads de salida LOW. Esto valida el firmware base por USB, no la etapa de potencia. Instrucciones y evidencia en [Fases 1 y 2](FASES_1_2.md). Las herramientas de una estación no configuran automáticamente las de otro colaborador.

## 10. Referencia del MOSFET

- [Infineon: ficha técnica IRLZ44N](https://www.infineon.com/assets/row/public/documents/24/49/infineon-irlz44n-datasheet-en.pdf)
