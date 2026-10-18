# Building and Testing

[Back to README](../README.md)

## Build

Requires Python 3.9+ and MinGW-w64 GCC and windres targeting Windows x64.
Dependencies are vendored in `third_party`; no game files are needed to compile.

From the project folder:

```sh
python3 scripts/build.py
```

Set `CC` and `WINDRES` if your compiler/resource compiler have different names.
The executable, DLL and synthetic test programs are written to `build/`.
The icon assets are already included; regenerating them is not needed to build.
The optional `scripts/build_icon.sh` helper regenerates the PNG and ICO assets
with FFmpeg. It is not run by the normal build or included in the binary package.

To assemble a versioned candidate with documentation and license notices:

```sh
python3 scripts/build.py --package
```

The destination is `dist/candidates/<VERSION>/`. Existing candidates are never
overwritten. `manifest.json` records source and artifact hashes. Building or
packaging does not install, publish or attach the viewer to Journey.

## Test

After building, run the synthetic fixtures on Windows:

```sh
python scripts/test.py
```

The runner exercises parsing, picking math, input handling, rendering, face
compositing and native panel controls. Attachment tests use a synthetic target,
not Journey. Graphics tests require a working Direct3D 11 device. Test logs go
to `build/test-results/`; normal viewer use does not create these logs.

Optional tests with your own installed game data:

```sh
python scripts/test.py --data-root "C:/path/to/Journey/Data"
```

This additionally checks installed level-table parsing and reference navigation
against the game's data. It reads those files without modifying them or executing
their scripts. Temporary inspection indexes are removed after testing; do not
redistribute generated indexes. The default suite explicitly skips this optional
data-dependent navigation check. A separate `panel_data_tests.exe` requires
a `Data` folder beside that test executable and
is intended for manual fixture setup; it is not required by the default suite.

Exact camera-script checks can also run with a Lua interpreter:

```sh
python3 scripts/test_camera.py /path/to/lua
```

Synthetic checks do not replace in-game checks of appearance, controls,
compatibility and performance.

## Optional offline inspection index

Ordinary viewer use reads installed compressed level tables directly. Developers
can instead generate an offline index from their own extracted Data folder:

```sh
python3 scripts/make_index.py --data-root /path/to/Data --output build/inspection
```

The generator parses data without executing Lua. Keep game data and generated
indexes local; do not redistribute them with source or release packages.
Default packaging does not include an inspection index.

## Source layout

- `src/panel.c`, `panel_style.h`, `inspector_*.h`: UI, attachment and inspection.
- `src/renderer.c`, `runtime.inc`: rendering and game-thread communication.
- `src/camera.h`, `inspect_input.h`: fixed camera actions and Inspect input.
- `src/protocol.h`: bounded local IPC shared by the EXE and DLL.
- `src/game_data.h`, `lua_data.h`, `gzip_data.h`: installed-data parsing.
- `src/image_profile.json`: localized compatibility checks.
- `tests/`: synthetic fixtures; `third_party/`: dependency sources and notices.

Retain third-party copyright and license notices. Do not include game executables,
extracted game files, machine-specific paths or generated test output in releases.
