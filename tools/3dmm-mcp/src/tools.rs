//! The MCP tool catalog. Each tool forwards to the bridge command of the same
//! name unless noted. Descriptions are written for a model that has never
//! seen 3D Movie Maker.

use serde_json::{json, Map, Value};

/// How the server handles a tool call.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    /// Forward arguments to the bridge command with this name.
    Bridge(&'static str),
    /// Start (or reconnect to) the app.
    Launch,
    /// Render a frame and return it as an image.
    RenderImage,
    /// Synthesize a spoken line, import it and (optionally) attach and act it.
    SpeakLine,
}

pub struct ToolSpec {
    pub name: &'static str,
    pub kind: Kind,
    pub description: String,
    pub schema: Value,
    /// Seconds to wait for the bridge (most commands are instant)
    pub timeout_secs: u64,
}

const COORDS: &str = "Positions are stage coordinates in metres relative to the current camera: x = right(+)/left(-) \
on screen, z = away from the camera(+)/toward it(-), optional y = height above the floor. The origin (0,0) is the \
spot where 3D Movie Maker drops new actors for this camera, usually centre stage. describe_scene reports the \
visible floor area; staying within roughly x -2..2 and z 0..3 keeps actors in shot for most cameras (check with \
render_frame). Escape hatch: {\"world\": [x, y, z]} in raw engine units (10 units = 1 m).";

const FRAMES: &str = "Movies run at 6 frames per second. Each scene has its own frame numbers starting at 1.";

fn position_schema(desc: &str) -> Value {
    json!({
        "description": desc,
        "oneOf": [
            {
                "type": "object",
                "properties": {
                    "x": {"type": "number", "description": "metres to the right of centre (negative = left)"},
                    "z": {"type": "number", "description": "metres away from the camera (negative = closer)"},
                    "y": {"type": "number", "description": "metres above the floor (default: on the floor)"}
                },
                "additionalProperties": false
            },
            {
                "type": "object",
                "properties": {
                    "world": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}
                },
                "required": ["world"],
                "additionalProperties": false
            }
        ]
    })
}

fn actor_ref(desc: &str) -> Value {
    json!({
        "type": ["integer", "string"],
        "description": format!("{desc} Actor id (from add_actor/describe_scene) or name; names match \
                                case-insensitively by prefix, e.g. \"Bino\" finds \"Bino BeGood\". Defaults to the \
                                selected actor.")
    })
}

fn frame_prop() -> Value {
    json!({"type": "integer", "minimum": 1, "description": "Go to this frame of the current scene first (default: the current frame)."})
}

fn obj(properties: Value, required: &[&str]) -> Value {
    let mut schema = Map::new();
    schema.insert("type".into(), json!("object"));
    schema.insert("properties".into(), properties);
    if !required.is_empty() {
        schema.insert("required".into(), json!(required));
    }
    Value::Object(schema)
}

fn spec(name: &'static str, kind: Kind, description: impl Into<String>, schema: Value) -> ToolSpec {
    ToolSpec {
        name,
        kind,
        description: description.into(),
        schema,
        timeout_secs: 60,
    }
}

fn bridge(name: &'static str, description: impl Into<String>, schema: Value) -> ToolSpec {
    spec(name, Kind::Bridge(name), description, schema)
}

