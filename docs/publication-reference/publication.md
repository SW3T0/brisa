# Referencia de la primera publicación de Brisa

Esta carpeta conserva la presentación y estructura usadas en GitHub antes de la revisión de legibilidad. No constituye una propuesta de volver a publicar el código.

- Repositorio: `SW3T0/brisa`, público, rama principal `main`.
- Descripción: “A minimal native PDF reader written in C++, featuring annotations, text highlighting and side-by-side document viewing.”
- Primera versión oficial: `v0.3.1`. Las etiquetas `v0.1`, `v0.2` y `v0.3` conservaron las iteraciones anteriores. `v0.1` está expresamente identificada como reconstruida; las fuentes originales de esa iteración habían sido sustituidas.
- El README se publicó en inglés, con descripción breve, captura, atajos, comportamiento de guardado, dependencias, compilación y licencias. Incluía “Personal project maintained as needed.” y una mención de la asistencia de un agente de IA.
- Código propio con licencia MIT; avisos de PDFium y sus dependencias en `licenses/`.
- Plataforma: Windows x64, C++17, Win32/GDI/GDI+ y PDFium. Compilación con Zig 0.15.2 o CMake/Visual Studio.

## Estructura publicada

```text
src/main.cpp
tests/                       # pruebas de interfaz y exportación; generador de PDFs
docs/screenshot.png
vendor/pdfium/include/       # cabeceras; DLL y biblioteca de enlace no versionadas
licenses/                    # licencias de terceros
README.md
LICENSE
.gitignore
.gitattributes
build.ps1
fetch-deps.ps1
CMakeLists.txt
```

`files.txt` contiene el inventario exacto. `README.md` y `screenshot.png` son copias de la presentación publicada tras actualizar la captura. `repository.json`, `release-v0.3.1.json` e `history.txt` conservan los metadatos y el historial de publicación.

## Procedimiento usado

Después de compilar y ejecutar las pruebas, se preparó un ZIP portable con `Brisa.exe`, `pdfium.dll`, README, captura y todas las licencias. La descarga de PDFium quedó fijada a `chromium/8076`, con comprobación SHA-256 en `fetch-deps.ps1`.

```powershell
gh repo create SW3T0/brisa --public --source . --remote origin --description 'A minimal native PDF reader written in C++, featuring annotations, text highlighting and side-by-side document viewing.'
git push -u origin main
git push origin --tags
gh release create v0.3.1 outputs/Brisa-0.3.1-win-x64.zip --repo SW3T0/brisa --title 'Brisa v0.3.1' --notes-file work/release-notes.md --verify-tag --latest
```

La captura del README se actualizó después mediante un commit independiente, sin modificar etiquetas ni crear otra versión.

## Copias locales

Antes de solicitar la eliminación del repositorio remoto se guardó todo el historial y las etiquetas en `outputs/brisa-before-readability-review.bundle`. La copia de trabajo y su repositorio Git local se conservan. El ZIP de la primera publicación permanece en `outputs/Brisa-0.3.1-win-x64.zip`.
