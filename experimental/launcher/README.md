# Launcher aislado de Roblox / WiVRn

## Estado

Host Android completo de Mocktail compilado, no una inyeccion en Sober.
Revision fijada: `9a1ccbb5cebea7bb578bfb198d2a3246734e6a40`.
Las modificaciones se aplican como overlays CMake en el build; el checkout
upstream y el WiVRn original permanecen intactos.

En esta maquina se preparo Roblox `2.738.1397`, versionCode `3092`, Build ID
`5f0704edd9064f566ee3d6df2bd2fabbcc709f03`. Esta revision contiene contratos
HostAbi y VR upstream. Se verificaron firmas APK, identidad ELF y hashes.
Dos canaries anonimos presentaron frames Vulkan reales y terminaron con
`Roblox lifecycle shutdown: Stopped`, sin legacy binary patches ni signal
recovery. Eso valida el arranque del host, no una partida PCVR en las Quest.

Despues del inicio de sesion independiente se observaron proyecciones OpenXR
del guest Roblox, recursos Vulkan de ojo distintos y poses de cabeza del
runtime aplicadas a DebugDeviceVR. No son las imagenes del probe independiente.
El usuario confirmo despues una experiencia VR con profundidad, seguimiento y
mandos, y envio capturas del juego. Eso confirma el camino real guest/OpenXR,
no la ausencia de problemas: el inicio tenia un origen incorrecto, el rayo de
seleccion no coincidia con el visible y quedaban calidad/audio/rendimiento.

Tras el parche el usuario confirmo recentrado, alineacion del rayo y calidad
visual. Reporto orientacion incorrecta de las manos del avatar y pidio dejarla
pendiente: ver [siguientes hotfixes](../../docs/roblox-vr-hotfixes.md).

La build `3056` instalada en Sober se puede importar como candidato, pero la
derivacion HostAbi fallo en `fmod-output-select`. No se forzo su activacion
ni se sustituyeron sus offsets por los de otra version. Su contrato VR estatico
no basta para permitir un arranque Android completo.

## Aislamiento

- Datos/configuracion/cache/autenticacion separados en
  `build-roblox-launcher-state/`, con permisos privados.
- Bubblewrap obligatorio: sistema y proyecto read-only, `/home` oculto salvo
  esta copia, payloads y perfiles read-only, `/tmp` privado y PID namespace.
- `/run/user/UID` se monta desde un directorio privado; solo se enlazan los
  sockets necesarios de Wayland, D-Bus, PipeWire/PulseAudio y WiVRn. Los locks
  del launcher no se escriben en la sesion del host.
- No se importan cookies, cuenta, preferencias o archivos privados de Sober.
  `import-apks` lee exclusivamente los dos APK indicados y no ejecuta su ELF.
- Se descartan las variables heredadas de credenciales, hooks, librerias y
  rutas legacy. No existe fallback a un arranque guest sin Bubblewrap.
- Runtime XR seleccionado por proceso, sin tocar `active_runtime.json`,
  overrides Flatpak, asociaciones de enlaces de Roblox ni aplicaciones Quest.
- Compilar OpenXR no activa VR: requiere `vr`/`--vr`. Los canaries lo desactivan
  explicitamente; la aprobacion desktop no equivale a aceptacion estereoscopica.

Es aislamiento de archivos y procesos para pruebas de compatibilidad, no una
garantia frente a codigo hostil: se comparten red, GPU y sockets de sesion.

## Compilar y preparar

```bash
cd path/to/WiVRn
bash experimental/launcher/build.sh
bash experimental/launcher/prepare-supported.sh
./tools/roblox-vr.sh doctor
```

`build.sh` usa las dependencias nativas existentes y extrae cuatro paquetes
de la metadata pacman del host en `build-roblox-launcher-deps/`. Comprueba
SHA-256; no instala paquetes en `/usr`. Estos paquetes son especificos del
host CachyOS/Arch x86_64 actual. En otras distribuciones hay que adaptar ese
prefijo, no extraer estas bibliotecas indiscriminadamente sobre el sistema.
Los paquetes WebKit se montan en un overlay read-only de `/usr/lib` solamente
dentro del sandbox, para respetar sus rutas libexec compiladas.

