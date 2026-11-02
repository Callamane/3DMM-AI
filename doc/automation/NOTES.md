# 3DMMEx automation bridge — engineering notes

These notes record what I found while reverse-engineering the engine for the
automation bridge (`src/automation/`) and the MCP server (`tools/3dmm-mcp/`).
They are written for someone who knows C++ but not Kauai. Code is the source of
truth; line numbers drift, so search for the function names.

Status legend: **verified** = confirmed by running code; **read** = from reading
source only.

---

## 1. Kauai in five minutes

Kauai is Microsoft's 1990s app framework that 3DMM is built on (`kauai/src`).
Things that are unusual:

* **Hungarian names everywhere.** `p` = pointer, `pv` = void*, `f` = bool, `lw`
  = int32, `stn` = string object, `fni` = file name, `rc` = rect, `cno` =
  chunk number, `ctg` = chunk tag (FourCC), `arid` = actor id, `nfrm` = frame
  number, `brs` = BRender scalar (float), `bra` = BRender angle. A function
  named `FXxx` returns bool (success), `PxxxNew` is a factory, `Cxxx` is a
  count, `Ixxx` an index.
* **Ref counting**, not RAII: objects derive from `BASE`; `AddRef()` /
  `Release()`; `ReleasePpo(&p)` releases and nulls. Factories return a +1 ref.
* **Errors are pushed, not returned.** Functions return `fFalse` and push an
  error code (`PushErc(ercXxx)`) onto the global error stack `vpers`.
  `APP::FCmdIdle` → `DisplayErrors()` later pops them and shows a *modal alert
  dialog*. The bridge must drain `vpers` after each command (it does) or the UI
  will block on a dialog.
* **Command queue** (`CEX`, global `vpcex`): UI events become `CMD`s (`cid` +
  4 `int32` args) dispatched to command handlers (`CMH`) via `CMD_MAP`s.
  `vpcex->EnqueueCid(...)` posts asynchronously.
* **Chunky files** (`CFL`): every content file (`.3cn`, `.3th`, `.chk`, `.3mm`)
  is a tree of chunks addressed by `(ctg, cno)` with parent/child links keyed by
  `chid`. A `.3mm` movie is a chunky file.
* **GOBs** are the widget tree; `DDG` is a document view; `DOCB` a document.
  `MVIE` is a `DOCB`, `MVU` (movie view) is its `DDG`.

## 2. Object model and ownership (read)

```
APP (vapp, utest.cpp)                 one per process
 ├─ KWA kidspace world (the "building": lobby, theatre, studio chrome scripts)
 └─ STDIO _pstdio (studio.cpp)        exists only while the Studio is up
     └─ MVIE _pmvie (movie.cpp)       the open movie (DOCB)
         ├─ MVU  (view, child GOB of the studio; PddgGet(0)/PddgActive())
         │   └─ TBXG children        one GOB per visible text box
         ├─ BWLD _pbwld               BRender world: camera, z-buffers, 8-bit RGB buffer
         ├─ MSQ  _pmsq                sound queue
         ├─ GST _pgstmactr            roll call: every actor in the movie (arid, name, template)
         └─ SCEN _pscenOpen           ONLY the current scene is in memory
             ├─ BKGD _pbkgd           background set + current camera (icam)
             ├─ GL<ACTR*> _pglpactr   actors in the scene
             │    └─ ACTR → TMPL (template: body, actions, costumes), BODY, route (GL<RPT>), events (GG<AEV>)
             ├─ GL<TBOX*> _pglptbox   text boxes (TBOX is a rich-text doc, TBXG its view)
             └─ GG<SEV> _pggsevFrm/_pggsevStart   scene events: camera changes, sounds, pauses
```

`MVIE::FSwitchScen(i)` writes the current scene back to the movie's temp file
and reads scene `i`. `Pscen()` is the open scene. Actors are owned by the scene;
`arid`s are movie-unique and persistent.

## 3. Boot and the shortest path into the Studio (read + verified)

`APP::_FInit` (utest.cpp): OS/audio/video checks → `_ParseCommandLine` →
product names / tag manager → `_FInitKidworld` → home logo → splash →
`GetPortfolioDoc(&fni)`:

* no document → `_FInitBuilding()` runs the building script (logon,
  lobby, intro videos). Getting to the Studio from there takes clicks (scripts in
  `src/building/*.cht`, `src/shared/util.cht` enqueue `cidLoadStudio`).
* a document on the command line → `_FInitStudio(&fni)` → straight into the
  Studio. **Verified**: `3D Movie Maker bongo.3mm` lands in the Studio.

`_FInitStudio(&fniNil)` with an empty `FNI` creates a Studio with a **new,
empty movie**. That is the no-click path the bridge uses: when automation is
enabled and no document was given, the app goes straight to the Studio with a
new movie instead of the building (one `if` in `_FInit`).

At runtime: `STDIO::FLoadMovie(pfni)` (public) opens a file, or creates a new
movie when `pfni == pvNil`. It calls `MVU::FCloseDoc`, which shows a modal
"save changes?" query if the movie is dirty, so the bridge refuses with an error
unless the caller passes `discard_changes` (then it clears both dirty flags:
`MVIE::SetDirty(false)` and `DOCB::SetDirty(false)`).

