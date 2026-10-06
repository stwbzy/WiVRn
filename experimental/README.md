# Roblox VR / WiVRn: experimental

Este directorio contiene un backend OpenXR de prueba y un launcher Android
nativo aislado que reutiliza Mocktail. No contiene el codigo de Sober ni instala
un parche dentro de Sober. No es una distribucion oficial de Roblox.

- [Launcher, preparacion, controles y pruebas](launcher/README.md).
- [Backend, contratos y comprobacion de payloads](backend/README.md).
- [Hotfixes pendientes](../docs/roblox-vr-hotfixes.md).
- [Procedencia y licencias](THIRD_PARTY_NOTICES.md).
- [Informe revisado de pruebas](../diagnostics/isolated-launcher/report.md).

El script `launcher/build.sh` obtiene el checkout de Mocktail del commit exacto
en `backend/upstream.lock`. Ese checkout no se versiona dentro del repositorio:
se conserva limpio y los cambios se generan mediante overlays en el build.
No hace falta compilar el servidor ni el APK personalizado de WiVRn para probar
este launcher; las pruebas VR documentadas usaron servidor y APK oficiales 26.9.

Esta rama publica forma parte del fork de WiVRn en
https://github.com/stwbzy/WiVRn/tree/feature/roblox-vr y conserva el historial
del proyecto base. La cuenta y la sesion de Roblox se crean localmente y no
forman parte del codigo publicado.
