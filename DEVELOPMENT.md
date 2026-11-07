# Development

Windhawk loads a mod from one single file, `dynamic-island-for-windows.wh.cpp`.
That file is generated. The source lives in `src/`, split into small files by topic.

## Workflow

1. Edit the files in `src/`.
2. Run `./build.ps1` (PowerShell 5.1 or 7). It writes `dynamic-island-for-windows.wh.cpp`.
3. Paste or load that file in the Windhawk editor to test.
4. Commit both: the changed files in `src/` and the regenerated `.wh.cpp`.

`./build.ps1 -Check` writes nothing and fails if the `.wh.cpp` does not match `src/`.
Run it before a commit or a pull request.

Never edit the `.wh.cpp` by hand: the next build overwrites it.

## How the build works

`src/manifest.txt` lists every part in order. The build only joins the files, so the
compiler sees exactly the same code as if it had been written in one file. Nothing
is reordered or rewritten.

| Path | Content |
| --- | --- |
| `src/header.txt` | `// ==WindhawkMod==` metadata block (id, version, compiler options) |
| `src/readme.md` | Readme shown in Windhawk, plain Markdown |
| `src/settings.yaml` | Mod settings, plain YAML |
| `src/prelude.cpp` | Defines and includes, before the anonymous namespace |
| `src/core/`, `src/system/`, `src/media/`, ... | Code inside the anonymous namespace, by topic |
| `src/render/renderer/` | The `Renderer` class, split across files (see below) |
| `src/entry.cpp` | Windhawk entry points (`Wh_ModInit` and the tool mod launcher) |

The build adds the Windhawk comment wrappers around `readme.md` and `settings.yaml`
and opens and closes the anonymous namespace (`@namespace-begin` / `@namespace-end`).

### Order matters

The parts form one translation unit, so a function must still be declared before it
is used, exactly as in a single file. When you move code between parts, keep that in
mind. A new file must be added to `src/manifest.txt`; the build fails if a file in
`src/` is not listed there.

### The Renderer class

`class Renderer` is about 9,500 lines. It is split across `src/render/renderer/*.cpp`
at member function boundaries: `01-core.cpp` opens the class and `29-members.cpp`
closes it. The files in between are fragments of the class body, so an editor that
opens one of them on its own will show errors. That is expected; the build output
compiles.