pub fn all_tools() -> Vec<ToolSpec> {
    let mut tools = vec![
        // ---------------------------------------------------------------
        // Session
        // ---------------------------------------------------------------
        spec(
            "launch",
            Kind::Launch,
            "Start Microsoft 3D Movie Maker with automation enabled (or reconnect if it is already running) and \
             open the Studio with a new, empty movie. Other tools call this automatically, so you rarely need it. \
             The app window opens on the user's screen.",
            obj(json!({}), &[]),
        ),
        bridge(
            "get_state",
            "Where things stand: which screen is showing, the open movie (title, file, unsaved changes, scene \
             count), the current scene (frame, frame range, background, camera, selected actor). all_scenes=true \
             also lists every scene's length and the movie's total length in seconds.",
            obj(json!({"all_scenes": {"type": "boolean", "default": false}}), &[]),
        ),
        bridge(
            "goto_studio",
            "Bring the app to the Studio (the movie editor) if it is somewhere else, e.g. the lobby.",
            obj(json!({}), &[]),
        ),
        bridge(
            "new_movie",
            "Close the current movie and start a new empty one. Fails if there are unsaved changes unless \
             discard_changes is true.",
            obj(json!({"discard_changes": {"type": "boolean", "default": false}}), &[]),
        ),
        bridge(
            "open_movie",
            "Open a .3mm movie file in the Studio.",
            obj(
                json!({
                    "path": {"type": "string", "description": "Absolute path to a .3mm file."},
                    "discard_changes": {"type": "boolean", "default": false}
                }),
                &["path"],
            ),
        ),
        bridge(
            "save_movie",
            "Save the movie as a .3mm file that opens in any copy of 3D Movie Maker. Saving clears the undo \
             history. The movie must have at least one scene.",
            obj(
                json!({
                    "path": {"type": "string", "description": "Absolute path ending in .3mm. Optional after the first save."},
                    "overwrite": {"type": "boolean", "default": true}
                }),
                &[],
            ),
        ),
        bridge(
            "undo",
            "Undo the last edit (3D Movie Maker keeps exactly one level of undo).",
            obj(json!({}), &[]),
        ),
        bridge("redo", "Redo the edit that undo just reverted.", obj(json!({}), &[])),
        // ---------------------------------------------------------------
        // Catalog
        // ---------------------------------------------------------------
        bridge(
            "list_backgrounds",
            "List the 3D sets (backgrounds) a scene can use, each with its camera angles. Names are terse \
             with a description of each set; filter matches names and descriptions (e.g. \"diner\"). \
             Pass ids or names to add_scene.",
            obj(
                json!({
                    "filter": {"type": "string", "description": "Only names containing this text."},
                    "include_cameras": {"type": "boolean", "default": true}
                }),
                &[],
            ),
        ),
        bridge(
            "list_cameras",
            "List the camera angles of a background (default: the current scene's).",
            obj(json!({"background": {"type": "string"}}), &[]),
        ),
        bridge(
            "list_templates",
            "List actors (people/creatures that walk and act) and props (objects). Actors differ a lot in what they \
             can do (some cannot talk or run), so check list_actions before casting. Use the id or name with \
             add_actor.",
            obj(
                json!({
                    "type": {"type": "string", "enum": ["all", "actor", "prop"], "default": "all"},
                    "filter": {"type": "string"}
                }),
                &[],
            ),
        ),
        bridge(
            "list_actions",
            "List what an actor or prop can do (walk, run, dance, wave, talk, ...). metres_per_frame is how far \
             the action travels per frame when used on a path; actions marked * in the name are the walking/running \
             family.",
            obj(json!({"template": {"type": "string", "description": "Template id or name."}}), &["template"]),
        ),
        bridge(
            "list_costumes",
            "List a template's body parts and how many built-in costumes each has, for set_costume.",
            obj(json!({"template": {"type": "string"}}), &["template"]),
        ),
        bridge(
            "list_materials",
            "List materials (colors/textures) that set_costume can paint onto a body part.",
            obj(json!({"filter": {"type": "string"}}), &[]),
        ),
        bridge(
            "list_sounds",
            "List built-in sounds: sfx (effects), speech (spoken lines) and music. Use the id or name with \
             attach_sound.",
            obj(
                json!({
                    "type": {"type": "string", "enum": ["all", "sfx", "speech", "music"], "default": "all"},
                    "filter": {"type": "string"}
                }),
                &[],
            ),
        ),
        bridge(
            "list_3d_word_styles",
            "List shapes and fonts for add_3d_word.",
            obj(json!({}), &[]),
        ),
        // ---------------------------------------------------------------
        // Scenes
        // ---------------------------------------------------------------
        bridge(
            "add_scene",
            format!(
                "Add a scene after the current one using a background, and make it current. Do this before \
                 adding actors (a new movie has no scenes). If the current scene is still empty, 3D Movie Maker \
                 replaces its background instead of adding another scene. {FRAMES}"
            ),
            obj(
                json!({
                    "background": {"type": "string", "description": "Background id or name from list_backgrounds."},
                    "camera": {"type": "integer", "minimum": 0, "default": 0, "description": "Camera angle index."}
                }),
                &["background"],
            ),
        ),
        bridge(
            "set_background",
            "Change the current scene's background (keeps actors and text).",
            obj(json!({"background": {"type": "string"}}), &["background"]),
        ),
        bridge(
            "select_scene",
            "Make another scene current (0-based index).",
            obj(json!({"index": {"type": "integer", "minimum": 0}, "frame": frame_prop()}), &["index"]),
        ),
        bridge(
            "set_camera",
            "Switch camera angle from a frame onward (a cut within the scene). Stage coordinates are defined \
             relative to the camera, so positions you pass afterwards are interpreted for the new camera.",
            obj(
                json!({"camera": {"type": "integer", "minimum": 0}, "frame": frame_prop()}),
                &["camera"],
            ),
        ),
        bridge(
            "set_transition",
            "Set how the current scene ends and the next begins.",
            obj(
                json!({"transition": {"type": "string", "enum": ["cut", "fade_to_black", "fade_to_white", "dissolve", "black"]}}),
                &["transition"],
            ),
        ),
        bridge(
            "delete_scene",
            "Delete a scene (default: the current one).",
            obj(json!({"index": {"type": "integer", "minimum": 0}}), &[]),
        ),
        bridge(
            "goto_frame",
            format!("Move the current scene's playhead. Edits apply at the current frame. {FRAMES}"),
            obj(
                json!({
                    "frame": {"type": "integer", "minimum": 1},
                    "where": {"type": "string", "enum": ["start", "end"]}
                }),
                &[],
            ),
        ),
        bridge(
            "add_pause",
            "Pause playback at a frame: for some seconds, until the current sound finishes, or until a mouse \
             click (avoid until_click for movies meant to play by themselves). kind=remove deletes a pause.",
            obj(
                json!({
                    "kind": {"type": "string", "enum": ["seconds", "until_sound", "until_click", "remove"]},
                    "seconds": {"type": "number", "exclusiveMinimum": 0, "maximum": 600},
                    "frame": frame_prop()
                }),
                &["kind"],
            ),
        ),
        // ---------------------------------------------------------------
        // Actors
        // ---------------------------------------------------------------
        bridge(
            "add_actor",
            format!(
                "Put an actor or prop into the current scene at the current frame (or 'frame'). New actors face \
                 the camera. {COORDS}"
            ),
            obj(
                json!({
                    "template": {"type": "string", "description": "Template id or name from list_templates."},
                    "position": position_schema("Where to stand (default: the camera's placement spot)."),
                    "facing": {"type": ["string", "number"], "description": "\"camera\" (default), \"away\", \"left\" or \"right\" (screen directions), or degrees turned counter-clockwise seen from above."},
                    "name": {"type": "string", "description": "A name to refer to this actor by (shown in the cast list)."},
                    "action": {"type": ["string", "integer"], "description": "Starting action name or index (default: at rest)."},
                    "frame": frame_prop()
                }),
                &["template"],
            ),
        ),
        bridge(
            "add_3d_word",
            format!("Add 3D text that behaves like an actor (can move, spin, get bigger). Floats 1 m above the floor by default. {COORDS}"),
            obj(
                json!({
                    "text": {"type": "string", "maxLength": 60},
                    "shape": {"type": ["string", "integer"], "description": "Shape name/index from list_3d_word_styles (default normal)."},
                    "font": {"type": "string", "description": "Font id or name from list_3d_word_styles."},
                    "position": position_schema("Where to put it."),
                    "name": {"type": "string"},
                    "frame": frame_prop()
                }),
                &["text"],
            ),
        ),
        bridge(
            "select_actor",
            "Select an actor (some tools default to the selected actor).",
            obj(json!({"actor": actor_ref("")}), &["actor"]),
        ),
        bridge(
            "remove_actor",
            "Remove an actor. mode=delete removes it from the whole scene; mode=exit makes it leave the stage at \
             the current frame (it stays in earlier frames).",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "mode": {"type": "string", "enum": ["delete", "exit"], "default": "delete"},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "set_action",
            "Change what an actor is doing from the current frame on (it keeps looping the action until another \
             change). freeze=true holds a single pose. To make the action last a specific time, use animate.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "action": {"type": ["string", "integer"], "description": "Action name or index from list_actions."},
                    "freeze": {"type": "boolean", "default": false},
                    "frame": frame_prop()
                }),
                &["action"],
            ),
        ),
        bridge(
            "move_actor",
            format!(
                "Make an actor travel along a path starting at the current frame (or 'frame'), using a moving \
                 action such as walk or run. The actor turns to face where it is going, walks through each \
                 waypoint in order and stops at the last one. Speed comes from the action (see \
                 metres_per_frame in list_actions); the result reports how many frames it took, and the \
                 scene's current frame ends at the arrival frame. Recording replaces any later path the actor \
                 had. pause_frames on a waypoint makes it mark time there. {COORDS} {FRAMES}"
            ),
            obj(
                json!({
                    "actor": actor_ref(""),
                    "path": {
                        "type": "array",
                        "minItems": 1,
                        "description": "Waypoints. Each is {x, z} in stage metres (optionally pause_frames).",
                        "items": {
                            "type": "object",
                            "properties": {
                                "x": {"type": "number"},
                                "z": {"type": "number"},
                                "pause_frames": {"type": "integer", "minimum": 0}
                            }
                        }
                    },
                    "action": {"type": ["string", "integer"], "description": "Moving action (default: the current action)."},
                    "frame": frame_prop()
                }),
                &["path"],
            ),
        ),
        bridge(
            "animate",
            format!(
                "Make an actor perform an action on the spot for a duration (talk, laugh, wave, dance...), \
                 starting at the current frame (or 'frame'). This also makes the scene last at least that long. \
                 Use it for dialogue: animate the speaker with a talking action while attaching a speech sound. \
                 Like move_actor, this replaces whatever the actor was doing after the start frame, so build each \
                 actor's performance in time order. {FRAMES}"
            ),
            obj(
                json!({
                    "actor": actor_ref(""),
                    "action": {"type": ["string", "integer"]},
                    "seconds": {"type": "number", "exclusiveMinimum": 0},
                    "frames": {"type": "integer", "minimum": 1},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "place_actor",
            format!("Teleport an actor (its whole current path segment) so it stands at a position. {COORDS}"),
            obj(
                json!({
                    "actor": actor_ref(""),
                    "position": position_schema("New position."),
                    "frame": frame_prop()
                }),
                &["position"],
            ),
        ),
        bridge(
            "rotate_actor",
            "Turn an actor from the current frame on. turn = degrees counter-clockwise seen from above \
             (positive turns the actor to its own left). facing is a shortcut that assumes the actor currently \
             faces the camera: \"left\"/\"right\"/\"away\". tilt/roll rotate about the other axes. \
             this_frame_only applies the rotation to just this frame.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "turn": {"type": "number"},
                    "facing": {"type": ["string", "number"]},
                    "tilt": {"type": "number"},
                    "roll": {"type": "number"},
                    "this_frame_only": {"type": "boolean", "default": false},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "scale_actor",
            "Resize an actor from the current frame on. factor multiplies the current size (2 = twice as big); \
             total size is limited to 0.25x-10x of normal. Bigger actors also take bigger steps.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "factor": {"type": "number", "minimum": 0.1, "maximum": 10},
                    "frame": frame_prop()
                }),
                &["factor"],
            ),
        ),
        bridge(
            "set_costume",
            "Change a body part's costume (see list_costumes), either to one of its built-in costumes or by \
             painting a material on it.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "body_part": {"type": "integer", "minimum": 0},
                    "costume": {"type": "integer", "minimum": 0},
                    "material": {"type": "string", "description": "Material id or name from list_materials."},
                    "frame": frame_prop()
                }),
                &["body_part"],
            ),
        ),
        bridge(
            "set_name",
            "Rename an actor (affects the cast list and how you refer to it).",
            obj(json!({"actor": actor_ref(""), "name": {"type": "string"}}), &["name"]),
        ),
        bridge(
            "actor_lifetime",
            "Get the frames an actor is in the scene. shift_frames moves its whole timeline earlier (negative) or \
             later (positive), like the Studio's sooner/later tool.",
            obj(json!({"actor": actor_ref(""), "shift_frames": {"type": "integer"}}), &[]),
        ),
        // ---------------------------------------------------------------
        // Text and sound
        // ---------------------------------------------------------------
        bridge(
            "add_text",
            "Add a text box to the current scene from the current frame (or 'frame') until until_frame (default: \
             end of scene). kind caption = static text (titles, subtitles, speech captions); scrolling = rolls up \
             like credits during playback. rect is a preset (\"top\", \"bottom\", \"center\", \"full\") or pixels \
             {left, top, right, bottom} in the 544x306 movie view.",
            obj(
                json!({
                    "text": {"type": "string"},
                    "kind": {"type": "string", "enum": ["caption", "scrolling"], "default": "caption"},
                    "rect": {"description": "Preset name or {left, top, right, bottom} pixels."},
                    "font": {"type": "string", "description": "Font name; an error lists the available fonts."},
                    "size": {"type": "integer", "minimum": 6, "maximum": 200, "description": "Pixels."},
                    "color": {"description": "\"#rrggbb\", a color name, or [r,g,b]."},
                    "background": {"description": "Box fill color or \"transparent\" (default)."},
                    "bold": {"type": "boolean"},
                    "italic": {"type": "boolean"},
                    "underline": {"type": "boolean"},
                    "frame": frame_prop(),
                    "until_frame": {"type": "integer", "description": "First frame where the text is gone."}
                }),
                &["text"],
            ),
        ),
        bridge(
            "import_sound",
            "Copy a sound file from disk into the movie and get a sound_id (\"user:N\") for attach_sound. \
             sfx/speech take .wav or .mp3; music takes .mid. Tip: on macOS, `say -o line.wav \
             --data-format=LEI16@22050 \"text\"` makes a spoken line.",
            obj(
                json!({
                    "path": {"type": "string", "description": "Absolute path."},
                    "type": {"type": "string", "enum": ["sfx", "speech", "music"]},
                    "name": {"type": "string"}
                }),
                &["path", "type"],
            ),
        ),
        spec(
            "speak_line",
            Kind::SpeakLine,
            "Give an actor a line of dialogue: synthesizes the text as speech on this computer, imports it into \
             the movie, plays it on the actor at the current frame (or 'frame') and, unless action is \"none\", \
             makes the actor perform a talking action on the spot for exactly as long as the line lasts. Returns \
             the line's length and end_frame so you can start the next line there. Give different characters \
             different voices. There is no lip sync in 3D Movie Maker; the talking action stands in for it. Like \
             animate, it replaces what the actor did after the start frame, so script each actor in time order. \
             Voices (macOS): e.g. Fred, Ralph, Albert, Daniel, Junior, Kathy, Samantha, Bad News, Whisper.",
            obj(
                json!({
                    "actor": actor_ref("Who says it."),
                    "text": {"type": "string", "description": "The line, as it should be spoken."},
                    "voice": {"type": "string", "description": "Speech voice name (default: the system voice)."},
                    "action": {"type": ["string", "integer"], "description": "Talking action, e.g. \"Talk\" or \"Yell\" (default \"Talk\"; \"none\" to leave the actor alone)."},
                    "frame": frame_prop()
                }),
                &["actor", "text"],
            ),
        ),
        bridge(
            "attach_sound",
            "Play a sound starting at the current frame (or 'frame'). target \"scene\" (default) makes it a \
             background sound of the scene; an actor id/name attaches it to that actor (music can only go on the \
             scene). loop repeats it; queue plays it after the sound already there instead of replacing it.",
            obj(
                json!({
                    "sound": {"type": "string", "description": "Sound id/name from list_sounds, or a \"user:N\" id from import_sound."},
                    "target": {"type": ["string", "integer"], "default": "scene"},
                    "loop": {"type": "boolean", "default": false},
                    "queue": {"type": "boolean", "default": false},
                    "volume": {"type": "number", "minimum": 0, "maximum": 2, "description": "1 = normal."},
                    "motion_match": {"type": "boolean", "default": false, "description": "Actor sounds only: repeat in step with the action (e.g. footsteps)."},
                    "frame": frame_prop()
                }),
                &["sound"],
            ),
        ),
        // ---------------------------------------------------------------
        // Observe and playback
        // ---------------------------------------------------------------
        bridge(
            "describe_scene",
            "Describe the current scene at the current frame (or 'frame'): camera and visible floor area, every \
             actor (position, on-screen location and bounds, action, lifetime), text boxes and scene sounds.",
            obj(json!({"frame": frame_prop()}), &[]),
        ),
        spec(
            "render_frame",
            Kind::RenderImage,
            "Render a frame of the current scene exactly as it looks in the movie (3D plus text) and show it to \
             you as an image. Use it to check framing after placing or moving actors.",
            obj(
                json!({
                    "frame": frame_prop(),
                    "path": {"type": "string", "description": "Where to write the PNG (default: a temp file)."}
                }),
                &[],
            ),
        ),
        bridge(
            "export_frames",
            "Write a PNG image sequence of a scene's frames to a folder (e.g. to make a video with ffmpeg at 6 fps).",
            obj(
                json!({
                    "dir": {"type": "string", "description": "Existing folder, absolute path."},
                    "scene": {"type": "integer", "minimum": 0},
                    "from": {"type": "integer"},
                    "to": {"type": "integer"},
                    "step": {"type": "integer", "minimum": 1, "default": 1},
                    "prefix": {"type": "string", "default": "frame"}
                }),
                &["dir"],
            ),
        ),
        bridge(
            "play",
            "Play the movie in the app window from the current frame (or from_start) to the end, with sound. \
             wait=true returns when playback finishes.",
            obj(
                json!({
                    "from_start": {"type": "boolean", "default": false},
                    "scene": {"type": "integer"},
                    "frame": frame_prop(),
                    "wait": {"type": "boolean", "default": false},
                    "timeout_seconds": {"type": "number", "default": 120}
                }),
                &[],
            ),
        ),
        bridge("stop", "Stop playback in the app window (does nothing if the movie is not playing).", obj(json!({}), &[])),
    ];

    for tool in &mut tools {
        match tool.name {
            "play" => tool.timeout_secs = 15 * 60,
            "export_frames" => tool.timeout_secs = 10 * 60,
            "goto_studio" | "launch" => tool.timeout_secs = 120,
            _ => {}
        }
    }
    tools
}

