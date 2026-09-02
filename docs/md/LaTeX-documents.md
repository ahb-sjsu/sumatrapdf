# LaTeX documents

**Available in [pre-release 3.7](https://www.sumatrapdfreader.org/prerelease)**

SumatraPDF opens LaTeX sources (`.tex`, `.ltx`, `.latex`) as the typeset paper. It compiles the file with your TeX distribution and shows the resulting PDF, the same way PostScript files are shown through Ghostscript.

## Requirements

A TeX distribution with `pdflatex.exe`: [MiKTeX](https://miktex.org/) or [TeX Live](https://tug.org/texlive/). SumatraPDF looks, in order, in:

- the directory named by the `SUMATRAPDF_TEX_BIN` environment variable
- `%PATH%`
- the default MiKTeX locations (`%LOCALAPPDATA%\Programs\MiKTeX\miktex\bin\x64`, `%ProgramFiles%\MiKTeX\miktex\bin\x64`)
- the newest `C:\texlive\<year>\bin\windows`

Without a TeX distribution, `.tex` files open as plain text.

## What happens on open

1. The root file is found: a `% !TEX root = main.tex` comment wins; otherwise the file itself if it contains `\documentclass`; otherwise the sibling `.tex` that has `\documentclass` and `\input`s or `\include`s the opened file.
2. `pdflatex` runs in the root's directory with `-interaction=nonstopmode -halt-on-error -synctex=1`. A `% !TEX program = xelatex` (or `lualatex`, TeXShop's `% !TEX TS-program =` also works) comment picks the engine; a document that loads `fontspec` gets `xelatex`. Other program names are ignored.
3. If the `.aux` cites anything, `bibtex` runs (or `biber` when biblatex left a `.bcf`), then `pdflatex` again.
4. `pdflatex` reruns while the log asks for it (cross-references, table of contents), up to four passes in total.

The `.aux`, `.log`, `.synctex.gz` and `.pdf` files land next to the root, as with a normal command-line build. If that directory is read-only they go to `%TEMP%\SumatraPDF-tex\<id>` instead.

Loading happens in the background; the window stays responsive while LaTeX runs. Each tool gets 5 minutes; `SUMATRAPDF_TEX_TIMEOUT_SECS` changes that (`0` removes the limit).

## Editing loop

Saving the `.tex` you opened re-compiles and refreshes the view, like any other file SumatraPDF watches. Double-clicking the page jumps to the source line through the generated SyncTeX file when an [inverse search command](LaTeX-integration.md) is configured.

## Errors

When LaTeX fails the tab shows the first error from the `.log` (for example `./paper.tex:12: Undefined control sequence.`) under "Error loading". Fix the source and save; the next reload recompiles.

## Related files

`.bib`, `.sty`, `.cls` and `.bst` files open as plain text.
