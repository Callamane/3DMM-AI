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
    /// Forward to a bridge command that writes a PNG ("path" in its result)
    /// and return the picture as image content.
    BridgeImage(&'static str),
    /// Run several tool calls in order.
    Batch,
    /// Export the movie as a video file (bridge export_media + ffmpeg).
    ExportVideo,
    /// Synthesize a spoken line, import it and (optionally) attach and act it.
    SpeakLine,
    /// Build a whole scene from a script (MCP side, composed of bridge calls).
    ScriptScene,
    /// Build a whole movie from scripted scenes, optionally exporting it.
    ScriptMovie,
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

const FRAMES: &str =
    "Movies run at 6 frames per second. Each scene has its own frame numbers starting at 1.";

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

/// The fields of one scripted scene (script_scene, and each of
/// script_movie's scenes)
fn scene_script_properties() -> Value {
    json!({
        "background": {"type": "string", "description": "Set name or a word from its description (\"diner\", \"haunted\")."},
        "camera": {"type": "integer", "minimum": 0, "description": "The camera the scene is staged for (left and right, blocking); auto_camera re-cuts anyway. Left out, the set's own camera is used, and if that leaves warnings the scene is restaged on up to 4 other cameras and the cleanest build kept (a note says which)."},
        "title": {"type": "string", "description": "Caption at the top for as long as it takes to read."},
        "cast": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "actor": {"type": "string", "description": "Actor template or a description word (list_templates)."},
                    "voice": {"type": "string", "description": "Speech voice; default: a different one for each character."}
                },
                "required": ["name", "actor"]
            }
        },
        "lines": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "who": {"type": "string"},
                    "says": {"type": "string"},
                    "action": {"type": ["string", "integer"], "description": "With says: how to act it (default from mood, else Talk)."},
                    "mood": {"type": "string", "enum": ["angry", "excited", "happy", "sad", "scared", "whisper", "thinking", "shout"], "description": "With says: delivery: the gesture (angry/shout yell, sad droops, thinking thinks) and the voice's speed, volume and pitch."},
                    "to": {"type": "string", "description": "With says: who it's said to (default: the nearest character)."},
                    "does": {"type": ["string", "integer"], "description": "An action to do instead of speaking."},
                    "seconds": {"type": "number", "description": "With does: how long (default 2)."},
                    "wait": {"type": "number", "description": "A pause in seconds."},
                    "enters": {"type": "string", "enum": ["left", "right"], "description": "Walks on from off-screen to their place (they start off stage)."},
                    "exits": {"type": "string", "enum": ["left", "right"], "description": "Walks off-screen and leaves the stage."},
                    "walks_to": {"description": "A character (stops beside them and faces them) or a stage point {x, z}."},
                    "throws": {"type": "string", "description": "A prop to throw (\"Sphere\" makes a ball), with 'at'."},
                    "at": {"type": "string", "description": "With throws: who it's thrown at."},
                    "reaction": {"type": ["string", "integer"], "description": "With says: the listener's action over the end of the line (\"Laugh\", \"Think\", \"Mad, Sad\"). With throws: the target's action if hit."},
                    "hit_sound": {"type": "string", "description": "With throws: a sound effect on the hit (\"bump\")."},
                    "sound": {"type": "string", "description": "A sound effect by name (list_sounds type sfx), e.g. \"bell service\"; the next line doesn't wait for it."},
                    "on": {"type": "string", "description": "With sound: the character it comes from (default: the scene)."}
                }
            }
        },
        "layout": {"type": "string", "enum": ["conversation", "line"], "default": "conversation"},
        "camera_style": {"type": "string", "enum": ["coverage", "wide", "none"], "default": "coverage"},
        "transition": {"type": "string", "description": "Transition after this scene (cut, dissolve, fade_to_black...)."},
        "props": {"type": "array", "description": "Set dressing from the start: [{prop: \"Treasure chest\", beside: \"Mia\"} or {prop, at: {x, z}}, optional name]. list_templates type prop shows them."},
        "pace": {"type": "string", "enum": ["brisk", "natural", "slow"], "default": "natural", "description": "Breathing room after each line."},
        "subtitles": {"type": "boolean", "default": false, "description": "Caption each spoken line at the bottom while it is said."},
        "music": {"type": "string", "description": "Scene music by name or mood word (list_sounds type music: \"funny moment\", \"tense\", \"sudden danger\"...), or \"none\" to silence the set's own."}
    })
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

