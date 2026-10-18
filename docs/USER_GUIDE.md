# User Guide

[Back to README](../README.md)

## Attach and stop

Keep `JourneyHullViewer.exe` and `journey_hull_viewer.dll` together in a folder
of your choice. It does not have to be Journey's installation folder.
Start Journey and load a level, then double-click `JourneyHullViewer.exe` to
open the viewer and click **Attach to Journey**. The viewer finds the game's
`Data/Scripts` folder automatically.

Use **Hide All** to hide hull drawings without closing the inspector.
Closing the viewer also hides them. Hull drawings and highlights are visible
only to you, not your companion. Drawing and inspecting hulls do not change
collisions or the behavior of triggers and other game objects.

## Draw hulls

### Types

The six checkboxes are **Boundary**, **Physical**, **Wind**, **Camera**,
**Region** and **Internal**. All start unchecked. Checking or unchecking a type
applies immediately; clearing the last type hides the hulls.

A hull matches **any** selected type. For example, a hull that is both Camera
and Physical remains visible when Camera is checked and Physical is not.
**Show All** shows active hulls within the drawing budget; **Hide All** hides them.

### Names

Choose **Names**, enter the 32-character hashes of the hulls you want, and click
**Show These Hulls**. Separate up to 64 hashes with spaces, commas, semicolons
or new lines. Standard copy/paste shortcuts work in the text box.

Hashes are case-sensitive and level-specific. Use a **hull's** hash, not the
hash of a trigger that references it. **Include inactive hulls** also draws
matching inactive hulls that are available in the current runtime snapshot;
it does not activate them or change their gameplay behavior.

The footer reports requested hashes, active/inactive matches and drawn hulls.
A missing match can mean a different level, an unloaded object or an incorrect
hash. Do not assume a file record is currently loaded in the game.

### Tinted hull faces

Enable **Tint Faces** to shade a picked hull or the hulls selected by name.
Enter an opacity from **0 to 100**; higher values make the charcoal faces darker
and less transparent. The default is 25%. General type/all-hull views remain
outlines unless a hull is highlighted.

Outlines and faces follow scene depth, so foreground scenery can obscure them.
If face drawing is unavailable, the footer says so and outlines can still be used.

## Pick a hull

1. Draw some hulls.
2. Enable **Inspect**, or press **Ctrl+I** while Journey or the viewer has focus.
3. Click a hull face in the game. You do not have to hit an outline precisely.
4. Choose a row in **Picked Hulls** to highlight that hull and inspect it.

Overlapping or obscured hulls may also be listed, up to 32 candidates. Picking
does not use a depth-buffer readback, so the first result is not necessarily
the nearest unobscured surface.

While **Inspect** is enabled, the mouse is used for hull selection rather than
normal game controls. Press **Ctrl+I** again to turn Inspect off and restore
normal mouse controls. Escape is not rebound. Keyboard controls remain available,
including remapped controls.

The selected hull stays highlighted while you follow references, open other
hashes, use Back/Forward, reload data or toggle Inspect. A new picked hull or an
explicit drawing action changes the highlight; changing levels clears it.

## Inspect objects and references

Enter an object's 32-character hash in the box below the inspector heading and
press **Open** or Enter. This accepts any object type in the loaded level data,
not just hulls. Use the view selector to switch between:

| View | What it shows |
| --- | --- |
| Properties | Expandable source fields, with true/non-zero scalar values emphasized. |
| Raw Data | A formatted representation of the full record, with selectable text. |
| Referenced By | Objects whose file data references the current object. |
| References | Objects referenced by the current object's file data. |
| Hull Links | Paths to hulls and upstream region sources through supported references. |

Double-click a linked property or reference row to inspect that object.
Timeline **TriggerName** fields are links too. Nothing executes a trigger.
Use the arrow buttons to go **Back** or **Forward** through inspection history.

**Hide zero / false** removes zero/false scalar rows from Properties. Other
fields remain available, and Raw Data always retains the full record. Bold
values are an aid to reading, not an indication of live activity or changed defaults.

### Find a trigger's hulls

Open a trigger and choose **Hull Links**. The tree shows supported paths through
references and clump membership; double-click a linked entry to follow it.
Arrows indicate reference direction. These paths describe authored relationships,
not whether the trigger's conditions will be met or an event will execute.

On a hull, **Show This Hull** isolates it. On a clump, **Show Hulls (N)** draws
its hull members, including nested clumps, with face tint when enabled.
Incomplete or over-limit results are reported rather than drawn as a complete set.
Link searches stop at 12 links and 64 distinct hulls per section.

### Copy hashes and coordinates

- Click a picked row and press **Ctrl+C** to copy its hash, or use **Copy Hash**
  for the currently inspected object.
- Drag across a property, reference or picked-hash row to select a substring.
  Press **Ctrl+C** to copy the selected text.
- Selecting a property/reference row and pressing **Ctrl+C** also copies its
  value directly. A single referenced hash is copied without its label.
- Coordinate triples can be copied into **Input x,y,z:** beside the camera position.
- The summary and Raw Data text are selectable too.

## Debug Cam

**Start** enables the game's debug camera; **Stop** disables it. The panel checks
its state periodically and again after level changes. Movement uses the game's
camera controls rather than a separate set of viewer key bindings.

The **Camera** row shows the rendered camera position in both normal and Debug
Cam modes. To move the debug camera to a position:

1. Start Debug Cam.
2. Enter three finite numbers in **Input x,y,z:**, for example `316.0, 71.7, 128.9`.
3. Click **Move Cam** or press Enter. The footer reports success or an input/error message.

Move Cam preserves orientation and the angle-lock setting. It moves the camera,
not the wayfarer. The button is disabled when Debug Cam is off.

In Debug Cam mode, press Fly to teleport your wayfarer to the camera.
Your companion can see this too, so please be considerate.

**Ignore terrain** disables terrain collision for both the normal and debug
cameras. **Set angle lock** controls the debug camera's angle lock. These settings
are left in place when the viewer closes; restore the options and use Stop
before closing if you want normal camera behavior.

## Where inspector data comes from

The viewer reads the attached game's `Data/Scripts` folder, or a `Data` folder
beside the viewer EXE. Original compressed `__…lua.gz` tables are preferred;
loose tables are a fallback. No manual extraction is required.

**Source Information** identifies where a record came from. **Game File
Properties** contains authored fields, not live creature state or proof of
which script the running game loaded. **Runtime State** is captured when a hull
is picked and may become stale afterwards.

**Reload Data** rereads the files; it does not reload the level or execute game
scripts. Tables are parsed as data. Unsupported or malformed tables are reported,
not executed. If file lookup is unavailable, picked runtime hashes still work.
An optional generated `inspection` folder is an advanced fallback, labelled
separately from installed game files; ordinary use does not require it.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Nothing is drawn | Check a type or use Show All. Make sure a level is loaded and the viewer is attached. |
| A named hull is missing | Check the level and exact hash. Try Include inactive hulls; unloaded hulls may still be unavailable. |
| Faces are hard to see | Increase opacity or isolate fewer hulls. Read the footer for face-rendering availability. |
| Properties are unavailable | Keep the original Data folder intact, attach to Journey, then use Reload Data. |
| Attach reports a conflict | Close another viewer or overlapping renderer. Restart Journey after changing viewer versions. |
| Move Cam is disabled | Start Debug Cam and wait for the state to update. |
| The window is cramped | Resize it or drag the divider. Smaller windows can scroll without shrinking the text. |

Compatibility failures are intentional safeguards. Do not bypass them or mix
EXE/DLL versions to force attachment.