Fullscreen: 3DMMEx starts fullscreen unless `switchresolution = "0"` is in
`~/Library/Application Support/3DMMEx/3dmovie.ini`. Automation mode forces a
window for that session only (hook at the top of
`APP::_FEnsureDisplayResolution`); the user's preference is not written.

## 4. Content identification and catalog (read)

Content is addressed by `TAG { sid, pcrf, ctg, cno }`:

* `sid` – source id (product). `vapp.SidProduct()` for shipped 3DMM content;
  `ksidUseCrf` means "inside this movie file" (`pcrf` then points at it) — used
  for imported user sounds.
* `ctg` – chunk type: `kctgBkgd` background, `kctgCam` camera (child of BKGD,
  addressed by `chid` = camera index), `kctgTmpl` actor/prop template, `kctgTdf`
  3D font, `kctgMsnd` sound, `kctgMtrl` material.
* `cno` – chunk number within the content files.

The in-app browsers do **not** list `.3cn` content directly; they list
*thumbnail descriptor* files (`.3th`, ftg `kftgThumbDesc`). Each `.3th` holds
"thumbnail parent" chunks (`kctgBkth`, `kctgTmth`, `kctgPrth`, `kctgSfth`,
`kctgSvth`, `kctgSmth`, `kctgTfth`, `kctgMtth`, …) whose data is a `TFC`
pointing at the real content `(ctg, cno)` and whose name is the display name.
`BCLS::PbclsNew(pcrm=nil, &cki{kctgXxth, cnoNil}, ctgNil)` returns
`GL<THD>` + `GST` names — exactly what the browsers show. Cameras use the
descending form `cki{kctgBkth, cnoBkgd}, ctgContent=kctgCath`; the THD `chid`
is the camera index. The Splot machine (`splot.cpp`) is a good reference for
programmatic use of all of this.

| Catalog | thumb ctg | content tag |
|---|---|---|
| backgrounds | `BKTH` | `BKGD` cno |
| cameras of a background | `BKTH`/cno → `CATH` | `CAM ` chid = index |
| actors | `TMTH` | `TMPL` cno |
| props | `PRTH` | `TMPL` cno |
| sound effects / speech / music | `SFTH` / `SVTH` / `SMTH` | `MSND` cno |
| 3D word fonts | `TFTH` | `TDF ` cno |
| materials (costume paint) | `MTTH` | `MTRL` cno |

Per-template data comes from the loaded `TMPL` (`vptagm->PbacoFetch(&tag,
TMPL::FReadTmpl)`): `Cactn()`, `FGetActnName(anid)`, `FGetCcelActn(anid)`,
`FGetDwrActnCel(anid, cel)` (step distance per cel), body-part sets
(`BODY::Cbset()`), custom costumes per set (`CcmidOfBset(ibset)`,
`CmidOfBset(ibset, i)`), accessory sets (`FIbsetAccOfIbset`).

3D words (TDT) are actors whose template is generated from text: `MVIE::FInsTdt(
stn, tdts, tagTdf)`. Shapes are the `tdts` enum in `inc/tdt.h` (normal, arch,
circle, …); fonts come from the `TFTH` catalog.

Sound types (`sty` in `inc/msnd.h`): `stySfx`, `stySpeech`, `styMidi`.

## 5. Coordinates, units, frames (verified)

* **BRender is built fixed point here** (`BASED_FIXED=1` in the BRender
  CMake): `BRS` is 16.16 fixed (`int32`), `BRA` is a 16-bit fraction of a
  turn. Convert with `BrScalarToFloat` / `BrFloatToScalar` /
  `BrAngleToDegree` / `BrDegreeToAngle` (the bridge wraps these as
  `DFromBrs`, `BrsFromD`, `DegFromBra`, `BraFromDeg` in `bridge.h`).
* BRender world space, right-handed, **Y up**, floor is **y = 0**.
  `BR_SCALAR(10.0)` is commented as "1.0 meters" in `ACTR::_GetNewOrigin`;
  consistent with actors measuring ~18–20 units tall.
* **Engine bug:** `ACTR::GetXyzWorld` returns `xyz.dzr + _dxyzRte.dyr` for z
  (should be `.dzr`). It is only used for mouse-drag scaling in `MVU`, so it
  is left alone; the bridge reads `BODY::GetPosition` (the root model's
  translation) instead.
* The camera matrix (`BWLD::GetCamera(&bmat34, &hither, &yon, &fov)`) is
  camera→world; rows 0/1/2 are the camera X/Y/Z axes in world space, row 3 is
  the camera position. BRender cameras look down **−Z**.
* New actors appear at the background's authored *placement point* for the
  current camera (`BKGD::GetActorPlacePoint`, which cycles through several
  points so successive actors don't overlap) and face the camera
  (`BKGD::BraRotYCamera`).
* Time: the movie runs at **6 frames per second** (`kfps` in `socdef.h`).
  Frames are integers; a scene's range is `[NfrmFirst, NfrmLast]` and grows as
  actors' routes get longer.

### Bridge "stage" coordinates (design)

So an AI never has to reason about BRender matrices, the bridge defines a
per-camera *stage frame* on the floor:

* origin = the camera's primary authored placement point (where 3DMM drops new
  actors; usually centre stage), projected to y = 0;
* **+x** = screen right, **+z** = away from the camera (into the picture), both
  being the camera axes projected onto the floor and normalised;
