# Control grafico estereo independiente

## Ejecucion

Desde una terminal interactiva, con WiVRn conectado a las Quest:

```bash
cd path/to/WiVRn
bash tools/stereo-control.sh
```

El launcher usa el ejecutable `hello_xr` de Khronos ya instalado, con Vulkan2,
configuracion Stereo, espacio Local y composicion opaca. No es un renderer
propio ni un adaptador de Roblox. A diferencia del harness FSR existente,
esta aplicacion dibuja geometria en imagenes Vulkan reales.

La seleccion del runtime solo afecta a este proceso. El launcher desactiva
la capa XeSS implicita local y las capas explicitas heredadas para el control.
No modifica archivos globales, Sober, el APK ni el servidor personalizado.
`WIVRN_CONTROL_RUNTIME` permite seleccionar otro manifest explicitamente.
Ctrl+C detiene la prueba. Los logs llevan fecha UTC y PID del launcher.

## Resultado observado: 2026-10-03

- Servidor WiVRn Flatpak 26.9, cliente de tienda 26.9, Quest 3S por USB.
- GPU del servidor: Intel Arc A750; encoder VAAPI AV1.
- Runtime: WiVRn v26.9, tracking de posicion y orientacion anunciado.
- Configuracion PRIMARY_STEREO con dos vistas.
- Swapchains por ojo de 1465x1535, sampleCount 1.
- Sesion READY -> SYNCHRONIZED -> VISIBLE -> FOCUSED.
- En el lanzamiento limpio el loader confirma que XeSS esta deshabilitado
  y crea la instancia sin capas habilitadas.
- `quest-screen.png`, capturada mediante ADB durante ese lanzamiento, muestra
  cubos de colores en ambas vistas con posiciones/perspectivas distintas.
  Esto confirma recepcion de imagenes de la escena en el visor, no solo IPC.

La primera ejecucion incluia XeSS y se cerro antes de repetir sin esa capa.
El log limpio es `20261003T182650Z-283886.log`.

## Limites

El usuario confirmo: "Si, si se ve el cubo". Esto confirma visibilidad real
de la escena. No confirma por separado profundidad o tracking estable.
La captura estatica no mide latencia, FPS sostenidos, fidelidad del tracking
ni comodidad binocular.
No demuestra compatibilidad grafica dentro del sandbox de Sober: esta prueba
usa el loader y las bibliotecas del host, con el runtime del servidor Flatpak.

Se intento una auditoria breve con la capa LUNARG api_dump. La ultima captura
`api-audit.log` solo contiene el inicio de la aplicacion y no aporta evidencia
de frames; no debe interpretarse como fallo del renderer ni como prueba de
`xrEndFrame`. La evidencia visual es la captura de las Quest y el lanzamiento
sin esa capa. La auditoria interrumpida por timeout no es un crash diagnosticado.

## Comprobacion en el visor

1. Ver cubos con volumen, no una ventana de escritorio.
2. Girar la cabeza: los cubos deben permanecer anclados al espacio.
3. Desplazar ligeramente la cabeza: comprobar paralaje.
4. Comparar ambos ojos cerrando uno cada vez: imagen coherente, no duplicada.
5. Informar de imagen negra, temblores, perspectiva invertida o profundidad rara.

Esto establece un control grafico antes de adaptar el backend VR de Roblox;
no habilita VR en Sober por si mismo.

Fuente: https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr
