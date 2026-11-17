# Invernadero ESP32

Invernadero automatizado para lechuga, con un ESP32-S3 conectado por Wi-Fi a una plataforma web pública y controles privados para el administrador.

## Estado actual

**Repositorio en fase de documentación y planificación.** Todavía no contiene una aplicación web, firmware, despliegue ni pruebas de funcionamiento de actuadores.

Se ha revisado el esquemático compartido y se han identificado las señales previstas. La PCB física, las protecciones y la capacidad eléctrica del montaje siguen pendientes de validación.

**La Peltier debe permanecer deshabilitada hasta instalar y verificar disipación, ventilación y protección térmica.** El modo Manual no podrá saltarse ese bloqueo.

## Documentación

| Documento | Contenido |
| --- | --- |
| [Plan de desarrollo](docs/PLAN.md) | Arquitectura, seguridad, modos, fases detalladas, entregables, pruebas y decisiones pendientes |
| [Hardware y seguridad](docs/HARDWARE.md) | GPIO, bomba, alimentación con batería, MOSFETs, calibración y protección térmica |
| [Diseño de interfaz](docs/DISENO_UI.md) | Dirección botánica premium, dashboard, administración, móvil, accesibilidad y estados reales |

## Sistema previsto

- **ESP32-S3:** medición y control local; no necesita una laptop encendida durante la evaluación.
- **DHT11:** temperatura y humedad del aire; no mide humedad del suelo.
- **Next.js en Vercel:** dashboard público y API HTTPS.
- **Supabase:** datos, actualizaciones en tiempo real y autenticación.
- **Administración:** solo la cuenta autorizada puede solicitar cambios de modo y actuaciones.
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

Todas las fases siguientes están pendientes; la documentación no equivale a implementación.

1. Verificar PCB, alimentación, señales y etapa de potencia.
2. Desarrollar y comprobar firmware básico de sensores.
3. Conectar nube y dashboard público con telemetría real.
4. Incorporar administrador, modos y órdenes confirmadas.
5. Calibrar riego e implementar automatización local.
6. Incorporar protección térmica y habilitar Peltier; fase pospuesta.
7. Ensayar evaluación sin laptop, incluidos reinicios y desconexiones.

La nube y el frontend pueden desarrollarse sin alimentar las cargas. Las pruebas de potencia requieren superar la revisión eléctrica, y el enfriamiento no puede declararse operativo si la fase térmica sigue pendiente.

## Antes de implementar

- Contrastar el mapa de GPIO y orientación con la PCB ensamblada.
- Revisar excitación de los IRLZ44N a 3,3 V, alimentación, fusibles y cableado.
- Definir macetas/sustrato, medir caudal y aprobar la programación de riego.
- Disponer de proyectos Vercel/Supabase y cuenta administradora, sin compartir secretos en el repositorio.

El repositorio no instala dependencias ni incluye todavía comandos de compilación, carga o despliegue. Se añadirán junto con cada implementación comprobada.