* units = **metres** (1 stage unit = 10 world units);
* `y` (optional) = height above the floor in metres.

Every response that includes a position gives both `stage` and `world`
coordinates; every command that takes a position accepts either
`{"x":..,"z":..}` (stage) or `{"world":[x,y,z]}`. Responses also include the
projected screen position (`screen: {x,y}` in movie-view pixels, plus
`in_view`) and `describe_scene` reports the approximate visible floor area so an
agent can keep actors in shot.

## 6. Paths: how motion is recorded and replayed (read)

An actor has a **route** `GL<RPT>` (points `xyz` + `dwr` distance to the next
point; `dwr == 0` ends a subroute) and an **event list** `GG<AEV>`; each event
has a type (`aetAdd`, `aetActn`, `aetStep`, `aetFreeze`, `aetRotF`, `aetSnd`, …)
and an `RTEL` location (`irpt` = preceding route point, `dwrOffset` = distance
past it, `dnfrm` = frames spent stalled there). Playback walks the route at the
current action's per-cel step size (`TMPL::FGetDwrActnCel`), one cel per frame.
So **speed is a property of the action** (walk vs run), scaled by the actor's
size; a route is pure geometry plus events.

Mouse recording (`MVU::_MouseDown/_MouseDrag/_MouseUp`, toolRecordSameAction):

1. `ACTR::FSetAction(anid, celn, fFreeze, &pactrRestore)` — creates the undo
   object (an `AUND` holding a dup of the actor) and a restore copy.
2. `SetTsInsert(ts)`, then once the drag is long enough `FBeginRecord(ts,
   fReplace=true, pactrRestore)` and `DeleteFwdCore(false)` (drop the rest of
   the old route).
3. Each mouse sample: `FRecordMove(dxr, dyr, dzr, grfmaf, ts, …)`. `d*r` is the
   world-space vector from the **last route point** to the mouse. It is
   smoothed with the previous sample (`(prev + 2·cur)/3`). If its length ≥ the
   current cel's step, one route point is appended exactly one step along that
   direction and the scene advances one frame. If shorter and ≥ 0.75 s
   (`kdtsThreshRte`) have elapsed since the last point, a *stall* frame is
   recorded instead (step size 0, the actor animates in place) — this is how
   you make an actor talk/wave on the spot for N frames. `fmafGround` clamps to
   the floor.
4. `FEndRecord(fReplace=true, pactrRestore)` ends the subroute with a freeze
   and step 0, so the actor **stops** at the end of the path.

The timestamps only gate the two thresholds above; they are not converted to
frames. The bridge therefore drives exactly this API with synthetic
timestamps: for each waypoint it repeatedly passes `target − current` (so each
call yields one step toward the target), and for "in place" frames it passes a
zero vector with a timestamp 1 s later. The result is byte-for-byte the same
kind of route the mouse produces, so playback in stock 3DMM is correct.
`FMoveRoute` (used by the place tool) translates an existing subroute; the
bridge uses it to position a newly added actor.

## 7. Undo (read; verified)

**3D Movie Maker keeps exactly one level of undo** (`MVIE` calls
`SetCundbMax(1)`, and `TBOX` asserts it). `undo` therefore undoes the last
bridge edit only. Saving clears it (stock behaviour).


`DOCB` keeps a `GL<UNDB*>`; `MVIE::FAddUndo(pmunb)` stamps the scene index and
frame, `DOCB::FUndo()/FRedo()` replay. Every non-`Core` engine edit creates its
own undo object (`MVIE::FRotateActr`, `FScaleActr`, `FChangeCam`, `FAddScen`,
`SCEN::FAddTbox`, `ACTR::FSetAction`, …). Two operations don't:

* `MVIE::FInsActr` — the *view* creates a `SUNA(utAdd)` on mouse-up after
  placement. The bridge creates the same `SUNA` after positioning the actor.
* Path recording — covered by the `AUND` from `FSetAction` (one Ctrl‑Z undoes
  the whole path, as with the mouse).

The bridge exposes `undo`/`redo` and every bridge edit is undoable from the UI.

## 8. Sounds (read)

* Built-in: `TAG{sid=product, ctg=MSND, cno}`. Scene ("background") sound:
  `MVIE::FAddBkgdSnd(tag, fLoop, fQueue, vlm, sty)` at the current frame.
  Actor sound: select the actor, `MVIE::FAddActrSnd(tag, fLoop, fQueue,
  fActnCel, vlm, sty)` (`fActnCel` = "motion match": play on this cel of the
  action every cycle).
* Import: `MVIE::FCopySndFileToMvie(pfil, sty, &cno, &stn)` copies a WAV/MP3
  (or MIDI for music) into the movie as an `MSND` chunk; the tag is then
  `TAG{sid=ksidUseCrf, pcrf=<movie autosave CRF from FEnsureAutosave>, ctg=MSND,
  cno}` (same as `BRWM`/`esl.cpp`). Unused imported sounds are garbage-collected
  on close.
* No lip sync: 3DMM has no phoneme/lip movement. "Talking" is just an action
  (e.g. *talk*, *yell*) played while a speech sound plays. The bridge's
  `attach_sound` + `animate` (in-place action for N frames) is the equivalent.

## 9. Capturing a composited frame without DEBUG (verified)

