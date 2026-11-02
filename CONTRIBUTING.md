# Contributing to 3DMM-AI

Thanks for helping out. 3DMM-AI has two halves, and contributions to either are welcome: the 3D Movie Maker app
with its automation bridge (C++), and the MCP server that drives it (Rust).

## Reporting a bug

Open a GitHub issue with:

* what you expected and what happened
* how to reproduce it: for scripting problems, the `script_scene` / `script_movie` arguments are the most useful
  thing you can attach
* your OS, and whether you built the app as Debug or RelWithDebInfo
* a screenshot or the storyboard image if the problem is visual

Windows reports are especially welcome: the code is written for it but hasn't been run there yet.

## Contributing code

See the [README](README.md) for building. Before opening a pull request, run:

```sh
ctest --test-dir build/<preset>                          # C++ unit tests
cd tools/3dmm-mcp
cargo fmt --check && cargo clippy --all-targets -- -D warnings
cargo test                                               # unit tests
cargo test --release -- --include-ignored                # also drives the app (needs a built app and speech)
```

The ignored tests launch the app and build real scenes, including the quality benchmark
(`tests/quality_bench.rs`, a few minutes). If you change how scenes are blocked or cut, include its before and after
summary lines in the pull request.

### Where things are

* `src/automation/`: the bridge. Command handlers are in `*cmd.cpp`; logic that doesn't need the engine (shot
  choice, review rules, physics, stage geometry) lives in separate files with unit tests in `tests/`.
* `tools/3dmm-mcp/src/`: the MCP server. `tools.rs` has the tool table and schemas, `server.rs` builds scenes from
  scripts, `script.rs` parses and checks scripts, `speech.rs` does text-to-speech.
* `doc/automation/NOTES.md`: design notes and measurements. Add to it when you change behaviour.
* The original app: Kauai (application framework), BRender (3D engine), AudioMan (Windows sound mixing),
  Socrates (the 3D Movie Maker app), Chomp (script compiler), Ched/Chelp (authoring tools).

### Coding style

* C++: format with the included clang-format configuration. Code inherited from 3D Movie Maker
  uses Microsoft's Apps Hungarian style (see [Hungarian Notation by Doug Klunder](https://idleloop.com/hungarian/));
  keep to it there and in the bridge, and prefer the Kauai types (`STN`, `GL`) in engine code.
* Rust: `cargo fmt` and clippy with warnings as errors.
* If in doubt, copy the style of the code around you.

### Don't change the file format

Movies must stay ordinary `.3mm` files: anything made here should open in 3D Movie Maker, and every movie made with
3D Movie Maker should still open here. Don't change the file format or the shipped content files.

### Licensing

Code you submit is licensed under the MIT license, like the rest of the project.