/// Checks arguments against the tool's schema before anything runs: an
/// unknown name (a typo, or `scene` where the tool takes `index`) would
/// otherwise be ignored and the tool would act on the default target.
/// Whether a value matches a JSON-schema "type" (a name or a list of names;
/// absent means anything). Whole numbers written as 3.0 count as integers.
fn value_has_type(value: &Value, expected: &Value) -> bool {
    let one = |t: &str| match t {
        "integer" => value.as_i64().is_some() || value.as_f64().is_some_and(|f| f.fract() == 0.0),
        "number" => value.is_number(),
        "boolean" => value.is_boolean(),
        "string" => value.is_string(),
        "array" => value.is_array(),
        "object" => value.is_object(),
        _ => true,
    };
    match expected {
        Value::String(t) => one(t),
        Value::Array(ts) => ts.iter().filter_map(Value::as_str).any(one),
        _ => true,
    }
}

fn describe_type(expected: &Value) -> String {
    match expected {
        Value::String(t) => format!(
            "a{} {t}",
            if t.starts_with(['a', 'e', 'i', 'o', 'u']) {
                "n"
            } else {
                ""
            }
        ),
        Value::Array(ts) => ts
            .iter()
            .filter_map(Value::as_str)
            .collect::<Vec<_>>()
            .join(" or "),
        _ => "anything".into(),
    }
}