`MVIE`'s DEBUG frame dump (`FWriteBmps`, `BWLD::FWriteBmp`) writes only the
BRender working buffer, i.e. **no text boxes**. Text boxes are `TBXG` GOBs,
children of the `MVU`. The bridge instead creates an 8-bit offscreen `GPT`
(`GPT::PgptNewOffscreen`) the size of the `MVU` and calls
`MVU::DrawTree(pgpt, …)`, which runs `MVU::Draw` (render + blit the 3D world)
then draws every child text box on top. The pixels plus the current palette
(`GPT::PglclrGetPalette`) are written as an indexed PNG by a tiny built-in
encoder (no new dependencies). Selection highlights are removed first
(`SelectActr(nil)`, tbox deselect) and restored afterwards.

## 10. Main-thread tick (verified)

`APPB::_Loop`: dispatch queued commands; otherwise `SDL_WaitEventTimeout(1 ms)`;
if nothing arrived, enqueue `cidIdle`. So `APP::FCmdIdle` runs roughly every
millisecond whenever the app is not busy, **including during playback**
(playback is clock-alarm driven, `MVIE::FCmdAlarm` → `FCmdRender`, and the
queue drains between frames). The bridge uses non-blocking sockets polled from
`APP::FCmdIdle` — no threads, no locking, works the same on Win32 and SDL.
A re-entrancy guard prevents nested polling when an engine call runs a nested
modal loop. (`SDLEnqueueCmd` exists as a thread-safe way to post commands if a
listener thread is ever needed.)

## 11. Design

### Process layout

```
MCP client ──stdio──> 3dmm-mcp (Rust, rmcp) ──TCP 127.0.0.1:7341──> bridge in 3DMMEx
```

### Enabling

* CMake option `3DMM_AUTOMATION` (default ON) compiles `src/automation/`.
* Runtime: off unless `--automation[=PORT]` is on the command line or the
  env var `THREEDMM_AUTOMATION=PORT` (or `1` for the default 7341) is set.
* Binds `127.0.0.1` only. One client at a time; a second connection is refused
  with an error line.

### Protocol

Newline-delimited JSON (UTF-8), one request per line:

```
→ {"id": 1, "cmd": "add_actor", "args": {"template": "Ron", "position": {"x": -1, "z": 0}}}
← {"id": 1, "ok": true, "result": {...}}
← {"id": 2, "ok": false, "error": {"code": "not_found", "message": "no actor template named 'Rob'; did you mean 'Ron'?"}}
```

`hello` returns `{"protocol": 1, "app": "3DMMEx", "version": ...}`.
Long operations (`play` with `wait`) reply when finished; the bridge processes
requests strictly in order.

### Commands

Session: `hello`, `get_state`, `goto_studio`, `new_movie`, `open_movie`,
`save_movie`, `undo`, `redo`.
Catalog: `list_backgrounds`, `list_cameras`, `list_templates`, `list_actions`,
`list_costumes`, `list_sounds`, `list_3d_word_styles`.
Scenes: `add_scene`, `select_scene`, `set_camera`, `set_transition`,
`delete_scene`, `goto_frame`, `add_pause`.
Actors: `add_actor`, `add_3d_word`, `select_actor`, `remove_actor`,
`set_action`, `move_actor`, `animate`, `place_actor`, `rotate_actor`,
`scale_actor`, `set_costume`, `set_name`, `actor_lifetime`.
Text & sound: `add_text`, `import_sound`, `attach_sound`.
Observe: `describe_scene`, `render_frame`.
Playback: `play`, `stop`, `export_frames`.

IDs: content IDs are strings `"<kind>:<sid>:<cno>"` (e.g. `"bkgd:2:4097"`);
every command that takes content also accepts the display name
(case-insensitive). Actors are referenced by `arid` (int) or name.

## 12. Verification log

* Stock build: `cmake --preset arm64-macos-debug` + `--target install`
  builds unchanged on macOS (M-series, Apple clang 21); 19/19 tests pass.
  Two environment issues, neither needing code changes: vcpkg's autotools
  ports (gperf) fail when the checkout path contains a space, and
  `brew install ninja autoconf autoconf-archive automake libtool` is needed.
* `3D Movie Maker <file.3mm>` goes straight to the Studio (screenshot).
* Bridge commands exercised from a client: catalog, scenes, actors, paths,
  3D words, costumes, scale, rotation (facing left/right confirmed visually),
  text boxes, imported WAV, built-in sounds, undo, save, reopen, play,
  render, export.
* Projection check: for an actor placed at stage (1, 1) the bridge's
  computed screen point (309, 207) matches the engine's own render bounds
  (x 275–355, bottom 216).
* Frame capture includes text boxes (rendered caption visible in PNG).
* `tools/3dmm-mcp/tests/end_to_end.rs` (two scenes: walk + caption + camera
  cut; two actors with different actions, scene music, imported WAV on an
  actor; save, reopen, play) passes against the debug build with asserts on.
* `tools/3dmm-mcp/tests/mcp_script.rs` starts the real MCP server binary and
  talks JSON-RPC to it like an AI client: a bad script is refused before
  anything is built, then a scripted scene comes back with lines in order
  and not overlapping, distinct voices, a shot list, a review, and the
  closing pause held. Both are ignored by default; run them with
  `cargo test -- --include-ignored` once a build is installed in dist/.
