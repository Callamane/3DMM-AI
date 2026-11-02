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
* Only one 3DMMEx instance can run at a time (stock behaviour), so the MCP
  server can't start a second copy if a non-automation copy is open.