El APK exacto de bootstrap tiene SHA-256
`bbe00ae306cc251c4ea55b7a932d9c524ecb0d6d9203c2a6161bcf0fae792742`.
`prepare-supported.sh` no descarga "latest": verifica esa revision, importa
solo a staging y requiere dos canaries antes de activar el payload privado.
Un fallo mantiene `current.json` intacto. No hay credenciales en esos canaries.

## Usar

```bash
./tools/roblox-vr.sh status
./tools/roblox-vr.sh desktop
./tools/roblox-vr.sh vr
./tools/roblox-vr.sh vr --quality balanced --diagnostics
./tools/roblox-vr.sh probe
```

El primer arranque abre una ventana propia de inicio de sesion de Roblox.
Iniciar sesion alli, no enviar credenciales por chat. `vr` requiere WiVRn
ejecutandose y las gafas conectadas. `probe` es una escena independiente de
120 frames, no renderizado de Roblox. El wrapper conserva el lock hasta que
su proceso termina y evita dos operaciones concurrentes sobre este store.

Para otro runtime WiVRn compatible, `vr`/`probe` aceptan
`--runtime /ruta/absoluta/openxr_wivrn.json`. No seleccionan SteamVR/ALVR.

### Inicio y mandos

Para recentrar solo el inicio: mirar al frente, mantener **Menu del mando
izquierdo durante 1,5 segundos** y soltar. Se usa el boton fisico; no se ha
inyectado un boton grafico en el LuaApp cerrado de Roblox.

El permiso procede de `OnLuaAppReady` y del ciclo nativo Browsing/Joining/
Playing. Queda bloqueado antes de comenzar una experiencia. Al volver al
inicio hay que hacer una pulsacion nueva. Una pulsacion corta, tracking perdido
o mandos dormidos no activan el recentrado. Cabeza, suelo, grip y aim comparten
la misma transformacion rigida; los offsets de ojo, el FOV y las poses de
composicion OpenXR no se alteran. Se conserva la gravedad, recentrando yaw y
posicion pero no inclinando el mundo con el pitch del casco.

El hit-test del menu usa ahora el frame nativo que alimenta el rayo visible,
sin reemplazarlo por otra pose OpenXR aim. Esto evita dos trayectorias distintas
sin inventar un angulo de compensacion. El usuario confirmo esa alineacion
en el menu dentro de una experiencia.
El inicio de Roblox mantiene su navegacion
gamepad existente; este parche no afirma haber implementado ray-click en sus
paneles LuaApp.

### Calidad y audio

- `balanced` (predeterminado): escala 1,25, MSAA 2 solicitado, texturas sin mip
  bias extra y presupuesto 512 MiB. En esta Quest se verificaron recursos y
  swapchains reales 1832x1920 por ojo.
- `quality`: escala 1,5, MSAA 4 solicitado y presupuesto 1024 MiB. Mayor coste
  GPU; no es garantia de mantener la frecuencia del casco.
- `performance`: escala 1,0, MSAA 1, LOD bajo y presupuesto 512 MiB.
- `legacy`: politicas graficas anteriores, para comparar.

La escala se aplica a las imagenes que Roblox renderiza, no a un reescalado de
una imagen plana. Se respetan los limites OpenXR. El video codificado de WiVRn
mantiene su configuracion: una imagen fuente mayor no garantiza un stream de
mayor resolucion. Los perfiles no legacy devuelven el control al slider de
calidad de Roblox, en vez de forzar siempre nivel 3. No fuerzan calidad maxima.