* The saved movie opens and plays in a **stock, unmodified** 3DMMEx built
  from the base commit (`4c1fa94`), launched with its existing `-A`
  (autoplay) switch: caption, walk, camera cut, dissolve and scene 2 all
  play; the app exits normally at the end.

### Making a movie through MCP (Phase 3)

The plan was to give a headless `claude -p` session only this MCP server
and the request "make me a 30-second movie of two guys arguing in a diner".
That run could not start: the local Claude Code CLI's OAuth session had
expired (needs `claude /login` by the user). As the closest substitute, the
same request was carried out by driving the MCP server over stdio using only
the tool descriptions, rendering frames to check work. Result: a 2-scene,
175-frame (29 s) movie with a title, an entrance walk, four synthesized
lines with Yell/Talk acting, a camera cut, a dissolve, an exit walk and
"THE END". It plays correctly in the stock build. Friction found and fixed
on the way:

* Background names are opaque ("MY-Rest Background") → descriptions added,
  `filter` matches them ("diner").
* "Walk" is ambiguous/absent for most actors → action matching ignores the
  `*` marker and ambiguous names return the candidates; guide examples use
  real names.
* Some actors can't talk (e.g. Henry has 11 actions) → `list_templates`
  says to check `list_actions` before casting.
* "Bino" didn't find "Bino BeGood" → prefix/substring actor names.
* Dialogue needed audio files the model couldn't make → `speak_line`.
* No way to see total length for "30 seconds" → `get_state all_scenes`.
* Screen bounds lagged one render behind → render before describing.
* **A saved file was incomplete if the app was killed right after saving**
  (header still in a stdio buffer) → `save_movie` flushes.
* Text box creation asserted when the Studio's text tools had never been
  used (font size 0) → engine defaults, 20 px captions.
* Per-actor recordings replace later actions (stock semantics) → stated in
  `animate`/`speak_line`/`move_actor` descriptions.

### Second round: making it easier for models

* `render_frame overlay=true`: the stage grid is drawn into the captured
  8-bit frame (`image8.cpp`: Bresenham lines, a 3x5 bitmap font, palette
  colours chosen by nearest match) by projecting floor points through the
  same camera model `describe_scene` uses; labels avoid each other.
* `preview_cameras`: `BKGD::FSetCamera` on the scene's background (or a
  background fetched just for the preview, with its own palette merged in
  from `BKGD::FGetPalette`), world-only capture, 2x downscale, tiled. The
  scene's camera is restored afterwards; no scene events are created.
* `describe_timeline`: steps `SCEN::FGotoFrm` over the whole scene, sampling
  each actor's on-stage state, action, freeze flag and body position, text
  box visibility (`TBOX::FIsVisible`), actor sound events
  (`ACTR::FSoundInFrm`) and scene sound events (`SCEN::FQuerySnd`).
* Checkpoints use "Save a Copy" (`MVIE::FSaveToFni(pfni, fFalse)`): the CFL
  is written to the copy, and the next edit moves the working CFL back to a
  temp file (`_FUseTempFile`), so the copy is never modified. The copy is
  named after the movie title because loading a file takes its title from
  the file name. After a restore, `ForceSaveAs` detaches the movie from the
  checkpoint file and `save_movie` defaults to the original path.
* Ending pose: `FEndRecord` always freezes the actor, and `FSetActionCore`
  re-freezes any action applied at the end of a path (`_FIsDoneAevSub`), so
  "keep looping" isn't expressible. Default is now `then: "rest"` (action 0
  when it's a rest action), which looks far more natural after dialogue.
* Actor descriptions were written by rendering all 45 shipped actors.
* `speak_line` always leaves the playhead at the end of the line, so
  consecutive lines chain without explicit frames; actors with no Talk
  action hold their rest pose for the line (which also keeps the scene long
  enough for the audio). Found when two lines from non-talking actors
  overlapped.
* `list_actions` `travels` comes from the Studio's trailing `*` marker; the
  step size alone is useless for this (stationary actions have a default
  step of 2.5 units too).
* Startup: automation sessions turn the startup sound off for that run,
  which also skips the two 4-second logo pauses it times (14 s → 6 s to
  ready). The first launch of a freshly rebuilt binary can take far longer
  while macOS vets it; the MCP launcher waits up to 180 s.
* Re-tested end to end through MCP with "a knight and a skeleton squabble
  over a treasure chest in a cave" (preview_cameras, overlay blocking, batch,
  speak_line for non-talking actors, checkpoint/restore, timeline); the
  saved movie plays in the stock build.

### Third round: editing and video export

* **Video export.** The bridge's `export_media` renders every frame of every
  scene and, while stepping, records sound events: scene sounds via
  `SCEN::FQuerySnd`, actor sounds via `ACTR::FSoundInFrm`/`FQuerySnd` (non
  motion-match). Each distinct sound is written once: WAV/MP3 chunk data is
  the original file bytes (imports are stored as 8-bit 11 kHz WAV); MIDI
  (`MIDS`, a millisecond-timed event stream) is rendered offline with the
  FluidSynth API and the bundled `soundfont.sf3`, by stepping `MSTP` events
  and calling `fluid_synth_write_s16` between them. `MSND` gained
  `FFindSndData` (its data chunk ids were private). The MCP server's
  `export_video` builds one ffmpeg graph: per-scene image sequences, `xfade`
  for 3DMM transitions (dissolve → dissolve, fade to black/white →
  fadeblack/fadewhite, black → short fadeblack), each sound `adelay`ed to its
  frame, trimmed where the next sound on the same target and type takes
  over (3DMM replaces rather than layers), looped with `-stream_loop`, mixed
  and loudness-normalised (`loudnorm` to −16 LUFS).
