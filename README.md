# Invernadero ESP32

Invernadero automatizado para lechuga, con un ESP32-S3 conectado por Wi-Fi a una plataforma web pública y controles privados para el administrador.

## Estado actual

**Fase 3 implementada y desplegada:** ESP32 → HTTPS → Supabase → dashboard público, con mediciones reales y actualización sin recargar. Se verificaron lectura pública, rechazo de escritura, deduplicación, escritorio/móvil y recuperación tras detener el ESP32. La fase 1 sigue pendiente de mediciones físicas; no hay controles de actuadores.

**Web pública:** <https://invernadero-esp32-delta.vercel.app>

Se ha revisado el esquemático compartido y se han identificado las señales previstas. La PCB física, las protecciones y la capacidad eléctrica del montaje siguen pendientes de validación.

**La Peltier debe permanecer deshabilitada hasta instalar y verificar disipación, ventilación y protección térmica.** El modo Manual no podrá saltarse ese bloqueo.

## Documentación

| Documento | Contenido |
| --- | --- |
| [Plan de desarrollo](docs/PLAN.md) | Arquitectura, seguridad, modos, fases detalladas, entregables, pruebas y decisiones pendientes |
| [Hardware y seguridad](docs/HARDWARE.md) | GPIO, bomba, alimentación con batería, MOSFETs, calibración y protección térmica |
| [Diseño de interfaz](docs/DISENO_UI.md) | Dirección botánica premium, dashboard, administración, móvil, accesibilidad y estados reales |
| [Registro de fases 1 y 2](docs/FASES_1_2.md) | Mediciones pendientes, configuración privada, compilación, carga y evidencia real |
| [Fase 3 operativa](docs/FASE_3.md) | Enlace público, configuración, despliegue, protocolo, permisos, pruebas y límites |

## Sistema previsto

- **ESP32-S3:** medición y control local; no necesita una laptop encendida durante la evaluación.
- **DHT11:** temperatura y humedad del aire; no mide humedad del suelo.
- **Next.js en Vercel:** dashboard público y API HTTPS.
- **Supabase:** datos, actualizaciones en tiempo real y autenticación.
- **Administración:** pendiente de fase 4; todavía no hay inicio de sesión ni mandos.
- **Diseño:** interfaz botánica premium, responsive, con gráficas de datos auténticos y estados claros.

El ESP32 inicia conexiones salientes a Internet. Laptop y celular abren la web desde cualquier red; no se abrirán puertos del router ni se usará la laptop como puente obligatorio.

## Comportamiento acordado

| Función | Decisión |
| --- | --- |
| Temperatura en Automático | Solicitar enfriamiento a 24 °C y apagar a 22 °C; valores iniciales ajustables |
| Riego en Automático | Horario y dosis después de medir caudal y acordar necesidades del cultivo |
| Riego en Manual | Pulsos con límite local, no encendido indefinido |
| Humedad ambiental | Mostrar; no usarla como sustituto de humedad del suelo |
| Peltier | Bloqueada inicialmente en ambos modos |
| Fallos de Internet | Control automático local con configuración válida; web indica datos desactualizados |

Los umbrales ambientales no son los límites de protección térmica del disipador. Una orden aplicada al GPIO tampoco confirma funcionamiento físico de bomba o ventiladores sin sensores de realimentación.

## Hoja de ruta

Estado real por fase:

1. Verificación eléctrica: revisión documental realizada; mediciones físicas pendientes.
2. Firmware base: validado por USB con DHT11, Wi-Fi, memoria esperada y salidas LOW; recuperación de red y pruebas de potencia no realizadas.
3. Nube y dashboard: desplegados y verificados con datos reales del ESP32; detalle en [Fase 3](docs/FASE_3.md).
4. Incorporar administrador, modos y órdenes confirmadas.
5. Calibrar riego e implementar automatización local.
6. Incorporar protección térmica y habilitar Peltier; fase pospuesta.
7. Ensayar evaluación sin laptop, incluidos reinicios y desconexiones.

La nube y el frontend pueden desarrollarse sin alimentar las cargas. Las pruebas de potencia requieren superar la revisión eléctrica, y el enfriamiento no puede declararse operativo si la fase térmica sigue pendiente.

## Antes de implementar

- Contrastar el mapa de GPIO y orientación con la PCB ensamblada.
- Revisar excitación de los IRLZ44N a 3,3 V, alimentación, fusibles y cableado.
- Definir macetas/sustrato, medir caudal y aprobar la programación de riego.
- Configurar la cuenta administradora en fase 4; los proyectos Vercel/Supabase ya existen y sus secretos no se versionan.

La web está en [`web/`](web/) y el firmware en [`firmware/invernadero/`](firmware/invernadero/). La configuración, compilación, carga y despliegue actuales están en [Fase 3](docs/FASE_3.md). El registro de fases 1 y 2 conserva su evidencia histórica. Las credenciales locales y los binarios no se versionan.