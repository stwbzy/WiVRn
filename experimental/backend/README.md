# Backend VR experimental de Roblox / WiVRn

## Estado real

Se compila un backend nativo reutilizando el nucleo VR de Mocktail, fijado en
`upstream.lock`. No es un nuevo motor escrito desde cero ni un parche instalado
en Sober. La biblioteca contiene la implementacion OpenXR, transporte Vulkan y
GLES, acciones/controladores, hapticos, recuperacion de sesion, espejo opcional
y puente del dispositivo VR de Roblox. Nuestro proyecto agrega compilacion
aislada, preflight de payload y pruebas, sin compilar el launcher Android entero.

**Sober no esta conectado a esta biblioteca.** No hay interposicion automatica
por LD_PRELOAD. No se deben tratar sus binarios como un parche PCVR terminado.
No se cambiaron Sober, el APK, overrides, runtime global ni WiVRn original.

Continuacion: `../launcher/README.md` documenta un launcher aislado con el
host Android completo de Mocktail ya compilado. Roblox 3092 paso dos canaries
de arranque/cierre Vulkan en esta maquina. Es una aplicacion separada, no
una integracion en Sober; el usuario ya confirmo una partida estereoscopica real
con ese launcher. Las pruebas independientes del backend, por si solas, no
demuestran ese resultado. Ver tambien `../../docs/roblox-vr-hotfixes.md`.

## Arquitectura

1. `payload_gate.cpp` lee el GNU Build ID del ELF, busca el perfil y exige que
   los offsets coincidan con un contrato VR compilado. No carga codigo ni
   modifica memoria. Un perfil conocido sin capacidad VR tambien se rechaza.
2. `mocktail::vr::OpenXrBackend` crea instancia/sistema XR, delega la creacion
   de dispositivo Vulkan al runtime y ejecuta el ciclo de frames en el hilo
   de presentacion del renderer, sobre el mismo dispositivo del juego.
3. `RobloxVrDeviceBridge` publica poses e interfiere en las funciones verificadas
   del dispositivo VR del motor. No identifica ojos por orden de creacion:
   correlaciona el getter real de cada ojo con sus attachments de render.
4. El transporte copia ambos render targets a swapchains XR antes de enviar
   una capa de proyeccion. Si falta procedencia, pose o frame coherente,
   descarta la capa en lugar de mostrar imagenes incorrectas.
5. El adaptador del host Android debe conectar el linker, Vulkan/GLES y el
   lifecycle del juego con estas APIs. Esta parte aun no existe para Sober.

El adaptador de `src/graphics/bionic_vulkan_loader_adapter.cc` ya se compila
por separado, incluida su sincronizacion de colas y cinco pruebas asociadas.
`libwivrn-roblox-host-adapter.so` requiere callbacks de ventana SDL del host y
delega las llamadas Vulkan al backend activo. Se carga explicitamente, en
ambito local, no como reemplazo global del Vulkan del sistema. No se puede
suponer que sea binariamente compatible con el loader privado de Sober.

## Compilar

Dependencias del host: CMake, Ninja, C++17, headers Vulkan/OpenXR/EGL/GLES,
loader OpenXR, libelf, OpenSSL, SDL3, libplacebo, nlohmann-json y GTest para pruebas. No se instalo nada
en el sistema; en esta maquina se reutilizan las dependencias disponibles.

Si falta la copia upstream, clonar el repositorio dentro de
`experimental/mocktail-vr` y hacer checkout del hash exacto de `upstream.lock`.
La configuracion rechaza cualquier otro commit. Se conserva su licencia y
autoria en ese checkout; no se modificaron sus fuentes.

```bash
cmake -S experimental/backend -B build-roblox-backend -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-roblox-backend -j 4
ctest --test-dir build-roblox-backend --output-on-failure
```

Artefactos:

- `libwivrn-roblox-backend.so`: nucleo nativo y APIs C++/exports del upstream.
- `wivrn-roblox-preflight`: inspeccion read-only de un ELF y su manifest.
- `wivrn-roblox-backend-smoke`: arma/desarma el backend real, sin frames del juego.
- `wivrn-roblox-xr-probe`: descubrimiento o renderer independiente de prueba.
- `libwivrn-roblox-host-adapter.so`: dispatch Vulkan y traduccion Android WSI.
- `wivrn-roblox-host-smoke`: conexion real de ese adaptador con una ventana SDL
  oculta y el mismo VkDevice del runtime, sin cargar Roblox.
- `wivrn-roblox-contract-verify`: verifica hash e instrucciones del ELF local,
  sin ejecutar codigo guest ni instalar hooks.

Los headers del upstream son parte de la interfaz C++ de esta compilacion;
no se promete ABI estable entre diferentes revisiones o compiladores.

## Comprobar compatibilidad

```bash
build-roblox-backend/wivrn-roblox-preflight /ruta/a/libroblox.so \
  experimental/mocktail-vr/config/roblox_compatibility.json
```

`contract_available=1` indica solo existencia del contrato, no readiness de
un launcher. Exit 2 significa rechazo seguro. No hay opcion para forzarlo.
El host que integre el backend DEBE ejecutar el gate antes de `Install`,
notificar el mapping real a `Activate` y aceptar su validacion de instrucciones.
El gate es una comprobacion de compatibilidad, no autenticacion criptografica
del payload; no sustituye la verificacion de procedencia del APK.

El ELF instalado en Sober, inspeccionado el 2026-10-03, tiene Build ID:

```text
3b211f25d8658e88e28c86a4ff1e02ff96092741
contract_available=0
Unknown Roblox Build ID; no hooks will be installed
```

