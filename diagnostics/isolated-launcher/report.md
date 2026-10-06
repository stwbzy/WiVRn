# Launcher aislado: diagnostico 2026-10-03

## Artefactos

- Host Mocktail completo: `build-roblox-launcher/mocktail`.
- Wrapper: `tools/roblox-vr.sh` y `experimental/launcher/launcher.py`.
- Dependencias extraidas localmente: `build-roblox-launcher-deps/`.
- Store y logs privados: `build-roblox-launcher-state/` (ignorado por Git).
- Upstream fijado en `9a1ccbb5cebea7bb578bfb198d2a3246734e6a40`;
  checkout y submodulos preservados sin modificaciones.

## Payloads

3056 / 2.737.1584, instalado en Sober:

- APK importado read-only y firmas verificadas por el parser nativo.
- Referencia 2998 descargada: APK SHA-256
  `a4a71ec39df6619a56ab7adce0959d1d60354835485b53806f4026914287ea86`.
- Referencia ELF SHA-256
  `4343a6a900d1fca27ff8e10b9d9c86bee40a5d0ba547cb29903b7769f4f42a9d`.
- Derivacion rechazada: `signature fmod-output-select matched 0 candidate locations`.
- No se activo ese candidato ni se ejecuto su ELF en el launcher.
- El helper shell upstream interpreta mal la salida de apksigner SDK 35
  (`Signer (minSdkVersion=...)`). No se amplio la lista de confianza ni se
  ignoro la firma: la importacion usa el verificador nativo upstream.

3092 / 2.738.1397, independiente de Sober:

- Build ID `5f0704edd9064f566ee3d6df2bd2fabbcc709f03`.
- ELF SHA-256 `8f7079c8977b88c8d9b61be201f7156b5041d7623863357f9ad6c3b0062531e9`.
- APK SHA-256 `bbe00ae306cc251c4ea55b7a932d9c524ecb0d6d9203c2a6161bcf0fae792742`.
- Firma verificada; contratos HostAbi y VR compilados upstream.
- Dos canaries anonimos exitosos, con mas de 240 queue presents cada uno,
  primera imagen real, allocator nativo, legacy patches/signal recovery
  desactivados, Vulkan WSI cerrado y lifecycle `Stopped`.
- `current.json` del store privado fue activado solo despues de ambos canaries.

## Quest / XR

- Quest 3S por ADB Wi-Fi, modelo consultado exitosamente.
- WiVRn 26.9 conectado por LAN al servidor 26.9.
- Runtime seleccionado solo por proceso; no se cambio el runtime global.
- Probe en el sandbox del launcher: 120 frames, 119 renderizados, 120 tracked;
  GPU Intel Arc A750. Es la escena independiente, NO una experiencia Roblox.
- Inicio de sesion independiente confirmado por el usuario; la ventana de
  autenticacion cerro y el cliente Roblox sigue abierto.
- Guest Roblox: dos swapchains 1465x1535, vinculados al mismo VkDevice de Arc
  A750. Eye getters/initializer del guest aportan recursos de ojo distintos.
- Proyecciones OpenXR reales observadas: mas de 9.000 cuadros enviados;
  cabeza recibida del runtime y aplicada a DebugDeviceVR.
- Actions de mandos creadas y adjuntadas; el ultimo estado observado sigue
  inicialmente `hands=0/0 input=0`, despues se observaron mandos Touch Plus
  activos y el usuario confirmo que funcionaban en partida.
- El usuario confirmo profundidad y experiencia VR real, con capturas. Indico
  vista inicial bajo el avatar, navegacion home solo gamepad y rayo visible
  distinto del hit-test. Calidad insuficiente, audio desktop y baja fluidez.

## Parche de usabilidad VR

- HomeView: mantener Menu izquierdo 1500 ms, solo tras LuaAppReady/Browsing.
  Joining/Playing/Shutdown lo bloquean. No modifica la camara en experiencia.
- Hit-test nativo conservado: se elimina la sustitucion por otra pose aim para
  no mezclar el rayo que se dibuja con el que selecciona.
- Perfil balanced: swapchains e imagenes del guest verificados 1832x1920, antes
  1465x1535. MSAA 2 solicitado y mip bias extra eliminado; el usuario confirmo
  calidad visual satisfactoria. No se cambio la resolucion del stream de WiVRn.
- Salida SDL y menu nativo FMOD verificadas `selected=WiVRn`, sin cambiar el
  sink predeterminado del escritorio. Audicion en las Quest pendiente.
- Espejo adicional desktop desactivado por defecto; vsync del monitor off.
- 31 pruebas Python y 13 pruebas nativas del nuevo overlay pasan. El nuevo
  ejecutable volvio a pasar dos canaries de desktop antes de abrir VR.
- El usuario confirmo recentrado correcto en el inicio y trayectoria de
  seleccion alineada dentro de la experiencia.
- El usuario reporto manos del avatar giradas hacia afuera con los mandos en
  posicion neutra. Se registra como HF-001 pendiente; solicito no corregirlo
  en esta entrega. Seguimiento en `docs/roblox-vr-hotfixes.md`.

## Rendimiento observado

- Antes del parche: muestra aproximada de 12 segundos, 39,9 FPS enviados desde
  el PC, con incertidumbre de 9,9 FPS por la cadencia de los marcadores.
- Despues, en el inicio: contadores detallados dan 36,0 presents/s y 36,0
  proyecciones/s. No hay evidencia de mejora de FPS en esta muestra.
- Frame cycle ~22,9 ms; xrWaitFrame ~22,0 ms; fence de copia ~0,47 ms.
  xrWaitFrame es sincronizacion del runtime, no tiempo de renderizado GPU.
- Pendiente comparar en la misma experiencia y diagnosticar el pacing del
  runtime; estos contadores no miden codificacion, Wi-Fi o decoder Quest.

## Aislamiento y limites

El filesystem sandbox oculta el home original, deja proyecto/sistema/payloads
read-only y tiene datos, runtime temporal y autenticacion propios. Se comparte
red/GPU y sockets necesarios de sesion; no es una barrera completa frente a
codigo hostil. El camino PCVR ya tiene aceptacion visual del usuario en una
experiencia. Recentrado, rayo y calidad tambien fueron confirmados; falta la
comprobacion fisica de audicion en el visor.
La seleccion por rayo en los paneles del inicio aun no
esta implementada. `vr-status` permite consultar contadores sin exponer logs.
Los logs con potencial informacion de sesion no se adjuntan ni versionan.
