# Journey Hull Viewer

A small Windows panel for visualizing hulls in Journey: region triggers,
wind boundaries, camera volumes, physical boundaries and other hull types.
Outlines use the game's camera and depth buffer, so scenery can obscure them.

Version: **1.0.0**.

## Use

Keep `JourneyHullViewer.exe` and `journey_hull_viewer.dll` together in any writable
folder. Start Journey normally, open the viewer, then click **Attach to Journey**.

### Debug Cam

- **Start / Stop** starts or stops the debug camera.
  When in Debug Cam mode, press **Fly** to teleport the wayfarer to the camera.
- **Ignore terrain** disables both debug-camera and regular-camera terrain
  collision; unchecking enables both. **Set angle lock** sets debug-camera angle lock.
  These local camera changes are left in place when the panel closes. Use Stop
  or uncheck the options when finished.

### Draw Hulls

- **Show all** displays every active hull within the fixed drawing budget.
- Check categories and click **Show checked types** to filter by type.
- Paste hull names into the text box, then click **Show only these hulls**.
  Names must be hull names, not the names of actions that use those hulls.
  Spaces, commas, semicolons and new lines separate up to 64 names.
  Press Return to add a new line.
- **Hide**, or close the panel, to stop drawing.

For example, in Underground, enter this 32-character hull hash:

```text
8dc1645e2ed9ee33aa75297e01b94d0c
```

Names are case-sensitive and level-specific; surrounding ASCII quotes are
accepted. The status reports active/inactive matches and drawn hulls, and input
errors stay visible. Nothing enables a hull or changes trigger behavior.
Outlines are visible only in your game, not to a companion.

Exit Journey before replacing either file.
Only one viewer panel may control a game process at a time.

## Compatibility

Requires the Windows x64 version of Journey. The viewer checks compatibility
before attaching and does not modify game files on disk.

Other mods that change the same game functions may conflict. Avoid running
another hull renderer at the same time.

Showing many hulls may affect performance. Use type filters or select specific
hulls to reduce the amount drawn.

## Build from source

Requires Python 3.9+ and MinGW-w64 targeting Windows x64. All source dependencies
are included. From this folder:

```sh
python3 scripts/build.py
python3 scripts/build.py --package
```

Set `CC` to your MinGW-w64 GCC executable if necessary. Outputs go to `build/`.
Packaged files go to `dist/candidates/<version>/`.

Optional exact camera-script checks with a local Lua interpreter:
`python3 scripts/test_camera.py /path/to/lua`.

Source layout: `src/panel.c` (panel and attachment), `src/renderer.c` (native hull
drawing), `src/runtime.inc` (startup and game-thread IPC), `src/protocol.h` (bounded
local commands), `src/image_profile.json` (compatibility checks), `tests/` and
`third_party/`. See [THIRD_PARTY.md](THIRD_PARTY.md) for dependency notices.