pub fn check_args(spec: &ToolSpec, args: &Value) -> Result<(), String> {
    let Some(properties) = spec.schema.get("properties").and_then(Value::as_object) else {
        return Ok(());
    };
    let takes = || {
        let mut names: Vec<&str> = properties.keys().map(String::as_str).collect();
        names.sort_unstable();
        if names.is_empty() {
            "no arguments".to_string()
        } else {
            names.join(", ")
        }
    };
    if let Some(args) = args.as_object() {
        let mut unknown: Vec<&str> = args
            .keys()
            .map(String::as_str)
            .filter(|name| !properties.contains_key(*name))
            .collect();
        if !unknown.is_empty() {
            unknown.sort_unstable();
            return Err(format!(
                "{} doesn't take {}; it takes: {}",
                spec.name,
                unknown
                    .iter()
                    .map(|name| format!("'{name}'"))
                    .collect::<Vec<_>>()
                    .join(", "),
                takes()
            ));
        }
    }
    if let Some(args) = args.as_object() {
        for (name, value) in args {
            let expected = &properties[name]["type"];
            if !value_has_type(value, expected) {
                return Err(format!(
                    "{}: '{name}' must be {}, not {value}",
                    spec.name,
                    describe_type(expected)
                ));
            }
        }
    }
    let missing: Vec<&str> = spec
        .schema
        .get("required")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|name| args.get(*name).is_none())
        .collect();
    if !missing.is_empty() {
        return Err(format!(
            "{} needs {}; it takes: {}",
            spec.name,
            missing.join(", "),
            takes()
        ));
    }
    Ok(())
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
        bridge(
            "checkpoint",
            "Snapshot the movie under a name before trying something risky (there is only one level of undo). \
             restore_checkpoint goes back to it. Doesn't change where save_movie writes. Clears undo.",
            obj(json!({"name": {"type": "string", "maxLength": 40}}), &["name"]),
        ),
        bridge(
            "restore_checkpoint",
            "Throw away changes since a checkpoint and return to it. A later save_movie without a path saves to \
             the movie's original file.",
            obj(json!({"name": {"type": "string"}}), &["name"]),
        ),
        bridge("list_checkpoints", "List the checkpoints made in this session.", obj(json!({}), &[])),
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
            "List actors (people/creatures that walk and act, each with a short description of how they look) \
             and props (objects). filter matches names and descriptions (e.g. \"man\", \"robot\"). Actors differ \
             a lot in what they can do (some cannot talk or run), so check list_actions before casting. Use the \
             id or name with \
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
            "List what an actor or prop can do. travels=true marks the walking/running family (shown with * in \
             their names; use them with move_actor, and metres_per_frame says how fast they go); the rest are done \
             on the spot with animate or speak_line (talk, laugh, dance, ...).",
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
            "move_scene",
            "Reorder scenes: move scene 'from' so it ends up at index 'to' (0-based).",
            obj(json!({"from": {"type": "integer", "minimum": 0}, "to": {"type": "integer", "minimum": 0}}), &["from", "to"]),
        ),
        bridge(
            "duplicate_scene",
            "Copy a scene (default: the current one) and insert the copy right after it, e.g. to make a variation \
             or a second take. The copy's actors get new ids.",
            obj(json!({"index": {"type": "integer", "minimum": 0}}), &[]),
        ),
        bridge(
            "import_scenes",
            "Insert scenes from another .3mm movie into this one (all of them, or the listed indexes), at \
             position 'at' (default: the end). Use it to combine movies or reuse a scene.",
            obj(
                json!({
                    "path": {"type": "string", "description": "Absolute path of the .3mm to take scenes from."},
                    "scenes": {"type": "array", "items": {"type": "integer"}, "description": "Scene indexes in that movie (default: all)."},
                    "at": {"type": "integer", "minimum": 0}
                }),
                &["path"],
            ),
        ),
        bridge(
            "trim_scene",
            "Cut the current scene down to frames keep_from..keep_to, deleting everything before and after \
             (the Studio's chop tools). Frame numbers can shift; check with describe_timeline.",
            obj(
                json!({"keep_from": {"type": "integer", "minimum": 1}, "keep_to": {"type": "integer", "minimum": 1}}),
                &[],
            ),
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
                    "facing": {"type": ["string", "number"], "description": "\"camera\" (default), \"away\", \"left\" or \"right\", or degrees turned counter-clockwise seen from above."},
                    "action": {"type": ["string", "integer"], "description": "Starting action name or index from list_actions (e.g. a spin)."},
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
                 metres_per_frame in list_actions; pick an action with travels=true); the result reports how many frames it took, and the \
                 scene's current frame ends at the arrival frame. Recording replaces any later path the actor \
                 had. pause_frames on a waypoint makes it mark time there. Afterwards the actor goes to its rest \
                 pose (then=\"freeze\" holds the last stride instead). Tip: render_frame with overlay=true \
                 draws the floor grid so you can pick waypoints. {COORDS} {FRAMES}"
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
                    "then": {"type": "string", "enum": ["rest", "freeze"], "default": "rest", "description": "Afterwards: return to the rest pose, or hold the last pose."},
                    "avoid_actors": {"type": "boolean", "default": true, "description": "Step around anyone standing in the way (where they are at the start frame), passing on the camera side when it's a toss-up. The result reports detours."},
                    "frame": frame_prop()
                }),
                &["path"],
            ),
        ),
        bridge(
            "physics",
            "Make an actor or prop move under gravity from the current frame: drop it (from 'height'), throw it \
             at a point or another actor ('to', arcing 'height' metres up), or launch it with a 'velocity'. It \
             bounces off the floor and off other actors, slides to a stop with friction and settles; hard \
             landings get a squash-and-stretch, and spin can tumble or twirl it. Baked into ordinary frames, so \
             the movie plays the same in any 3D Movie Maker. Returns each impact (frame, speed, what was hit) so \
             you can attach_sound a bump or animate a reaction on that frame. Like move_actor, it replaces what \
             the actor did after the start frame. Sphere, Cube and the other props make good balls and crates.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "motion": {"type": "string", "enum": ["drop", "throw", "launch"], "description": "Default: throw if 'to' is given, launch if 'velocity' is, else drop."},
                    "to": {"description": "Throw target: a stage point {x, z} or an actor id/name (it will hit them)."},
                    "height": {"type": "number", "minimum": 0, "description": "Drop: start height in metres (default 1.5). Throw: how high the arc rises above the start (default 1)."},
                    "velocity": {"type": "object", "description": "Launch: {x, y, z} metres per second (x right, y up, z away)."},
                    "bounciness": {"type": "number", "minimum": 0, "maximum": 0.95, "default": 0.5},
                    "friction": {"type": "number", "minimum": 0, "maximum": 2, "default": 0.4},
                    "spin": {"type": "string", "enum": ["none", "tumble", "twirl"], "default": "none"},
                    "squash": {"type": "boolean", "default": true, "description": "Squash and stretch on hard landings."},
                    "collide": {"type": "boolean", "default": true, "description": "Bounce off other actors."},
                    "action": {"type": ["string", "integer"], "description": "What the actor does meanwhile (default: its current action)."},
                    "max_seconds": {"type": "number", "minimum": 0.5, "maximum": 30, "default": 20},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "animate",
            format!(
                "Make an actor perform an action on the spot for a duration (talk, laugh, wave, dance...), \
                 starting at the current frame (or 'frame'). This also makes the scene last at least that long. \
                 Use it for dialogue: animate the speaker with a talking action while attaching a speech sound. \
                 Like move_actor, this replaces whatever the actor was doing after the start frame, so build each \
                 actor's performance in time order. Afterwards the actor returns to rest (then=\"freeze\" holds \
                 the last pose). To keep an action going longer, animate for longer. {FRAMES}"
            ),
            obj(
                json!({
                    "actor": actor_ref(""),
                    "action": {"type": ["string", "integer"]},
                    "seconds": {"type": "number", "exclusiveMinimum": 0},
                    "frames": {"type": "integer", "minimum": 1},
                    "then": {"type": "string", "enum": ["rest", "freeze"], "default": "rest", "description": "Afterwards: return to the rest pose, or hold the last pose."},
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
            "arrange_actors",
            "Block a group of actors in one call, at the current frame. layout conversation (default) stands them \
             on a shallow arc, partners turned toward each other but open to the camera; line stands them in a \
             row facing the camera. Starts close to the camera and moves the group back or sideways until \
             everyone is fully in shot and at least 80% visible (not hidden by furniture or each other); \
             among the spots that work (up to one step further back) it picks the one where the most other \
             cameras see someone's face, so auto_camera has angles to cut to, and lists them in other_angles. \
             Reports the spot used and a framing note if nothing worked. Do it before animating: it moves each \
             actor's whole current path segment.",
            obj(
                json!({
                    "actors": {"type": "array", "items": {"type": ["integer", "string"]}, "description": "Actor ids or names. Default: everyone on stage."},
                    "layout": {"type": "string", "enum": ["conversation", "line"], "default": "conversation"},
                    "spacing": {"type": "number", "minimum": 0.3, "maximum": 5, "default": 1, "description": "Metres between neighbours."},
                    "x": {"type": "number", "description": "Fix the group's centre (stage metres); default lets it slide to avoid furniture."},
                    "z": {"type": "number", "description": "Fix the group's depth (stage metres); default finds the closest that fits."},
                    "coverage": {"type": "boolean", "default": true, "description": "Prefer a spot other cameras also cover (false: simply the closest that fits)."},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "rotate_actor",
            "Turn an actor from the current frame on. face turns it to look somewhere, whatever way it faces \
             now: \"camera\", \"away\", \"left\"/\"right\" (screen directions), another actor (so two \
             characters look at each other), or a stage point {x, z}. turn = extra degrees counter-clockwise \
             seen from above. tilt/roll rotate about the other axes. this_frame_only applies the rotation to \
             just this frame. describe_scene reports each actor's facing.",
            obj(
                json!({
                    "actor": actor_ref(""),
                    "face": {"description": "\"camera\", \"away\", \"left\", \"right\", degrees from facing the camera (+90 = screen right), an actor id/name, or {x, z}."},
                    "turn": {"type": "number"},
                    "facing": {"type": ["string", "number"], "description": "Older name for face."},
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
                    "color": {"description": "\"#rrggbb\", a color name, [r,g,b], or \"auto\" (dark text over a bright part of the picture, yellow over a dark one)."},
                    "background": {"description": "Box fill color or \"transparent\" (default)."},
                    "bold": {"type": "boolean"},
                    "italic": {"type": "boolean"},
                    "underline": {"type": "boolean"},
                    "align": {"type": "string", "enum": ["left", "center", "right"], "description": "Default: center for the top/bottom/center presets, left otherwise."},
                    "frame": frame_prop(),
                    "until_frame": {"type": "integer", "description": "First frame where the text is gone."}
                }),
                &["text"],
            ),
        ),
        bridge(
            "edit_text",
            "Change an existing text box (index from describe_scene/describe_timeline): replace its text and/or \
             change its colour, size and alignment. The box keeps its place and timing.",
            obj(
                json!({
                    "index": {"type": "integer", "minimum": 0},
                    "text": {"type": "string"},
                    "color": {"description": "\"#rrggbb\", a colour name, or [r,g,b]."},
                    "size": {"type": "integer", "minimum": 6, "maximum": 200},
                    "align": {"type": "string", "enum": ["left", "center", "right"]}
                }),
                &["index"],
            ),
        ),
        bridge(
            "remove_text",
            "Delete a text box from the current scene (index from describe_scene/describe_timeline).",
            obj(json!({"index": {"type": "integer", "minimum": 0}}), &["index"]),
        ),
        bridge(
            "remove_sound",
            "Remove sounds of one type that start at a frame, from the scene (target \"scene\") or an actor. \
             describe_timeline lists when sounds start (sounds_start_at, scene_sounds_start_at).",
            obj(
                json!({
                    "type": {"type": "string", "enum": ["sfx", "speech", "music"]},
                    "target": {"type": ["string", "integer"], "default": "scene"},
                    "frame": frame_prop(),
                    "motion_match": {"type": "boolean", "default": false}
                }),
                &["type"],
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
            "script_scene",
            Kind::ScriptScene,
            "Build a whole scene from a script in one call: adds the scene on the set, casts the characters, \
             blocks them (arrange_actors), plays every line in order (synthesized speech acted with a talking \
             action, or an action for so many seconds, or a pause), adds a title, edits the camera cuts \
             (auto_camera) and reviews the result (review_movie), returning the timing of each line, the shot \
             list and any problems to fix. Characters without a voice each get a different one. If the movie \
             already has scenes it checkpoints first. Afterwards everything can be refined with the normal \
             tools. Example lines: {\"who\": \"Frank\", \"says\": \"Those are my fries.\"}, \
             {\"who\": \"Greg\", \"does\": \"Laugh\", \"seconds\": 2}, {\"wait\": 1}, \
             {\"who\": \"Rita\", \"enters\": \"left\"}, {\"who\": \"Greg\", \"walks_to\": \"Rita\"}, \
             {\"who\": \"Rita\", \"throws\": \"Sphere\", \"at\": \"Greg\", \"reaction\": \"Mad\"}, \
             {\"who\": \"Frank\", \"exits\": \"right\"}, {\"sound\": \"bell service\"}. Walks step around people; \
             throws use physics. subtitles: true captions the dialogue; music picks the score.",
            obj(
                {
                    let mut properties = scene_script_properties();
                    properties["preview"] = json!({"type": "boolean", "default": true, "description": "Return a storyboard of the new scene (one picture) with the result."});
                    properties["replace"] = json!({"type": "integer", "minimum": 0, "description": "Rebuild this existing scene in place from the script (to revise a scene: edit the script and run it again). The old version is checkpointed first."});
                    properties
                },
                &["background", "cast", "lines"],
            ),
        ),
        spec(
            "script_movie",
            Kind::ScriptMovie,
            "Build a whole movie in one call: 'scenes' is a list of scene scripts, each exactly like a \
             script_scene call (background, cast, lines, subtitles, music, transition...). Every scene is \
             checked before anything is built (errors name the scene and line); a character keeps the same \
             actor and voice in every scene. Returns each scene's report (timings, shots, review), the total \
             length, and, with 'export', the MP4. target_seconds adds a note if the length is far off.",
            obj(
                json!({
                    "scenes": {"type": "array", "minItems": 1, "items": {"type": "object", "properties": scene_script_properties(), "required": ["background", "cast", "lines"]}},
                    "target_seconds": {"type": "number", "minimum": 1},
                    "export": {"type": "string", "description": "Absolute .mp4 path to export the finished movie to."},
                    "export_max_megabytes": {"type": "number", "minimum": 1, "description": "With export: keep the MP4 under this size."},
                    "title_card": {"description": "Opening card: the words (\"THE MAP\") or {text, background (default black), action (3D word action, default \"Spin Letters\"; also \"The Wave\", \"Hop\", \"Grow and Shrink\", \"Cartwheel Letters\"...), seconds (3.5), music, caption}. The words become an animated 3D word, the classic 3D Movie Maker title."},
                    "end_card": {"description": "Closing card, same form (\"THE END\"; default action \"The Wave\")."},
                    "preview": {"type": "boolean", "default": true, "description": "Return a storyboard of the whole movie (one picture) with the result."}
                }),
                &["scenes"],
            ),
        ),
        spec(
            "speak_line",
            Kind::SpeakLine,
            "Give an actor a line of dialogue: synthesizes the text as speech on this computer, imports it into \
             the movie, plays it on the actor at the current frame (or 'frame') and, unless action is \"none\", \
             makes the actor perform a talking action on the spot for exactly as long as the line lasts. The \
             scene's current frame ends at the line's end, so consecutive speak_line calls without 'frame' play \
             one after another (end_frame is also returned). Actors that can't talk stand still while speaking. Give different characters \
             different voices. There is no lip sync in 3D Movie Maker; the talking action stands in for it. Like \
             animate, it replaces what the actor did after the start frame, so script each actor in time order. \
             Voices (macOS): e.g. Fred, Ralph, Albert, Daniel, Junior, Kathy, Samantha, Bad News, Whisper.",
            obj(
                json!({
                    "actor": actor_ref("Who says it."),
                    "text": {"type": "string", "description": "The line, as it should be spoken."},
                    "voice": {"type": "string", "description": "Speech voice name (default: the system voice)."},
                    "mood": {"type": "string", "enum": ["angry", "excited", "happy", "sad", "scared", "whisper", "thinking", "shout"], "description": "Delivery: picks the gesture (unless action is given) and changes the voice's speed, volume and pitch."},
                    "action": {"type": ["string", "integer"], "description": "Talking action, e.g. \"Talk\" or \"Yell\" (default \"Talk\"; \"none\" to leave the actor alone, e.g. talking while walking)."},
                    "then": {"type": "string", "enum": ["rest", "freeze"], "default": "rest", "description": "Afterwards: return to the rest pose, or hold the last pose."},
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
            "Describe the current scene at the current frame (or 'frame'): camera and visible floor area (stage \
             x/z of the floor at the bottom and middle of the screen; feet between those rows are comfortably \
             in shot), every actor (position, on-screen location and bounds, action, lifetime), text boxes and \
             scene sounds. verbose=true adds the raw camera matrix.",
            obj(json!({"frame": frame_prop(), "verbose": {"type": "boolean", "default": false}}), &[]),
        ),
        spec(
            "render_frame",
            Kind::BridgeImage("render_frame"),
            "Render a frame of the current scene exactly as it looks in the movie (3D plus text) and show it to \
             you as an image. Use it to check framing after placing or moving actors. overlay=true draws the \
             floor grid of stage coordinates (lines every metre, labelled X=… and Z=…) and labels each actor \
             \"#id name\": the quickest way to see where positions land and whether furniture hides someone.",
            obj(
                json!({
                    "frame": frame_prop(),
                    "overlay": {"type": "boolean", "default": false},
                    "path": {"type": "string", "description": "Where to write the PNG (default: a temp file)."}
                }),
                &[],
            ),
        ),
        spec(
            "preview_cameras",
            Kind::BridgeImage("preview_cameras"),
            "Show every camera angle of a background on one contact sheet (cells labelled CAM n), so you can \
             pick angles at a glance. Without 'background' it previews the current scene's set, actors included; \
             with one, the set is shown empty and nothing in the movie changes.",
            obj(
                json!({
                    "background": {"type": "string", "description": "Background id or name (default: the current scene's)."},
                    "cameras": {"type": "array", "items": {"type": "integer"}, "description": "Only these camera indexes."}
                }),
                &[],
            ),
        ),
        spec(
            "storyboard",
            Kind::BridgeImage("storyboard"),
            "See a whole scene at once: frames sampled evenly across it (default 12) on one image, each \
             labelled with its frame number and time. The quickest way to review a scene you built. \
             all_scenes: true samples the whole movie instead (cells shared out by scene length, labelled \
             S<scene> F<frame>), the closest thing to watching it.",
            obj(
                json!({
                    "all_scenes": {"type": "boolean", "default": false, "description": "Sample the whole movie (ignores scene, from and to)."},
                    "scene": {"type": "integer", "minimum": 0, "description": "Default: the current scene."},
                    "count": {"type": "integer", "minimum": 2, "maximum": 24, "default": 12},
                    "from": {"type": "integer", "minimum": 1},
                    "to": {"type": "integer", "minimum": 1}
                }),
                &[],
            ),
        ),
        bridge(
            "suggest_camera",
            "Rank this background's cameras by how well they show the actors at a frame: how many are fully in \
             shot, how large they appear, how centred the group is. It can't see furniture blocking the view, \
             so confirm the pick with render_frame. Apply it with set_camera.",
            obj(
                json!({
                    "actors": {"type": "array", "items": {"type": ["integer", "string"]}, "description": "Actors to frame (default: everyone on stage)."},
                    "frame": frame_prop()
                }),
                &[],
            ),
        ),
        bridge(
            "describe_timeline",
            "Summarise what happens when in a scene: camera changes, each actor's segments (frames, action, \
             where they stand or from/to where they move, when they're off stage), when text boxes are visible \
             and when sounds start. Use it to check timing and continuity without rendering every frame. \
             Positions use the stage coordinates of the scene's opening camera.",
            obj(json!({"scene": {"type": "integer", "minimum": 0, "description": "Default: the current scene."}}), &[]),
        ),
        bridge(
            "auto_camera",
            "Edit a scene's camera cuts like a film editor. The scene is split at each line of dialogue (each \
             time an actor starts a speech sound, or at the beats given); every one of the set's camera angles is tried on every line and the \
             cuts that best show the speaker (in shot, not hidden, facing us, a good size, near the middle) win, \
             with a cost per cut so it only cuts when the new angle is clearly better and never leaves a shot \
             under min_shot_seconds. style coverage (default) opens wide then goes to singles of whoever speaks \
             (shot / reverse shot); wide keeps everyone in frame. Replaces the scene's existing cuts. Returns the \
             shot list, with a warning on shots where no angle shows the speaker's face. apply=false previews.",
            obj(
                json!({
                    "scene": {"type": "integer", "minimum": 0, "description": "Default: the current scene."},
                    "style": {"type": "string", "enum": ["coverage", "wide"], "default": "coverage"},
                    "min_shot_seconds": {"type": "number", "minimum": 0, "maximum": 30, "default": 2},
                    "cut_penalty": {"type": "number", "minimum": 0, "maximum": 10, "description": "Higher means fewer cuts (style default: coverage 0.4, wide 1.2)."},
                    "cameras": {"type": "array", "items": {"type": "integer"}, "description": "Only use these camera angles."},
                    "apply": {"type": "boolean", "default": true},
                    "verbose": {"type": "boolean", "default": false, "description": "Also return every line's score for every camera."},
                    "beats": {"type": "array", "items": {"type": "object", "properties": {"frame": {"type": "integer"}, "actor": {"type": ["string", "null"]}, "speaks": {"type": "boolean", "default": true}, "camera": {"type": "integer", "description": "Only this camera for the beat (an entrance or exit staged for it keeps its screen direction)."}}, "required": ["frame"]}, "description": "Split the scene here instead of at each sound: each beat is a frame and the actor it is about (null for everyone), in frame order; speaks false for an action, where their back to the camera is no fault; camera pins a beat to one angle. Give one per line and per action so an action after a line (someone running off) gets its own shot. script_scene passes these itself."}
                }),
                &[],
            ),
        ),
        bridge(
            "review_movie",
            "Check the whole movie for problems a viewer would notice, without looking at every frame: actors \
             out of shot, cut off by the frame edge or hidden behind someone nearer the camera (flagged hardest \
             while they speak), text too short to read, stretches over 8 s where nothing changes, empty or very \
             short scenes, no sound at all. Each issue gives its scene, frames, actor, and a fix to try. Run it \
             before saving or exporting, fix the warnings, and run it again.",
            obj(json!({"scene": {"type": "integer", "minimum": 0, "description": "Review only this scene. Default: every scene."}}), &[]),
        ),
        spec(
            "export_video",
            Kind::ExportVideo,
            "Export the movie as an MP4 video with sound: every scene rendered frame by frame, joined with \
             the scenes' transitions (dissolve, fade to black/white), and all sounds mixed in at their frames \
             (dialogue, effects, imported sounds, and background music rendered from MIDI). Plays anywhere, \
             unlike the .3mm. Needs ffmpeg installed. Takes a few seconds per scene.",
            obj(
                json!({
                    "path": {"type": "string", "description": "Absolute output path ending in .mp4."},
                    "scale": {"type": "integer", "enum": [1, 2, 3], "default": 2, "description": "Pixel scale of the 544x306 movie (2 = 1088x612)."},
                    "audio": {"type": "boolean", "default": true},
                    "transition_seconds": {"type": "number", "minimum": 0, "maximum": 5, "default": 1},
                    "max_megabytes": {"type": "number", "minimum": 1, "description": "Keep the file under this size (e.g. 25 for email and most chat apps): a two-pass encode at the bitrate that fits, instead of constant high quality."},
                    "smooth": {"type": "boolean", "default": false, "description": "Add motion-interpolated in-between frames for smoother movement than the movie's 6 fps (the .3mm is unchanged; fast limbs can smear a little)."},
                    "scene": {"type": "integer", "minimum": 0, "description": "Export only this scene."}
                }),
                &["path"],
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
        spec(
            "batch",
            Kind::Batch,
            "Run several tool calls in order in one go, e.g. adding a cast and blocking a scene. Each step is \
             {\"tool\": name, \"args\": {...}}. Stops at the first error unless stop_on_error is false. Returns \
             each step's result (and any images). Use it once you know what you want; use single calls while \
             exploring.",
            obj(
                json!({
                    "steps": {
                        "type": "array",
                        "minItems": 1,
                        "maxItems": 50,
                        "items": {
                            "type": "object",
                            "properties": {"tool": {"type": "string"}, "args": {"type": "object"}},
                            "required": ["tool"]
                        }
                    },
                    "stop_on_error": {"type": "boolean", "default": true}
                }),
                &["steps"],
            ),
        ),
        bridge("stop", "Stop playback in the app window (does nothing if the movie is not playing).", obj(json!({}), &[])),
    ];

    for tool in &mut tools {
        match tool.name {
            "play" => tool.timeout_secs = 15 * 60,
            "script_scene" => tool.timeout_secs = 10 * 60,
            "script_movie" => tool.timeout_secs = 30 * 60,
            "export_frames" | "export_video" => tool.timeout_secs = 20 * 60,
            "goto_studio" | "launch" | "preview_cameras" | "describe_timeline" | "storyboard"
            | "suggest_camera" | "review_movie" | "auto_camera" => tool.timeout_secs = 120,
            _ => {}
        }
    }
    tools
}

/// Short workflow guide, served as an MCP resource and prompt.
pub const WORKFLOW_GUIDE: &str = r#"# Making a movie with 3D Movie Maker

0. **Fastest start** — `script_movie(scenes: [...], export: ".../movie.mp4")` builds and exports a whole movie in one call; `script_scene(background, cast, lines)` does one scene (set, cast, blocking, dialogue, staging, sound, cuts, review). To revise a scene, edit its script and run `script_scene(replace: index)`. Then refine with the tools below.
1. **Plan** — movies run at 6 frames per second; a 30 s movie is ~180 frames across all scenes. Each scene has its own frame numbers from 1 and lasts as long as its longest-living actor.
2. **Pick a set** — `list_backgrounds` (each has a description; `filter: "diner"` works). `preview_cameras(background)` shows every camera angle on one image without changing the movie.
3. **Scene** — `add_scene(background, camera)`. A new movie has no scenes; add one before anything else.
4. **Cast** — `list_templates(type: "actor")` lists actors with a description of how they look; check `list_actions(template)` before casting (some can't talk or run; names vary, e.g. "Walk Fast *").
5. **Block** — `add_actor(template, position, facing, name)`, then `arrange_actors` to stand a group so everyone faces each other, fits the shot and isn't hidden by furniture (it slides the group as needed). `rotate_actor(face: "Greg")` turns one actor toward another, the camera or a point; `describe_scene` reports each actor's `facing` and `visible_fraction`. Positions are stage metres relative to the camera: x right/left, z away/toward, origin = the set's default spot. `render_frame(overlay: true)` draws the floor grid (X=/Z= lines every metre) and labels actors, so you can see exactly where positions land and whether furniture hides someone.
6. **Act** — work through each actor in time order (recording at a frame replaces what that actor did afterwards):
   - `move_actor(actor, path: [{x,z},...], action: "Walk Fast")` walks from the current frame and reports the arrival frame.
   - `speak_line(actor, text, voice, action: "Talk")` synthesizes the line, attaches it, and acts it for exactly its length; chain lines using the returned `end_frame`. Give each character its own voice.
   - `animate(actor, action, seconds)` does an action on the spot (laugh, dance, think) and extends the scene.
   - `physics(actor, to: "Greg")` throws something (a Sphere prop makes a ball) that arcs, bounces off people and the floor and rolls to a stop; `motion: "drop"` drops it. Use the returned impact frames for a bump sound or a reaction. Walks step around people by themselves (`avoid_actors`).
   - Actors return to their rest pose afterwards (`then: "freeze"` to hold the last pose).
7. **Camera and text** — once the acting is done, `auto_camera` edits the scene's cuts for you (wide establishing shot, then the best angle on whoever is speaking); fix any shot it warns about and run it again. `set_camera(camera, frame)` cuts by hand. `add_text(text, kind: "caption", rect: "top"|"bottom"|"center", frame, until_frame, size, color)` for titles and subtitles.
8. **Check** — `storyboard` shows a whole scene on one image; `suggest_camera` ranks angles for the actors in shot; `describe_timeline` summarises who does what when (cameras, actions, movement, text, sounds). `render_frame(frame)` shows a frame. `get_state(all_scenes: true)` gives every scene's length and the total. **`review_movie` checks everything at once** (actors out of shot, cut off or hidden while speaking, unreadable text, dead stretches) and says how to fix each problem; run it before saving and again after fixing.
9. **More scenes** — `add_scene` again (actors belong to one scene; add them again); `set_transition("dissolve")` on the earlier scene.
10. **Save and share** — `save_movie(path: ".../name.3mm")` keeps an editable movie; `export_video(path: ".../name.mp4")` makes a video with sound that plays anywhere. `play(from_start: true, wait: true)` shows it in the app.

Editing
- Scenes: `move_scene`, `duplicate_scene`, `import_scenes` (from another .3mm), `trim_scene(keep_from, keep_to)`, `delete_scene`.
- Text: `edit_text(index, text, color, size)`, `remove_text(index)`. Sound: `remove_sound(type, target, frame)`.

Safety and speed
- There is one level of undo. Before something risky, `checkpoint(name)`; `restore_checkpoint(name)` goes back.
- `batch(steps: [{tool, args}, ...])` runs several calls at once once you know what you want.
- No lip sync exists in 3D Movie Maker; talking actions stand in for it.
"#;
