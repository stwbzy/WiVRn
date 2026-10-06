# Sober / WiVRn: diagnostico real, 2026-10-03

## Resultado

Las Quest se conectaron a WiVRn. Una aplicacion de control dentro del sandbox
de Sober pudo crear una instancia OpenXR y descubrir el visor. Sin embargo,
la ejecucion de Roblox observada siguio renderizando una ventana de escritorio.
No se implemento ni valido un parche PCVR.

La evidencia apunta a la activacion/backend VR de Sober/Roblox como siguiente
area de investigacion, no a cambiar primero el transporte de WiVRn. No demuestra
que sea imposible adaptar ese backend ni que jamas haya llamadas XR.

## Aislamiento

- Copia de trabajo separada del checkout original de WiVRn; se conservaron sus cambios existentes.
- La copia excluye directorios de compilacion y caches de Gradle/CMake.
- No se modifico el runtime OpenXR global ni los overrides persistentes de Sober.
- El control uso WiVRn Flatpak 26.9 y APK de tienda 26.9, no el servidor
  personalizado antiguo del proyecto. Esto evita mezclar versiones en esta prueba.
- Sober instalado: 1.7.1; etiqueta de ejecucion `s0122-3056-x86_64-gnu`.
- Quest 3S conectadas por ADB y reverse TCP 9757.

## Evidencia positiva de runtime

Ver `runtime-probe.log` y `runtime_probe.c`. La aplicacion carga directamente
`libopenxr_wivrn.so`, negocia la interfaz del runtime y consulta el sistema:

```text
xrCreateInstance result=0
xrGetSystem result=0
xrGetSystemProperties result=0
system=Meta Quest 3 on WiVRn orientationTracking=1 positionTracking=1
xrDestroyInstance result=0
```

El nombre es el reportado por el runtime; el dispositivo fisico es Quest 3S.
Este control NO usa el loader normal, NO crea una sesion grafica, NO consulta
poses por frame y NO envia imagenes estereo. Prueba acceso a la biblioteca y
descubrimiento del visor por IPC, no renderizado PCVR completo.

El runtime ofrece `XR_KHR_vulkan_enable2` y `XR_FB_display_refresh_rate`.
En esta enumeracion no ofrece `XR_KHR_composition_layer_depth`,
`XR_FB_space_warp` ni `XR_META_performance_metrics`. Un adaptador debera
negociar capacidades; su ausencia no prueba por si sola un bloqueo de Roblox.

## Evidencia del juego

Ver `sober-render-evidence.log`, filtrado del registro de esta ejecucion:

- `SurfaceController::disableVR` al iniciar.
- Swapchains de ventana Vulkan, incluidos 1920x1080 y 941x1021.
- `game_loaded` para una experiencia, seguido de regreso al menu.
- El usuario reporto que Roblox cargo como PC normal y nada cambio en VR.

La primera reapertura fue rechazada porque ya habia una instancia de Sober.
Se inicio despues una instancia nueva con los permisos temporales y el manifest
de WiVRn; esa instancia si registro entrada en una experiencia.
La muestra de `/proc/280795/maps` no mostro bibliotecas `openxr` o `wivrn`.
Si mostro `/app/bin/libloader.so`, que es el loader de Sober y no demuestra
la presencia del loader OpenXR. Roblox puede mapearse mediante su loader propio.

## Limitaciones y pruebas descartadas

- Lanzar Sober bajo strace termino con SIGSEGV antes de cargar el juego.
  `sober-syscalls.log` NO es evidencia valida de ausencia de llamadas XR del juego.
  La causa de ese fallo no se determino.
- La lectura de `environ` y `fd` del proceso fue rechazada por el sistema;
  no se forzo acceso ni se inspeccionaron variables privadas.
- Una muestra de bibliotecas no detecta necesariamente cargas transitorias.
- No se verifico de forma independiente si la experiencia probada soporta VR.
- No se capturo una secuencia completa `xrCreateSession` / `xrEndFrame` de Roblox.

## Reproduccion

Con WiVRn Flatpak conectado a las Quest y Sober cerrado:

```bash
bash diagnostics/sober-2026-10-03/launch-sober-xr.sh
```

No ejecutar strace en el juego para reproducir esta prueba.
Compilacion del control, con headers OpenXR instalados:

```bash
cc -O2 -Wall -Wextra diagnostics/sober-2026-10-03/runtime_probe.c -ldl \
  -o diagnostics/sober-2026-10-03/runtime-probe
```

## Siguiente fase propuesta

1. Control grafico independiente con dos vistas, swapchains y envio de frames
   OpenXR a WiVRn. Mantenerlo separado de Roblox.
2. Evaluar un backend abierto que active el dispositivo VR del motor y obtenga
   los render targets reales por ojo. Mocktail rama `vr` es una referencia,
   no un parche listo para el binario actual de Sober.
3. Si el backend x86 de esta version no tiene la ruta necesaria, evaluar el
   cliente Quest ARM con traduccion compatible, como la arquitectura de Cordial.
4. Validar version/build y ABI antes de cualquier hook. Rechazar builds
   desconocidas; no usar offsets de versiones anteriores a ciegas.
5. Solo adaptar WiVRn si una prueba valida identifica una incompatibilidad
   concreta de extensiones, graficos o IPC. No simular PCVR duplicando la ventana.

Referencias de la investigacion previa:

- https://github.com/vinegarhq/sober/issues/500
- https://vinegarhq.org/Sober/FAQ/index.html
- https://create.roblox.com/docs/production/publishing/vr-guidelines
- https://github.com/komaruworld/mocktail/tree/vr
- https://github.com/luohoa97/cordial/blob/main/docs/vr.md