* **Exact soundtrack (revised).** The first version queried sound events
  (`SCEN::FQuerySnd`, `ACTR::FQuerySnd`) and guessed replacement rules, and
  missed motion-matched sounds. It now reads what the engine itself queues
  for playback: after each frame step the movie's `MSQ` holds that frame's
  sounds (scene events, actor events and motion-matched cels), already
  resolved by the engine's channel rules; `MSQ` gained read accessors
  (`Csqe`/`GetSqe`) and the bridge drains the queue without playing it. The
  first frame is replayed with `SCEN::FReplayFrm(fscenSounds | fscenActrs)`.
  The bridge then schedules clips in milliseconds per channel (actor or
  scene × sound type): a new sound cuts the current one unless queued, in
  which case it starts when the current one ends (WAV lengths from the
  header, MIDI from the rendered file). Verified with motion-matched
  footsteps on a walk cycle: one step every 7 frames.
* **Scene editing** reuses the scene sorter's composite movie (`CMVI`):
  `FAddToCmvi` lists this movie's scenes and appends another movie's as
  imports; entries can be reordered (`GL::Move` inserts *before* its target
  index, so forward moves pass `to + 1`) or dropped (`fNuked`), and
  `FSetCmvi` rebuilds the movie. `duplicate_scene` imports from a "Save a
  Copy" snapshot of the current movie. `trim_scene` uses `SCEN::FChop` and
  `FChopBack`.
* **Text and sound edits**: `edit_text` replaces a TBOX's runs with the
  existing character properties (or applies new colour/size with
  `FApplyChp`); `remove_text` is `SCEN::FRemTbox`; `remove_sound` is
  `SCEN::FRemSnd` or `ACTR::FDeleteSndCore` wrapped in an actor undo.

* `storyboard` samples a scene's frames evenly onto one labelled contact
  sheet; `suggest_camera` switches the background through its cameras
  (`BKGD::FSetCamera`, no scene events), renders the world each time so
  `BODY` screen bounds are current, and ranks cameras by actors fully in
  shot, then by on-screen size near half the frame height, then centring.
  It can't detect occlusion by set pieces.
* Palettes: each set may carry its own palette entries, which the engine
  merges into the screen palette only when it draws the scene
  (`MVIE::DoTrans` -> `GPT::SetActiveColors`). Captures taken right after
  a scene switch (export of every scene after the first, whole-movie
  storyboards) used the previous set's colours and came out scrambled.
  Captures now merge the scene's palette (`PglclrScene`) and make it the
  active palette first (`UseScenePalette`), since SDL converts 8-bit blits
  between surfaces through it; `RefreshViews` does the same so the screen
  matches the scene left current. The end-to-end test compares an exported
  frame of an undrawn set with a render made after the screen showed it.
* `storyboard(all_scenes: true)` shares the cells out across scenes by
  length (at least one each) and maps cells from sets with other palettes
  onto the first cell's palette.
* Text alignment uses `TXRD::FApplyPap` (paragraph `jc`), part of the
  stock rich-text format: the stock build shows aligned text the same way.
  Captions placed with the top/bottom/center presets default to centred.
* Facing: actors face along their model -x axis (found by adding actors
  with known facings and checking renders). `BODY::GetMatrix` (new, one
  accessor) exposes the root transform; `DegFacingFromCamera` compares it
  with the camera's +z axis, so 0 = toward the camera and +90 = screen
  right, the same convention as `facing` on add_actor. `rotate_actor(face)`
  turns by the difference to an absolute target (direction, actor, point).
* Visibility: `FVisibleFraction` counts an actor's pixels (render with and
  without it) and divides by the same count with nothing in front of it:
  other actors hidden and `BWLD::SetIgnoreBackgroundDepth` (new, off by
  default) filling the working Z buffer with the farthest depth instead of
  the set's depth image. Unobstructed actors read 0.99-1.0 in any pose; one
  behind the diner booth 0.26. Bounding boxes alone misjudge this (a
  half-covered actor with a clear face has a 69% box overlap). review_movie
  flags below 0.4; arrange_actors wants 0.8 for everyone. About 5 ms per
  actor.
* `arrange_actors` (blockcmd.cpp; layout maths in stage.cpp, unit tested)
  tries depths from 1 m back, and at each depth slides the group 0, +-0.6,
  +-1.2 m sideways, keeping the first spot where everyone is in shot, not
  cut off at the sides, not box-covered by a nearer actor by more than
  35%, and at least 80% visible. Movies it arranges play the same in the
  stock build.
