//! Checks on the MCP tool table that don't need the app.

use std::collections::HashSet;

use serde_json::Value;
use threedmm_mcp::tools::{all_tools, Kind};

/// Commands the bridge implements (src/automation/*.cpp command tables).
const BRIDGE_COMMANDS: &[&str] = &[
    "hello",
    "get_state",
    "goto_studio",
    "new_movie",
    "open_movie",
    "save_movie",
    "undo",
    "redo",
    "quit",
    "list_backgrounds",
    "list_cameras",
    "list_templates",
    "list_actions",
    "list_costumes",
    "list_materials",
    "list_sounds",
    "list_3d_word_styles",
    "add_scene",
    "set_background",
    "select_scene",
    "set_camera",
    "set_transition",
    "delete_scene",
    "goto_frame",
    "add_pause",
    "add_actor",
    "add_3d_word",
    "select_actor",
    "remove_actor",
    "set_action",
    "move_actor",
    "animate",
    "place_actor",
    "rotate_actor",
    "scale_actor",
    "set_costume",
    "set_name",
    "actor_lifetime",
    "add_text",
    "import_sound",
    "attach_sound",
    "describe_scene",
    "render_frame",
    "preview_cameras",
    "describe_timeline",
    "checkpoint",
    "restore_checkpoint",
    "list_checkpoints",
    "export_frames",
    "export_media",
    "storyboard",
    "suggest_camera",
    "move_scene",
    "duplicate_scene",
    "import_scenes",
    "trim_scene",
    "edit_text",
    "remove_text",
    "remove_sound",
    "play",
    "stop",
];

#[test]
fn tool_names_are_unique_and_map_to_bridge_commands() {
    let mut seen = HashSet::new();
    for tool in all_tools() {
        assert!(seen.insert(tool.name), "duplicate tool {}", tool.name);
        if let Kind::Bridge(cmd) | Kind::BridgeImage(cmd) = tool.kind {
            assert!(
                BRIDGE_COMMANDS.contains(&cmd),
                "{} forwards to unknown command {cmd}",
                tool.name
            );
        }
    }
}

#[test]
fn schemas_are_objects_with_known_required_fields() {
    for tool in all_tools() {
        let schema = &tool.schema;
        assert_eq!(schema["type"], "object", "{}", tool.name);
        let properties = schema["properties"].as_object().expect("properties");
        if let Some(required) = schema.get("required").and_then(Value::as_array) {
            for field in required {
                let field = field.as_str().unwrap();
                assert!(
                    properties.contains_key(field),
                    "{}: required '{field}' not in properties",
                    tool.name
                );
            }
        }
        assert!(
            tool.description.len() > 20,
            "{} needs a real description",
            tool.name
        );
    }
}
