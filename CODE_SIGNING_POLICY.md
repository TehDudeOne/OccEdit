# Code signing policy

> **Estado: solicitud a SignPath Foundation pendiente. OCCEdit no está aprobado ni firmado.** Los artefactos de GitHub Actions son compilaciones de prueba sin firma digital.

## Reconocimiento del proveedor

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

**Este reconocimiento describe el servicio que se pretende solicitar; no significa que OCCEdit tenga actualmente acceso a él.**

## Responsables y control de cambios

- **Autor / maintainer / committer:** [TehDudeOne](https://github.com/TehDudeOne).
- **Revisor de contribuciones externas:** [TehDudeOne](https://github.com/TehDudeOne). Toda contribución ajena se revisará antes de fusionarse.
- **Aprobador de firmas:** [TehDudeOne](https://github.com/TehDudeOne), sujeto a aprobación y a configuración posterior en SignPath.
- Los responsables deben utilizar autenticación multifactor en GitHub y SignPath antes de solicitar firmas.
- Ninguna solicitud de firma será automática: cada versión requerirá aprobación explícita.

## Origen verificable de los binarios

- Las compilaciones para firmar deberán producirse mediante flujos de GitHub Actions controlados y auditablemente ligados al código público del repositorio.
- No se firmarán binarios externos ni componentes propietarios de terceros.
- Las dependencias se documentan en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- La identidad de producto y versión en los metadatos PE se verificará antes de solicitar la firma.
- El workflow de firma y las credenciales se agregarán **solo después de recibir aprobación y configurar los identificadores oficiales**.

## Privacidad

**This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.**

OCCEdit es una aplicación local para leer y editar archivos OCC seleccionados por la persona usuaria. No se pretende incorporar telemetría ni carga automática de informes. Este compromiso se revisará en cada contribución. El usuario debe conservar copias de sus originales; archivos OCC de clientes, ubicaciones o ensayos reales no se incluirán en el repositorio ni en artefactos públicos.

## Distribución

No existe todavía una versión firmada, ni se declara compatibilidad universal con OMICRON Test Universe. La publicación de una versión firmada quedará condicionada a completar pruebas, preparar una versión reproducible y obtener aprobación externa de SignPath Foundation.

Condiciones oficiales: https://signpath.org/terms