* Physics (physics.cpp, unit tested; physicscmd.cpp). The engine has no
  physics, and a recorded path can't hold arbitrary per-frame positions:
  `FRecordMove` places route points exactly one action-step apart, so speed
  comes from the action. Instead the actor holds its action in place
  (`FHoldInPlace`, the same recording animate uses) and every frame gets a
  `FMoveRoute` without `fmafEntireSubrte`, i.e. an `aetMove` "move from
  here" event, the Studio's own edit, so the stock build plays it. Spin is
  `FRotate` per frame (actors roll about their model z axis after being
  turned to face the travel); squash-and-stretch on hard floor impacts is
  `FPull` (the Studio's squash tool; it multiplies the current stretch, so
  the bake applies ratios and restores 1:1 two frames later). The
  simulation: 20 substeps per frame, gravity 9.8, restitution from
  `bounciness`, Coulomb friction on the floor and at bounces, light air
  drag; other actors are vertical cylinders sized from their world bounds
  (`GetActorFootprint`) at each frame, so moving actors are hit where they
  are. A Cube thrown at Frank plays identically in the stock build.
* Walks avoid people: `move_actor(avoid_actors)` (default on) runs the path
  through `VecRouteAround`, which inserts a detour beside anyone a leg
  would pass through (where they stand at the start frame), on the side
  the leg was closer to, or the camera side when it's nearly a tie so the
  walker stays in view. Legs that start or end at someone are left alone.
* review_movie flags collisions: two on-stage actors closer than 70% of
  their combined footprint radii at the same height ("walk through each
  other" if either is moving, else "stand inside each other").
* Frame rate: 6 fps is a compile-time constant (`kfps`, inc/socdef.h), not
  stored in the movie. Everything in a .3mm counts frames (each action pose,
  each step, sound starts), so raising it would speed up every movie,
  including the shipped ones, while sounds keep real time and drift. For
  smoother video without touching the movie, `export_video(smooth: true)`
  motion-interpolates in-betweens with ffmpeg's minterpolate (6 -> 24 fps
  before scaling); fast limbs can smear a little, so it is opt-in.
* `auto_camera` (directcmd.cpp; choice rules in directing.cpp, unit
  tested). Beats start at each actor sound (`FSoundInFrm`), plus an
  establishing beat before the first. Every camera is tried on every beat
  (`BKGD::FSetCamera`, no scene events) at frames near the beat's start,
  middle and end, scoring the worst: the speaker must be in shot, not cut
  off and at least half visible (`FVisibleFraction`), then gains for
  framing (ideal height per style), centring, facing the camera (3/4 view
  best, back turned penalised in coverage) and listeners in frame;
  establishing beats want everyone visible. `VecChooseShots` is a Viterbi
  pass with a cost per cut; `HoldShortShots` merges shots under
  min_shot_seconds. Cuts are applied by walking the scene and calling
  `MVIE::FChangeCam` where the camera differs (changing back to the
  previous camera deletes an old cut), so they are ordinary camera events.
  On the diner movie coverage gives wide / Greg single / wide / Greg single
  and plays the same in the stock build. review_movie now reports a
  listener out of shot or hidden while someone else speaks as a note, not a
  warning, since that is ordinary cutting.
* `BWLD::SetIgnoreBackgroundDepth` rebuilds the whole working Z buffer when
  switched: the per-render cleanup only touches regions actors were drawn
  in, so with no other actor on screen the far fill never happened (an
  actor fully behind a booth measured as fully visible), and switching back
  must restore the set's depth everywhere or actors could later draw
  through furniture.
* `script_scene` (MCP side, tools/3dmm-mcp/src/script.rs + server.rs) checks
  the whole script first (cast names, one of says/does/wait per line,
  durations) and reports the exact entry at fault, so a bad script builds
  nothing. Then: checkpoint (if the movie has scenes), add_scene, add_actor
  per character, arrange_actors, each line in order on a running frame
  counter (speak_line / animate / a pause), a hold on the first character
  if the script ends on a pause, the title for its reading time,
  auto_camera, the transition and review_movie. Characters without a voice
  get different installed voices (preferred list first, then any English
  voice from `say -v ?`). The pie movie (26 s, two scenes) took two calls.
* Staging in scripts: `enters`/`exits` (left or right), `walks_to` (a
  character or a point) and `throws` (a prop `at` someone, with an optional
  `reaction`). The parser checks staging order (nobody acts before entering
  or after exiting; nobody walks up to or throws at someone off stage).
  Entering characters are blocked with the group, then removed and re-added
  on their cue just past the screen edge at their mark's depth (from the
  camera's visible floor) and walk in with the template's walk (first
  travelling action named like a walk, not backwards), turning to the
  nearest character. Exits walk off-screen and `remove_actor(mode: exit)`.
  Crosses stop 0.8 m short of the character and then on the nearest free
  spot (`clear_spot`: 0.65 m from anyone, preferring the camera side).
  Throws add the prop at hand height beside the thrower and use `physics`;
  a hit with a reaction animates the target from the impact frame. The
  closing pause holds someone still on stage. review_movie notes, rather
  than warns about, an actor briefly hidden while walking and not speaking.
