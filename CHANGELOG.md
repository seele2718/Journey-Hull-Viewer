# Changelog

## 2.0.0 — 2026-10-18

### Added

- In-game hull picking with Ctrl+I and a Picked Hulls list.
- Adjustable transparent hull face highlighting for picked and named hulls.
- Object inspection by hash, structured properties and formatted raw data.
- Clickable references, timeline trigger links, Back/Forward history and Hull Links.
- Drawing nested clump hull members together.
- Direct reading of installed compressed level tables, without manual extraction.
- Camera coordinates and Move Cam to entered X, Y, Z positions.
- Selectable/copyable hashes and coordinates, plus optional hiding of zero/false properties.
- Application and window icons.

### Improved

- One resizable workspace with drawing controls beside the inspector.
- Immediate type filtering: a hull matches any selected category, including multi-type hulls.
- All type checkboxes start unchecked, matching initially hidden drawing.
- Picked-hull highlighting persists while following object references.
- Debug-camera state refreshes across level changes; Move Cam preserves angle lock.
- More compact controls, responsive footer messages and immediate picked-row selection.

## 1.0.0 — 2026-10-11

Initial release of Journey Hull Viewer.

- Standalone panel for attaching to a running game and viewing hull outlines.
- Show all active hulls, filter by type, or select hulls by name/hash with the
  option to include inactive hulls.
- Debug camera controls for starting/stopping, terrain collision and angle lock.
