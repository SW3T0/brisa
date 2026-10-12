These checks use a Windows `BRISA_TEST` build; test hooks are excluded from releases.

1. Compile using the command from `build.ps1`, adding `-DBRISA_TEST` and changing the output to `work/Brisa-03-test.exe`. Copy `pdfium.dll` into `work`.
2. Run `python tests/create_fixtures.py`, then copy the resulting PDFs into `work` (rename a copy of `estudio ñ.pdf` to `segundo.pdf`).
3. Copy `tests/workspace-test.ps1` into `work`, then run it from the repository directory. It checks workspace navigation, highlighting, undo, autosave/reopen, screenshots and asynchronous PDF export.
4. Install the test-only dependency with `python -m pip install pymupdf`, then run `python tests/export_test.py work/Brisa-03-test.exe work/export-tests`.

Export checks use MuPDF as an independent renderer, cover all page rotations, retain existing annotations and verify that originals and existing destinations survive failed saves.

Formatting tools are optional development tools, not application dependencies:

```powershell
clang-format -i src/main.cpp
python -m black tests/create_fixtures.py tests/export_test.py
Import-Module PSScriptAnalyzer
Invoke-Formatter -ScriptDefinition (Get-Content tests/workspace-test.ps1 -Raw) -Settings .PSScriptAnalyzerSettings.psd1
```

The last command returns formatted text; it does not overwrite the script. Preserve UTF-8 with BOM when saving PowerShell files containing Spanish text.
