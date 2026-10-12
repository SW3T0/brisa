# Nueva publicación tras la revisión

El 12 de octubre de 2026 el usuario solicitó volver a subir Brisa después de completar la revisión de legibilidad.

Se recrea `SW3T0/brisa` como repositorio público. La rama `main` incorpora la refactorización previamente aislada en `refactor/readability`, sin cambios funcionales. Se conserva el README breve y la captura con apuntes y ejercicios de cálculo.

La versión se mantiene en `v0.3.1`. Puesto que el repositorio remoto anterior fue eliminado, su etiqueta se actualiza localmente para identificar las fuentes revisadas de esta nueva publicación. El commit de la publicación anterior (`0ba3fdc9ae6125166de825a7f770f79a0978f10b`) y la etiqueta original permanecen en el bundle `outputs/brisa-before-readability-review.bundle` y en la referencia de publicación archivada. Las etiquetas `v0.1`, `v0.2` y `v0.3` no se modifican.

La descarga portable se reconstruye con el ejecutable compilado a partir del código revisado, el mismo runtime PDFium, README, captura y licencias. Las pruebas de interfaz y exportación vuelven a pasar antes de la subida.
