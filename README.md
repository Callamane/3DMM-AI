# 3DMMEx

3DMMEx is a source port of [Microsoft 3D Movie Maker](https://github.com/microsoft/Microsoft-3D-Movie-Maker) that aims to preserve the classic 3D Movie Maker experience while adding minor enhancements for an improved user experience.

3DMMEx builds upon the amazing work of the [3DMMForever](https://github.com/foone/3DMMForever) project.

## Features

* Cross-platform support
  * SDL2 graphics/input support
  * Linux and macOS platform support
* Portability improvements
  * Supports compiling with Visual Studio 2022, Clang and GCC
  * 64-bit support: supports compiling for x64 and ARM64
  * Inline assembly language code replaced with portable C++
  * Unit tests
* Usability improvements
  * Improved mouse input handling
  * New keyboard shortcuts for scrolling in browser dialogs
  * High-quality sound import

## Getting Started

Windows and macOS users can download a release from the [releases page](https://github.com/benstone/3DMMEx/releases). Unpack the ZIP file and run 3DMOVIE.EXE / 3D Movie Maker. Linux users can follow the build instructions below to compile from source.

3DMMEx will default to running in full-screen mode. You can switch to windowed mode by pressing Alt-Enter.

On Windows you can change application settings by pressing Ctrl-Shift-I to open the Product Information dialog box. There is no settings UI for Linux/macOS yet, so to change settings you will need to edit the configuration file and restart. This file is located in:

* Windows: `%APPDATA%\3DMMEx\3dmovie.ini`
* macOS: `~/Library/Application Support/3DMMEx/3dmovie.ini`
* Linux: `${XDG_CONFIG_HOME}/3DMMEx/3dmovie.ini` or `~/.config/3DMMEx/3dmovie.ini`

For help with making a movie, check out the Help Machine in the Studio or the tutorials in the Projects Room.

## Build

### Requirements

* CMake 3.28+
* Ninja build
* Windows:
  * Visual Studio 2022
    * Desktop development with C++ workload
    * Clang (optional)
* Linux:
  * GCC
  * Development libraries for SDL2, SDL2_ttf, gstreamer, GTK3, iconv and Fontconfig
  * Zenity (optional: used for dialog boxes)
  * [Comic Sans MS font](https://corefonts.sourceforge.net/) (optional but strongly recommended!)
  * gstreamer
  * Installing dependencies:
    * Ubuntu: `sudo apt install g++ cmake ninja-build libsdl2-dev libsdl2-ttf-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgtk-3-dev libfontconfig-dev zenity`
* macOS:
  * Xcode tools
  * vcpkg
    * vcpkg will download and build SDL2, Fontconfig, FluidSynth, and their dependencies.
    * Some dependencies require autotools when building. You can install that with Homebrew: `brew install autoconf autoconf-archive automake libtool`

### Building

Use CMake to build the project. The project includes a CMakePresets.json file that specifies [build presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html).

#### Building on Windows

* Open the Developer Command Prompt for Visual Studio 2022 (or use a tool such as [VCVars](https://github.com/bruxisma/VCVars) to add the build tools to your path)
* Run `cmake --preset <preset-name>`:
  * x86 Debug build: `x86-msvc-debug`
    * Debug builds are recommended for local development and testing, as they have additional runtime checking that helps to identify bugs.
  * x86 Release build: `x86-msvc-relwithdebinfo`
  * There are additional presets for x64 builds and compiling with Clang instead of the Visual C++ compiler
* Run `cmake --build build\<preset-name> --target install`
  * The `--target install` is optional but recommended. This will create a directory `dist\<preset-name>` with the directory layout required for 3DMM to run.
  * Kauai development tools (eg. Chelp, Ched) are not built by default. You can build them by building the `tools` target.
* To run tests, run `ctest --test-dir build\<preset-name>`
* To build a release package, run `cmake --build build\<preset-name> --target dist`
  * A ZIP archive containing a portable install is created in the build directory.
  * If WiX is installed, an MSI package is also created.

#### Building on macOS

* Set `VCPKG_ROOT` to your vcpkg checkout
* Run `cmake --preset <preset-name>`
  * Apple Silicon / ARM64: `arm64-macos-debug` or `arm64-macos-relwithdebinfo`
  * Intel: `x86_64-macos-debug` or `x86_64-macos-relwithdebinfo`
* Run `cmake --build build/<preset-name> --target install`
* To run 3DMM, run `dist/<preset-name>/3D Movie Maker.app`
  * The current deployment target is macOS 12.0 and later

#### Building on other platforms

* Run `cmake --preset <preset-name>`
  * The recommended preset is `sdl-debug` for local development, or `sdl-relwithdebinfo` for release.
* Run `cmake --build build/<preset-name> --target install`
* To run tests, run `ctest --test-dir build/<preset-name>`
* To run 3DMM, run `dist/<preset-name>/3dmovie`

## Automation bridge and MCP server

3DMMEx can be driven by other programs, including AI assistants via the
[Model Context Protocol](https://modelcontextprotocol.io). Two pieces:

* **Automation bridge** (`src/automation/`): a small command server compiled into the app. It listens on
  `127.0.0.1` only, speaks newline-delimited JSON, and calls the movie engine directly on the UI thread. It is
  **off unless enabled** at runtime, and can be compiled out with `-D3DMM_AUTOMATION=OFF`.
* **MCP server** (`tools/3dmm-mcp/`, Rust, [`rmcp`](https://crates.io/crates/rmcp), stdio): exposes the bridge as MCP
  tools, returns rendered frames as images, and starts the app on demand.

Engineering notes (object model, coordinates, how paths are recorded, design decisions, verification and limitations)
are in [doc/automation/NOTES.md](doc/automation/NOTES.md).

### Build and connect (macOS)

```sh
export VCPKG_ROOT=~/vcpkg   # checkout path must not contain spaces
cmake --preset arm64-macos-relwithdebinfo
cmake --build build/arm64-macos-relwithdebinfo --target install
ctest --test-dir build/arm64-macos-relwithdebinfo
cargo build --release --manifest-path tools/3dmm-mcp/Cargo.toml
claude mcp add --scope user 3dmm -- "$PWD/tools/3dmm-mcp/target/release/3dmm-mcp"
```

Then open Claude Code and ask for a movie, e.g. *"make me a 30-second movie of two guys arguing in a diner, save it
to ~/Movies/diner.3mm"*. The MCP server starts 3DMMEx (windowed, straight into the Studio) on the first tool call.
The server finds the app in `dist/` next to it, preferring release builds; set `THREEDMM_APP` to use another build
(a `.app` bundle or executable). Other environment variables: `THREEDMM_PORT` (default 7341) and
`THREEDMM_NO_AUTOLAUNCH=1` (require an already-running app). `export_video` needs ffmpeg (`brew install ffmpeg`, or set
`THREEDMM_FFMPEG`).

Windows: the bridge is written to be portable (Winsock on Windows, BSD sockets elsewhere; no macOS-only APIs) and
`speak_line` uses Windows' built-in speech synthesizer, but so far it has only been built and tested on macOS.

### Enabling the bridge by hand

Start 3DMMEx with `--automation` (port 7341) or `--automation=PORT`, or set the environment variable
`THREEDMM_AUTOMATION=1` (or a port number). The app logs `automation: listening on 127.0.0.1:PORT` to stderr,
skips the building/intro, logo pauses and startup jingle, opens the Studio with a new movie and runs in a window
for that session (about 6 s from launch to ready). Only one client
may connect at a time.

### Protocol

One JSON object per line in each direction:

```
→ {"id": 1, "cmd": "add_actor", "args": {"template": "Billy", "position": {"x": -1, "z": 1}}}
← {"id": 1, "ok": true, "result": {"actor": {"id": 0, "name": "Billy", ...}}}
← {"id": 2, "ok": false, "error": {"code": "not_found", "message": "no actor/prop named 'Bily'; ..."}}
```

`hello` returns the protocol version (currently 1). Error codes: `bad_args`, `not_found`, `bad_state`,
`engine_error`, `unknown_command`, `io_error`. Requests run in order; `play` with `"wait": true` replies when
playback ends. Content can be named by id (`"bkgd:2:2"`) or by name (case-insensitive, unique prefixes work);
actors by numeric id or name.

### Tools

| Group | Tools |
|---|---|
| Session | `launch`, `get_state` (`all_scenes` for total length), `goto_studio`, `new_movie`, `open_movie`, `save_movie`, `undo`, `redo`, `checkpoint`, `restore_checkpoint`, `list_checkpoints` |
| Catalog | `list_backgrounds` (with cameras and descriptions), `list_cameras`, `list_templates` (actors described by look), `list_actions`, `list_costumes`, `list_materials`, `list_sounds`, `list_3d_word_styles` |
| Scenes | `add_scene`, `set_background`, `select_scene`, `set_camera`, `set_transition`, `delete_scene`, `move_scene`, `duplicate_scene`, `import_scenes` (from another .3mm), `trim_scene`, `goto_frame`, `add_pause` |
| Actors | `add_actor`, `add_3d_word`, `select_actor`, `remove_actor`, `set_action`, `move_actor` (walk a path, stepping around people), `physics` (drop, throw or launch with gravity, bounces, collisions, spin and squash-and-stretch), `animate` (act on the spot for a duration), `arrange_actors` (block a group: facing each other, all in shot, clear of furniture), `place_actor`, `rotate_actor` (`face` another actor, the camera or a point), `scale_actor`, `set_costume`, `set_name`, `actor_lifetime` |
| Text and sound | `add_text`, `edit_text`, `remove_text`, `import_sound`, `attach_sound`, `remove_sound`, `speak_line` (MCP only: text-to-speech dialogue, attached and acted) |
| Observe and play | `describe_scene`, `describe_timeline` (who does what when), `render_frame` (returns the image; `overlay` draws the stage grid and actor labels), `preview_cameras` (every angle of a set on one image), `storyboard` (a scene's frames on one image, or the whole movie with `all_scenes`), `suggest_camera` (angles ranked for the actors in shot), `auto_camera` (edits a scene's cuts: establishing shot, then the best angle on whoever speaks), `review_movie` (automatic check: actors out of shot, cut off or hidden, unreadable text, dead stretches, each with a fix), `export_frames` (PNG sequence), `play`, `stop` |
| Export | `export_video` (MCP: MP4 with transitions and all sound, music rendered from MIDI; needs ffmpeg), `export_media` (bridge: frames + audio cues), `export_frames` |
| MCP only | `script_scene` (a whole scene from a script: set, cast, blocking, dialogue, title, cuts and a review, in one call), `batch` (several calls in one) |

The MCP server checks arguments against each tool's schema and rejects unknown names (with the list it takes)
instead of letting a typo fall back to a default target. Background and actor names also match their descriptions,
so `add_scene(background: "diner")` finds the restaurant set. The bridge also has `hello` and `quit`. The MCP server publishes a workflow guide as the resource
`3dmm://guide/workflow` and a `make_movie` prompt.

### Coordinates

Positions are **stage coordinates in metres, relative to the current camera**: `x` is right (+) / left (−) on
screen, `z` is away from (+) / toward (−) the camera, optional `y` is height above the floor. The origin is the spot
where 3D Movie Maker drops new actors for that camera, usually centre stage. `describe_scene` reports the floor area
the camera sees and, for each actor, its stage position, projected screen position and on-screen bounds. Raw engine
coordinates are available as `{"world": [x, y, z]}` (10 units = 1 m, y up, floor at 0). Time is in frames: 6 per
second, numbered per scene from 1.

### Limitations

* One level of undo (an engine limit); saving clears it.
* Path speed comes from the chosen action (walk vs run) and the actor's size; `move_actor` reports how many
  frames a path took rather than taking a target duration.
* No lip sync (the engine has none); `speak_line` pairs speech with a talking action.
* Recording an action or path for an actor replaces what that actor did after that frame, as in the Studio.
  Actions end in the rest pose (or frozen); an action can't be left looping past the end of a recording.
* Rendered frames are 8-bit, 544×306, like the Studio's view.
* `export_video` takes its soundtrack from the engine's own sound queue as it steps through each frame, so it follows
  playback's rules (a new sound replaces the one on the same actor/scene and type unless queued; motion-matched
  sounds such as footsteps repeat with the action). Sounds stop at the end of their scene.
* Debug builds show assertion dialogs that block automation until dismissed; use a release build unattended.
* Only one copy of 3DMMEx can run at a time.

## Contributing

See [CONTRIBUTING.MD](CONTRIBUTING.md) for information on how to contribute to 3DMMEx.

If you have questions, please create a GitHub issue or post in the project's [Discussions page](https://github.com/benstone/3DMMEx/discussions).

## Legal stuff

The following sections have been carried over from the original 3D Movie Maker [GitHub repository](https://github.com/microsoft/Microsoft-3D-Movie-Maker) released by Microsoft in May 2022.

### Code cleanup

This code was restored from the Microsoft corporate archives and cleared for release.

* Developer names and aliases were removed, with the exception of current employees who worked on the
  original release who consented to keeping their names in place
* The archive consisted of several CDs, some of which were for alternate builds or products, and
  have been excluded
* The code does not build with today's engineering tools, and is released as-is.

### Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft
trademarks or logos is subject to and must follow
[Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks/usage/general).
Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship.
Any use of third-party trademarks or logos are subject to those third-party's policies.

This repo includes a build from 1995 of BRender from Argonaut software. Approval to open source BRender as MIT was given in an email from Jez San, former CEO of Argonaut. Other versions of BRender exist at <https://github.com/foone/BRender-v1.3.2> and <https://github.com/foone/BRender-1997> Thanks to Jez and the whole BRender team for their hard work on this amazing engine. A full historical list of BRender contributors is available at <https://github.com/foone/BRender-v1.3.2/blob/main/README.md>

This repo does NOT include the SoftImage SDK "./DKIT" from 1992.

Jez also offered this interesting BRender anecdote in an email:

```
When Sam Littlewood designed BRender, he didn’t write the code. And then document it.  
The way most things were built at the time.
First, he wrote the manual.  The full documentation
That served as the spec.  Then the coding started.
```
