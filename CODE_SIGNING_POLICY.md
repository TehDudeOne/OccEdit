# Code signing policy

**Estado: preparación para solicitar acceso a SignPath Foundation. El proyecto NO está aprobado ni firmado actualmente.**

## Publicación y procedencia

- El código público de este repositorio será la fuente de las compilaciones oficiales.
- Las versiones preliminares del workflow se publican **SIN FIRMA** para pruebas.
- El workflow de firma y sus secretos se agregarán solo después de que SignPath Foundation apruebe el proyecto y entregue las identificaciones de proyecto/política y credenciales correspondientes.
- Cada solicitud de firma requerirá la aprobación manual de una persona autorizada, y el artefacto deberá provenir de un workflow verificable en GitHub-hosted runners.
- El mantenedor deberá activar autenticación multifactor y proteger los workflows de compilación/firma.

## Responsables

- Autores, revisores y aprobadores de firma: **por asignar y publicar en el repositorio antes de solicitar la aprobación**.
- Los cambios externos se revisarán antes de fusionarlos. No se admiten binarios de procedencia desconocida.

## Privacidad

La aplicación se ha diseñado como herramienta local para examinar/editar archivos OCC seleccionados por el usuario. No incorpora funciones de telemetría ni envío automático de archivos a otros sistemas; esta afirmación debe verificarse en las revisiones de código de cada versión. Los archivos OCC de usuarios no se incluirán en el repositorio ni en los artefactos de CI.

## Una vez aprobado

Incluir en el README y en la página de descargas el siguiente reconocimiento obligatorio:

> Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

La política de SignPath se consulta en https://signpath.org/terms.
