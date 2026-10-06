# Roblox VR: siguientes hotfixes

Estado revisado: 2026-10-03. Launcher aislado de Mocktail, Roblox 3092 /
2.738.1397, Quest 3S, WiVRn servidor/APK oficiales 26.9, Intel Arc A750.

## HF-001: orientacion de las manos del avatar

**Pendiente; no corregir en esta entrega, por solicitud del usuario.**

Reproduccion: dentro de una experiencia VR, sostener ambos mandos hacia delante
en su orientacion neutra de descanso. Las manos del avatar aparecen abiertas
y giradas hacia afuera, en vez de seguir naturalmente los mandos.

Antes del arreglo se debe comparar la orientacion OpenXR grip, aim y el frame
nativo de mano por lado, incluyendo convenciones de ejes, perfil Touch Plus y
correcciones existentes del host. No aplicar un angulo arbitrario al rayo ni
alterar la camara del juego para compensar la mano.

Criterios de aceptacion:

- Ambas manos coinciden con la orientacion neutra y la rotacion real del mando.
- No hay inversion izquierda/derecha al rotar munecas o elevar los brazos.
- El rayo visible y el punto de seleccion mantienen la alineacion ya validada.
- El recentrado del inicio no afecta a las manos ni a la camara en partida.
- Comparar mas de una experiencia para separar un error del puente de uno
  especifico del avatar/juego.

## HF-002: frame pacing y fluidez

Pendiente. La ultima muestra del inicio dio 36 presents/s y 36 proyecciones/s
en el PC. No se afirma que sean los FPS vistos en las Quest ni que el parche
haya mejorado el rendimiento. Comparar la misma experiencia con ambos perfiles,
medir render GPU, sincronizacion XR y transporte por separado. Conservar el
ciclo correcto de `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`.

## HF-003: seleccion por rayo en el inicio

Pendiente. El inicio conserva navegacion tipo gamepad: joystick y boton A.
La correccion del rayo ya validada se refiere al menu dentro de la experiencia;
no implementa clicks por rayo en los paneles del inicio de Roblox.

## Verificacion de audio pendiente

La seleccion de salida WiVRn se comprobo en SDL/FMOD y solo se aplica al proceso
aislado. Falta confirmacion explicita de audicion en el visor. No cambiar la
salida predeterminada del escritorio ni mover audio de otras aplicaciones.

## Cambios confirmados por el usuario

- VR real con profundidad y mandos dentro de una experiencia.
- Recentrado del inicio manteniendo Menu izquierdo durante 1,5 segundos.
- Trayectoria de seleccion alineada con el rayo visible dentro del juego.
- Calidad visual satisfactoria con el perfil balanced, 1832x1920 por ojo.

No confundir esta aceptacion con soporte general de otras builds de Roblox,
ausencia de errores de avatar o garantia de frecuencia estable del visor.
