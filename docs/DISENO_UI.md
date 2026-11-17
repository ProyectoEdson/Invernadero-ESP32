# Diseño de interfaz: botánico premium

[Inicio](../README.md) · [Plan de desarrollo](PLAN.md) · [Hardware](HARDWARE.md)

## 1. Intención

Crear una experiencia visual cuidada para un invernadero real, no una página de números ni un panel genérico con botones. La interfaz debe transmitir calma, claridad y precisión, y seguir siendo útil cuando el dispositivo falla o no hay lecturas.

Este documento es una especificación, no una interfaz implementada ni una captura de un producto existente.

### Dirección aprobada

- Paleta natural: verde bosque como acento, salvia como apoyo, superficies claras cálidas y contraste suficiente.
- Tipografía de lectura clara, jerarquía editorial y cifras con ancho estable para evitar saltos durante actualizaciones.
- Espaciado generoso, composición equilibrada y densidad controlada. No convertir cada dato pequeño en una tarjeta independiente.
- Gráficas limpias, bordes suaves y sombras discretas; sin saturar de brillos, degradados o efectos que compitan con la información.
- Animación breve y funcional, nunca imprescindible para entender un estado.

La identidad inicial será Invernadero ESP32. Fotografías, logotipo, nombre comercial alternativo y recursos con licencia no están definidos; no inventar imágenes que parezcan mostrar el cultivo real.

## 2. Stack visual

Next.js/React permite componentes complejos, layouts responsive, animaciones y gráficas interactivas. Vercel aloja la aplicación; Supabase proporciona datos y acceso, sin imponer su aspecto.

Base prevista: TypeScript y Tailwind CSS, con componentes accesibles y una librería de gráficas elegida durante implementación. Recharts es una candidata, no una dependencia ya aprobada/instalada. No se instala una librería de animación por defecto si CSS basta.

Las fuentes deberán cargarse de forma que no dependan de peticiones externas continuas al visitar la web. La biblioteca UI y las fuentes concretas se cerrarán antes de su incorporación. No sacrificar accesibilidad por personalización visual.

## 3. Superficie pública

### Jerarquía del dashboard

1. **Cabecera:** identidad, enlace de acceso administrador y estado de actualización.
2. **Resumen principal:** temperatura y humedad ambiental con unidades, calidad y hora de lectura.
3. **Evolución reciente:** gráfica de lecturas auténticas; series y ejes con unidades explícitas, sin mezclar °C y porcentaje en una escala engañosa.
4. **Estado del sistema:** modo aplicado y canales reportados, con avisos persistentes relevantes.
5. **Contexto:** umbrales configurados y explicación de lo que realmente mide cada sensor.

La temperatura tendrá mayor jerarquía por su relación con el enfriamiento. La humedad ambiental no llevará etiquetas de suelo húmedo/seco.

### Gráficas honestas

- Mostrar únicamente muestras recibidas y válidas. No inventar historial para llenar la pantalla.
- Al abrir una sesión sin puntos suficientes, explicar que la curva se construye con nuevas lecturas.
- Si solo está disponible la última lectura, mostrarla sin una curva ficticia.
- No unir como si fueran continuos periodos sin datos; hacer visibles las interrupciones.
- Las líneas de referencia de 22 y 24 °C se identificarán como umbrales iniciales del automático, no como certificación agronómica.
- El histórico entre sesiones queda pendiente de política de almacenamiento/retención; no mostrar selectores de periodos que no se puedan consultar.

### Estado público de actuadores

Mostrará la salida reportada por el ESP32, no la confirmación física de funcionamiento. Sin sensor de flujo o realimentación, activar GPIO5 no demuestra que haya agua circulando.

Los canales sin hardware instalado o bloqueados tendrán estados explícitos, no un simple apagado verde. La Peltier mostrará «Bloqueada: protección térmica pendiente» mientras corresponda.

## 4. Superficie administradora

Ubicación propuesta: `/admin`, con inicio de sesión y verificación del permiso en servidor.

- La vista pública sigue disponible sin cuenta.
- Una sesión válida sin autorización administradora no muestra ni habilita control.
- El selector Automático/Manual explica el efecto antes de confirmar un cambio.
- En Manual, la bomba ofrece una acción de pulso limitado; no un interruptor que permita dejarla encendida indefinidamente.
- Los ventiladores se representan como un canal conjunto si comparten Q3.
- La Peltier permanece bloqueada tanto visualmente como en backend y firmware hasta completar su habilitación segura.
- La configuración muestra diferencias pendientes y versión aplicada. Guardar en la nube no equivale a aplicar en el dispositivo.

### Ciclo de una orden

Usar estados legibles: «Enviando», «Pendiente del dispositivo», «Aplicada», «Rechazada», «Vencida» o «Sin confirmación».

El botón no cambia el estado reportado de forma optimista. Tras pulsar, se muestra progreso; el estado real cambia con la confirmación. Impedir doble envío y explicar si ya existe una operación en curso.

La UI puede prevenir errores, pero no es la barrera de seguridad: backend y firmware validan las mismas restricciones relevantes.

## 5. Estados que deben diseñarse

| Estado | Comportamiento visual y funcional |
| --- | --- |
| Cargando | Esqueleto estable, sin números de demostración |
| Dispositivo nunca conectado | Explicación y ausencia de lecturas; no presentar ceros como medición |
| Lecturas válidas | Valores, unidades y fecha; actualización discreta |
| Sensor con fallo | Identificar variable inválida; conservar última lectura solo si se etiqueta como antigua |
| Wi-Fi/Internet perdido | Aviso persistente de datos desactualizados; no mostrar conexión como activa |
| Suscripción Realtime interrumpida | Aviso y recuperación de estado; no confundir conexión del navegador con la del ESP32 |
| Orden pendiente | Progreso y prevención de solicitudes duplicadas |
| Orden rechazada o vencida | Motivo visible y estado reportado sin falsos cambios |
| Sesión vencida | Pedir acceso de nuevo; no sugerir que una orden se aplicó |
| Bloqueo térmico | Mensaje persistente con motivo; no depender de una notificación efímera |
| Sin puntos para gráfica | Mensaje útil; no datos inventados |

## 6. Responsive y accesibilidad

- Diseñar primero para celular; las métricas principales deben verse sin buscar controles entre paneles.
- En escritorio, aprovechar columnas y ancho para gráficas sin ampliar innecesariamente cada tarjeta.
- Verificar desde 360 px de ancho, además de tablet y escritorio; evitar scroll horizontal global.
- Áreas táctiles cómodas, objetivo mínimo de 44 × 44 px para controles principales.
- Etiquetas explícitas y navegación por teclado con foco visible.
- Contraste objetivo WCAG AA; estados diferenciados por texto/icono además del color.
- Respetar reducción de movimiento. Las actualizaciones periódicas no deben mover el foco ni anunciar cada lectura de forma invasiva al lector de pantalla.
- Fechas y horas comprensibles, zona horaria explícita donde afecte horarios de riego.
- Unidades constantes: °C, % de humedad relativa, y ml/s o ml según configuración calibrada.

## 7. Criterios de acabado

La interfaz se considerará terminada cuando:

- Haya una identidad visual coherente, no componentes sueltos sin jerarquía.
- Dashboard y administración funcionen en celular y escritorio.
- Los estados de error, bloqueo y desconexión tengan el mismo nivel de acabado que el estado normal.
- Las gráficas correspondan a datos reales y los controles a operaciones implementadas.
- Una acción crítica no dependa únicamente de un color, hover o animación.
- No aparezcan botones, periodos de histórico o controles de potencia que el sistema no pueda cumplir.
- Se comprueben las pantallas reales en navegador; una compilación correcta no equivale a verificación visual.

## 8. Orden de diseño e implementación

1. Aprobar composición del dashboard y administración mediante una propuesta visual antes de implementar el frontend completo.
2. Definir tokens compartidos de color, tipografía, espaciado, estados y foco.
3. Construir componentes base y layout responsive con estados vacíos honestos.
4. Conectar telemetría real y gráfica reciente.
5. Integrar sesión, órdenes y bloqueos.
6. Verificar la superficie real en móvil/escritorio y corregir problemas observados.

Se pueden usar datos de prueba identificados durante desarrollo, pero nunca presentarlos como evidencia de funcionamiento del ESP32 ni dejarlos como fallback ficticio en producción.