/// Short workflow guide, served as an MCP resource and prompt.
pub const WORKFLOW_GUIDE: &str = r#"# Making a movie with 3D Movie Maker

1. **Catalog** — `list_backgrounds` (pick a set and camera), `list_templates` (actors and props), `list_actions` for each actor you'll use, `list_sounds`.
2. **Scene** — `add_scene(background, camera)`. A new movie has no scenes; add one before anything else. Each scene has its own frame numbers starting at 1. The movie runs at 6 frames per second.
3. **Actors** — `add_actor(template, position, facing, name)`. Positions are stage metres: x right/left, z away/toward the camera, origin = the set's default spot. Check `describe_scene` → camera.visible_floor for what's in shot.
4. **Motion and acting** —
   - `move_actor(actor, path=[{x,z},...], action="Walk Fast")` walks a path from the current frame and reports the arrival frame.
   - `animate(actor, action, seconds)` does an action on the spot (talking, laughing, dancing) and extends the scene.
   - `set_action` changes the action from a frame on; `rotate_actor` turns.
   - Edits happen at the scene's current frame: use the `frame` argument (or `goto_frame`) to place things in time. Actors stay on stage until the scene ends.
5. **Sound and text** — `attach_sound(sound, target=actor|"scene", frame)`; `import_sound(path)` for your own WAV/MP3 (e.g. `say -o line.wav --data-format=LEI16@22050 "Hello"` on macOS). `add_text(text, kind="caption", rect="bottom", frame, until_frame)` for titles and subtitles.
6. **Check** — `render_frame(frame)` shows you the picture; fix framing with `place_actor` or `set_camera`. `describe_scene` lists everyone's position and lifetime.
7. **More scenes** — `add_scene` again; `set_transition` on the earlier scene for a fade/dissolve.
8. **Save** — `save_movie(path=".../name.3mm")`. Optionally `export_frames` for a PNG sequence, or `play(from_start=true)` to watch it in the app.

Tips
- Lengths: a scene lasts as long as its longest-living actor. A 30 s movie at 6 fps is 180 frames across all scenes.
- Speed comes from the action: "Walk Slowly" < "Walk Fast" < "Run" (names vary by actor; check list_actions). Bigger actors step further.
- Dialogue: there is no lip sync; animate the speaker with a talking/gesturing action while the speech sound plays, and add a caption.
- One level of undo; saving clears it.
"#;