* Sound in scripts: `music` (by name or mood word; "none" plays the
  "sound stopper" to silence the set's own music), `sound` lines (an effect
  on the scene or `on` a character; the next line doesn't wait) and
  `hit_sound` on throws (attached to the target on the impact frame).
  Names resolve through list_sounds' filter: an exact name, else the first
  containing it, with a note saying which was used. `subtitles: true`
  captions each spoken line at the bottom; the next line waits until the
  caption has been up for its reading time (the review's rule), since quick
  lines are said faster than read. A script with a throw blocks the group
  2 m apart (1 m left no room, and the prop spawned against the target),
  and the prop appears up to 0.6 m in front of the thrower.
* `script_movie` parses every scene before building any (errors say
  `scenes[i]: lines[j]: ...`), requires a character to keep one actor
  across scenes, and gives each character one voice for the whole movie
  (the one named anywhere, else a distinct installed voice). It
  checkpoints once, builds the scenes in order, totals warnings and length
  (`target_seconds` adds a note when more than 20% off) and can export the
  MP4. "Make me a 30-second movie of two guys arguing in a diner" became
  one call: two scenes, 31.5 s, subtitles, music, a throw and an exit,
  built and exported in 7.4 s with no review warnings.
* `add_text(color: "auto")` renders the 3D picture, averages the
  brightness under the text box and picks dark navy text over bright areas
  (above 0.55) or yellow over dark ones; scripts use it for subtitles and
  titles (yellow subtitles over the diner's cream counter were unreadable).
* review_movie notes, rather than warns about, an actor hidden for under a
  second by another actor walking past in front.
* auto_camera penalises a shot where another actor covers over 45% of the
  frame (someone pressed against the lens), and the framing score drops
  faster for subjects bigger than ideal than for ones a little small.
* `review_movie` (reviewcmd.cpp; rules in review.cpp, unit tested) steps
  every frame of each scene without rendering to track movement, action
  changes, sound starts, cuts and text visibility, then renders every third
  frame plus each cut and sound start to read `BODY` screen bounds. Out of
  shot = not `FIsInView` or bounds wholly off screen; cut off = bounds touch
  the left, right or top edge (the bottom is allowed: close shots crop
  legs); hidden = 60% of the bounds covered by a nearer actor's bounds
  (distance to the camera). Bounding boxes are coarse and set pieces are
  invisible to it, like `suggest_camera`. A sound start marks the actor as
  speaking for the next two seconds, which raises out-of-shot and cut-off
  findings to warnings. Static stretches ignore actors held in a "rest"
  action. The whole diner movie reviews in about 20 ms.
* Request timeouts (`play` with `wait`, `goto_studio`) use
  `std::chrono::steady_clock` instead of the app's scalable `TsCurrent()`
  clock, after one release run reported a 60 s playback timeout 14 s in.

## 13. Decisions

* **Polling, not threads.** Non-blocking sockets serviced from
  `APP::FCmdIdle` (runs ~every ms, including during playback). Simplest
  correct option; every engine call is naturally on the UI thread.
* **Hand-rolled JSON and PNG** (`json.cpp`, `pngwrite.cpp`) to avoid adding
  vcpkg dependencies to an upstream project. Both are unit tested.
* **Paths use the mouse-recording API**, not hand-built route events, so the
  `.3mm` contents are exactly what the Studio would produce.
* **Automation starts in the Studio, windowed**, without touching the user's
  saved fullscreen preference.
* **`add_scene` keeps Studio semantics**: if the current scene is still empty
  its background is replaced (the reply says so).
* **Names are fuzzy, ids are exact.** Content and actors resolve by id, exact
  name, unique prefix or unique substring; ambiguity is an error listing the
  candidates.
* **Errors are drained** from the engine's error stack after every command so
  `DisplayErrors()` never pops a modal alert during automation.
* **MCP tool table is declarative** (`tools.rs`) with hand-written JSON
  schemas, so descriptions can be tuned for models without touching Rust
  types. `speak_line` is MCP-side sugar over `import_sound`/`attach_sound`/
  `animate` using the OS speech synthesizer.
* **Build option is `3DMM_AUTOMATION`** (matching the project's `3DMM_*`
  option names) rather than `THREEDMM_AUTOMATION`; the environment variable
  that enables it at runtime is `THREEDMM_AUTOMATION` because shell variable
  names can't start with a digit.

## 14. Known limitations and open questions

* One level of undo (engine limit).
* Speed of a path is set by the action (and actor size); there is no
  "arrive at frame N" option. `move_actor` reports the arrival frame.
* No per-actor facing readout: `describe_scene` reports position, action and
  screen bounds but not orientation (no public accessor).
* No lip sync (engine has none); `speak_line` pairs speech with a talking
  action.
* Frame capture is 8-bit palettized (that's what 3DMM renders); half-res
  "slow CPU" mode renders at the reduced size the engine uses.
* Debug builds show assertion dialogs, which block the bridge until
  dismissed; use the release preset for unattended runs.
* `goto_studio` from the building runs the Util gob's "Load Studio" script;
  it is only needed if someone navigates away, since automation sessions
  start in the Studio.
* Paths on Windows with non-ASCII characters may fail in `render_frame`
  (`fopen` with narrow strings).
* Focus loss used to freeze the engine: `APP::_Activate` queues
  `cidDeactivate`, which suspends every GOK and sits in a modal loop until
  the window is reactivated. The bridge still answered (idle runs inside the
  loop), but a queued `cidPlay` never started and playback already running
  stopped advancing, so `play` with `wait` timed out whenever the app lost
  focus (e.g. a freshly rebuilt app opening behind the terminal). In
  automation mode `_Activate` now does nothing; a hidden app plays the
  30-second diner movie in 33 s, the same as a visible one.
* Only one 3DMMEx instance can run at a time (stock behaviour), so the MCP
  server can't start a second copy if a non-automation copy is open.
