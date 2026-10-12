# Estado de retirada

El 12 de octubre de 2026 se solicitó la eliminación de `SW3T0/brisa` mediante `gh repo delete SW3T0/brisa --yes`, después de guardar la referencia de publicación y el bundle local.

GitHub rechazó la operación con HTTP 403: la credencial de GitHub CLI carece del ámbito `delete_repo`. Se inició `gh auth refresh -h github.com -s delete_repo`, pero la autorización pendiente caducó sin completarse. El navegador integrado tampoco tiene una sesión autenticada con acceso a los ajustes del repositorio.

El usuario confirmó después que había eliminado el repositorio desde GitHub. La comprobación posterior con `gh repo view SW3T0/brisa --json url` ya no pudo resolver ese repositorio.

**Retirada completada el 12 de octubre de 2026.** La copia de trabajo local, historial, etiquetas, README y referencia de publicación están conservados. La rama `refactor/readability` no se ha publicado.