VR selecciona la salida `WiVRn` de SDL/PulseAudio antes de arrancar FMOD.
`PULSE_SINK=wivrn.sink` se aplica solo al proceso aislado; no se cambia el sink
predeterminado ni se mueven Discord/navegador/otras aplicaciones. Si la salida
del casco no existe, falla de forma explicita; `--audio desktop` es un opt-out
del usuario. Esto enruta reproduccion, no cambia el microfono ni permisos.

Se desactiva la copia adicional de ojos al espejo desktop por defecto
(`--mirror` la restaura) y la sincronizacion con el monitor; se conserva el
reloj de frames de OpenXR. `--diagnostics` mide el flujo del PC y las etapas XR.
No se elimina `xrWaitFrame` ni se fabrican cuadros para inflar los FPS.

## Diagnostico VR

Si se captura el arranque en un log privado, se puede consultar sin publicar
sus lineas originales ni detener la sesion:

```bash
./tools/roblox-vr.sh vr > build-roblox-launcher-state/state/vr-session.log 2>&1
# Desde otra terminal mientras el launcher sigue abierto:
./tools/roblox-vr.sh vr-status --log build-roblox-launcher-state/state/vr-session.log --sample-seconds 3
```

`vr-status` emite solo campos numericos/booleanos y estados enumerados a partir
de marcadores VR permitidos. Rechaza rutas fuera de `state/`, symlinks y logs
mayores de 64 MiB. No lee la carpeta `auth/`. El muestreo permite distinguir
contadores historicos de proyecciones que siguen avanzando. Recursos de ojo
distintos no garantizan que sus pixeles sean diferentes ni que la experiencia
responda correctamente a la cabeza: eso requiere confirmacion visual.
Mandos `null` significan que no hay evidencia de su estado; `false` indica el
ultimo estado inactivo observado, no necesariamente un fallo del backend.
Con `--diagnostics`, un muestreo de 10-15 segundos informa por separado
`pc_present_fps`, `pc_projection_fps` y tiempos de espera/copias. Son anteriores
a la codificacion/transmision/decodificacion, no FPS medidos en las Quest.
Sin diagnosticos, `pc_projection_fps_estimate` es aproximado y muestra su
incertidumbre por la cadencia de 120 frames de los marcadores.

## Candidatos

```bash
./tools/roblox-vr.sh import-apks /ruta/base.apk /ruta/split_config.x86_64.apk
./tools/roblox-vr.sh derive VERSIONCODE-BUILDID /ruta/libroblox-referencia.so
./tools/roblox-vr.sh canary VERSIONCODE-BUILDID
```

Solo se acepta como referencia el ELF cuyo hash coincide con el perfil
fijado `roblox_host_abi_reference.json`. La derivacion se detiene si una firma
no coincide de forma unica. La build local `3056` tiene un gate VR adicional
de hash/instrucciones y opt-in por Build ID; este gate no permite evitar la
aprobacion HostAbi. No se exponen `--force-run-latest` ni
`--allow-unverified-build` en los arranques normales del wrapper.

## Verificar

```bash
python3 -m unittest discover -s experimental/launcher -v
ctest --test-dir build-roblox-launcher --output-on-failure
ctest --test-dir build-roblox-backend-candidate --output-on-failure
```

Pruebas del launcher: variables heredadas, roots privados, paths y symlinks,
referencia invalida, ausencia de Bubblewrap, flags de fuerza y ocultacion real
del directorio Sober dentro de un sandbox. Los logs nativos sin sanitizar
permanecen en el directorio privado ignorado del build, no se versionan.
El parser VR prueba redaccion, recursos distintos/nulos, GLES, seguimiento,
reinicio y perdida de sesion, ademas de negar el acceso a archivos de auth.
Las pruebas nativas del overlay cubren pulsacion larga/rearme, aislamiento del
juego, retorno al inicio, tracking invalido, gravedad y distancias relativas,
ademas de perfiles y dimensiones limitadas por el runtime.

Fuente y licencia del host: https://github.com/komaruworld/mocktail/tree/vr
(Apache-2.0). No se redistribuyen APK, ELF o credenciales de Roblox.