Ese rechazo sigue vigente en la compilacion normal y con el manifest upstream.
El analisis posterior encontro un contrato candidato especifico, descrito abajo;
no se promueve automaticamente a soportado. Su juego sigue intacto.
No se distribuye el ELF inspeccionado: la copia temporal esta en el directorio
de build ignorado. Evidencia en `diagnostics/stereo-control/backend-preflight.log`.

## Pruebas realizadas

- Compilacion de biblioteca compartida con `-Wl,-z,defs`: sin simbolos pendientes.
- 109 casos en ambas compilaciones: 93 nucleo/gate, 8 espejo/runtime,
  5 sincronizacion de colas y 3 verificaciones del payload; todos pasan.
- Se separaron las pruebas de espejo en otro proceso, como upstream, porque
  la opcion de instrumentacion de rendimiento se cachea en su primer uso.
- Preview sobre WiVRn 26.9 y Quest 3S: 120 frames, 119 renderizados y
  120 muestras de tracking. Es una escena independiente; no demuestra
  presentacion de render targets de Roblox por `OpenXrBackend`.
- La prueba smoke comprueba exclusivamente instancia/sistema y teardown
  del backend real. No activa hooks ni abre una sesion grafica del juego.
- Host smoke normal y candidato: sesion XR creada sobre VkDevice de la Arc
  A750, dos swapchains de 1465x1535 y cola valida. `roblox_loaded=0` y
  `submitted_frames=0`: no es una validacion del renderizado del juego.

## Contrato candidato de la build instalada

`aapt2` confirma Roblox `2.737.1584`, versionCode `3056`.
El analizador upstream encontro RTTI/vtable, constructor, getter de estado,
getters/inicializador de ojos, flags, hapticos y getter de puntero espacial.
Resultado sin advertencias: `diagnostics/stereo-control/roblox-installed-vr-analysis.json`.
Se ejecuta con Capstone 5.0.6 en un venv dentro del build, no en Python global.

El verificador C++ compara diez comprobaciones con las mismas funciones de
validacion del backend: Build ID, SHA-256, RELRO, almacenamiento writable,
getters, constructor, inicializador, puntero y hapticos. Hash exacto esperado:

```text
a0464843bc408a37d05cd8a6cb20090f295b7299e73dcbe4a06d23d46ad7314e
```

`installed_contract.h` y `installed-candidate.json` registran los datos
derivados, no offsets extrapolados de otra version. El manifest es experimental,
no permitido por defecto y NO habilita replay de constructores del host Android.
Esto valida un subconjunto del ABI VR estaticamente; no valida por separado
todos los consumidores de poses, inicializacion JNI/allocator o una partida.

La variante opcional se construye en otro directorio:

```bash
cmake -S experimental/backend -B build-roblox-backend-candidate -G Ninja \
  -DWIVRN_ENABLE_INSTALLED_VR_CONTRACT=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-roblox-backend-candidate -j 4
ctest --test-dir build-roblox-backend-candidate --output-on-failure
build-roblox-backend-candidate/wivrn-roblox-preflight --experimental \
  build-roblox-backend/libroblox-inspected.so \
  experimental/backend/installed-candidate.json
```

El preflight experimental devuelve `contract_available=1 integration_required=1`.
Sin `--experimental`, o sin la opcion de compilacion, se rechaza. `CheckPayload`
verifica ademas el hash y las instrucciones; `CheckBuild` solo inspecciona metadata.
No usar `CheckBuild` como sustituto de `CheckPayload` antes de integrar un loader.
`Install` tambien exige que `WIVRN_ROBLOX_EXPERIMENTAL_BUILD` coincida con el
Build ID exacto. No se configuro esta variable ni se ejecuto `Activate` sobre Roblox.

CMake genera un overlay del fichero del puente en el directorio de build,
manteniendo intacto el checkout upstream. No altera el binario instalado.
Para reproducir las tres pruebas con ELF local, pasar en CMake
`-DWIVRN_TEST_PAYLOAD=/ruta/absoluta/a/libroblox.so`; el ELF no se redistribuye.
La prueba de corrupcion solo modifica una copia temporal dentro del build.

Logs: `backend-tests.log`, `backend-candidate-tests.log`,
`backend-host-smoke.log`, `backend-candidate-host-smoke.log`,
`backend-candidate-preflight.log`, `backend-candidate-disabled.log` y
`roblox-installed-contract.log`, en `diagnostics/stereo-control`.

## Integracion del host

El launcher de `../launcher/` incorpora linker Bionic, allocator/JNI,
SurfaceController y adaptadores graficos upstream dentro de un proceso
separado. Se obtuvo el ELF de referencia exacto de 2998, verificando su hash.
La derivacion del sidecar HostAbi para la build instalada 3056 se detuvo en
la firma `fmod-output-select`; no se invento ese perfil ni se desactivaron
sus validaciones. El launcher usa la build 3092 con contratos upstream,
verificacion de firma/hashes y dos canaries locales antes de la activacion.

Para Sober cerrado no se dispone de una interfaz documentada de linker/renderer
en este proyecto. La ruta controlable sigue siendo el launcher abierto de
Mocktail con payload y HostAbi verificados. No ejecutar el ELF de Android con
`dlopen` de glibc ni inyectar este adaptador a ciegas en Sober.

No basta interceptar `xrEndFrame`: tambien hay que activar el dispositivo VR,
aplicar poses antes del render, obtener dos imagenes verdaderas y compartir
dispositivo, colas, semaforos y lifecycle correctamente.

Fuente: https://github.com/komaruworld/mocktail/tree/vr
