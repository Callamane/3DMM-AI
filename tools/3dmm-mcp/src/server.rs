//! MCP server: exposes the tool table and forwards calls to the bridge.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use base64::Engine;
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, ContentBlock, GetPromptRequestParams,
    GetPromptResponse, GetPromptResult, Implementation, ListPromptsResult, ListResourcesResult,
    ListToolsResult, PaginatedRequestParams, Prompt, PromptArgument, PromptMessage,
    ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, Resource,
    ResourceContents, Role, ServerCapabilities, ServerConfig, Tool,
};
use rmcp::service::RequestContext;
use rmcp::{ErrorData as McpError, RoleServer, ServerHandler};
use serde_json::{json, Value};
use tokio::sync::Mutex;

use crate::bridge::{BridgeClient, BridgeError};
use crate::launcher;
use crate::script;
use crate::speech;
use crate::tools::{all_tools, Kind, ToolSpec, WORKFLOW_GUIDE};
use crate::video;

const GUIDE_URI: &str = "3dmm://guide/workflow";

const INSTRUCTIONS: &str = "Tools for making movies in Microsoft 3D Movie Maker (1995) through its automation \
bridge. Typical order: list_backgrounds/preview_cameras -> add_scene -> list_templates/list_actions -> add_actor -> \
render_frame(overlay) to block -> move_actor/speak_line/animate per actor in time order -> add_text -> \
review_movie (fix its warnings) and render_frame to check -> save_movie / export_video. Read the resource 3dmm://guide/workflow for details. \
The app starts automatically on the first call.";

pub struct ThreeDmmServer {
    port: u16,
    autolaunch: bool,
    tools: Arc<Vec<ToolSpec>>,
    by_name: HashMap<&'static str, usize>,
    client: Arc<Mutex<Option<BridgeClient>>>,
}

impl ThreeDmmServer {
    pub fn new(port: u16, autolaunch: bool) -> Self {
        let tools = all_tools();
        let by_name = tools.iter().enumerate().map(|(i, t)| (t.name, i)).collect();
        Self {
            port,
            autolaunch,
            tools: Arc::new(tools),
            by_name,
            client: Arc::new(Mutex::new(None)),
        }
    }

    /// Returns a connected client, starting the app if needed.
    async fn ensure_connected<'a>(
        &self,
        guard: &'a mut Option<BridgeClient>,
    ) -> anyhow::Result<&'a mut BridgeClient> {
        if guard.is_none() {
            match BridgeClient::connect(self.port).await {
                Ok(client) => *guard = Some(client),
                Err(err) if self.autolaunch => {
                    tracing::info!("3DMMEx not reachable ({err:#}); launching it");
                    let (client, exe) = launcher::launch(self.port).await?;
                    tracing::info!("launched {}", exe.display());
                    *guard = Some(client);
                }
                Err(err) => {
                    return Err(err.context(
                        "3DMMEx is not running with automation enabled (start it with --automation, or unset \
                         THREEDMM_NO_AUTOLAUNCH)",
                    ))
                }
            }
        }
        Ok(guard.as_mut().expect("client was just set"))
    }

    async fn bridge_call(
        &self,
        cmd: &str,
        args: Value,
        timeout: Duration,
    ) -> anyhow::Result<Value> {
        let mut guard = self.client.lock().await;
        let client = self.ensure_connected(&mut guard).await?;
        let result = client.call(cmd, args, timeout).await;
        if let Err(err) = &result {
            // Bridge errors are normal answers; anything else means the
            // connection is unusable, so reconnect next time
            if err.downcast_ref::<BridgeError>().is_none() {
                *guard = None;
            }
        }
        result
    }

    async fn launch(&self) -> anyhow::Result<Value> {
        let state = self
            .bridge_call("get_state", json!({}), Duration::from_secs(30))
            .await?;
        if state.get("screen").and_then(Value::as_str) != Some("studio") {
            self.bridge_call("goto_studio", json!({}), Duration::from_secs(120))
                .await?;
            return self
                .bridge_call("get_state", json!({}), Duration::from_secs(30))
                .await;
        }
        Ok(state)
    }

    async fn speak_line(
        &self,
        args: Value,
        timeout: Duration,
    ) -> anyhow::Result<Vec<ContentBlock>> {
        let result = self.speak_line_value(args, timeout).await?;
        Ok(vec![ContentBlock::text(result.to_string())])
    }

    /// speak_line's work, returning its result for callers that build on it
    async fn speak_line_value(&self, args: Value, timeout: Duration) -> anyhow::Result<Value> {
        let text = args
            .get("text")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        let voice = args.get("voice").and_then(Value::as_str);
        let actor = args.get("actor").cloned().unwrap_or(Value::Null);
        let action_given = args.get("action").cloned();
        let action = action_given.clone().unwrap_or_else(|| json!("Talk"));

        let wav = speech::synthesize(&text, voice).await?;
        let seconds = speech::wav_seconds(&wav)?;
        let label: String = text.chars().take(40).collect();
        let imported = self
            .bridge_call(
                "import_sound",
                json!({"path": wav, "type": "speech", "name": label}),
                timeout,
            )
            .await?;
        let _ = std::fs::remove_file(&wav);
        let sound_id = imported["sound_id"].clone();

        let mut attach = json!({"sound": sound_id, "target": actor});
        if let Some(frame) = args.get("frame") {
            attach["frame"] = frame.clone();
        }
        let attached = self.bridge_call("attach_sound", attach, timeout).await?;
        let start = attached["frame"].as_i64().unwrap_or(1);
        // Round up so the actor keeps talking until the line is over
        let frames = ((seconds * 6.0).ceil() as i64).max(1);

        let mut result = json!({
            "sound_id": sound_id,
            "seconds": (seconds * 100.0).round() / 100.0,
            "start_frame": start,
            "frames": frames,
            "end_frame": start + frames,
        });
        let then = args.get("then").cloned().unwrap_or_else(|| json!("rest"));
        let animate = |action: Value| json!({"actor": actor, "action": action, "frames": frames, "frame": start, "then": then});
        if action.as_str().map(str::to_lowercase).as_deref() == Some("none") {
            // The actor is left alone (e.g. it is walking meanwhile); move the
            // playhead past the line so the next one follows it
            let moved = self
                .bridge_call("goto_frame", json!({"frame": start + frames}), timeout)
                .await?;
            let last = moved["last_frame"].as_i64().unwrap_or(start + frames);
            if last < start + frames {
                result["note"] = json!(format!(
                    "the scene currently ends at frame {last}, so this line will be cut off unless something in \
                     the scene lasts until frame {}",
                    start + frames
                ));
            }
        } else {
            match self
                .bridge_call("animate", animate(action.clone()), timeout)
                .await
            {
                Ok(animated) => result["action"] = animated["action"].clone(),
                Err(err) if action_given.is_none() => {
                    // Not every actor can talk: hold the rest pose for the
                    // line instead, which also keeps the scene long enough
                    self.bridge_call("animate", animate(json!(0)), timeout)
                        .await?;
                    result["action"] = json!("At Rest");
                    result["note"] = json!(format!(
                        "this actor has no Talk action, so it stands still while speaking ({}); pass 'action' \
                         with one of its actions (list_actions) to animate it",
                        describe_error(&err)
                    ));
                }
                Err(err) => {
                    // The line is in; say why the acting part didn't happen
                    result["action_error"] = json!(describe_error(&err));
                    self.bridge_call("goto_frame", json!({"frame": start + frames}), timeout)
                        .await?;
                }
            }
        }
        Ok(result)
    }

    /// The action an actor walks with: its first travelling action named like
    /// a walk (not backwards), else any travelling action. Null keeps the
    /// current one (move_actor then says what's wrong).
    async fn walking_action(
        &self,
        template: &str,
        cache: &mut HashMap<String, Value>,
        timeout: Duration,
    ) -> Value {
        if let Some(walk) = cache.get(template) {
            return walk.clone();
        }
        let actions = self
            .bridge_call("list_actions", json!({"template": template}), timeout)
            .await
            .map(|r| r["actions"].as_array().cloned().unwrap_or_default())
            .unwrap_or_default();
        let travels: Vec<&Value> = actions
            .iter()
            .filter(|a| a["travels"] == json!(true))
            .collect();
        let named = |a: &&&Value| a["name"].as_str().unwrap_or_default().to_lowercase();
        let walk = travels
            .iter()
            .find(|a| named(a).contains("walk") && !named(a).contains("back"))
            .or_else(|| travels.first())
            .map(|a| a["name"].clone())
            .unwrap_or(Value::Null);
        cache.insert(template.to_string(), walk.clone());
        walk
    }

    /// Walks an actor to a stage point from `frame`; returns the arrival frame
    async fn walk(
        &self,
        who: &str,
        to: (f64, f64),
        action: &Value,
        frame: i64,
        timeout: Duration,
    ) -> anyhow::Result<i64> {
        let mut args = json!({"actor": who, "path": [{"x": to.0, "z": to.1}], "frame": frame});
        if !action.is_null() {
            args["action"] = action.clone();
        }
        let walked = self.bridge_call("move_actor", args, timeout).await?;
        Ok(walked["end_frame"]
            .as_i64()
            .unwrap_or(frame + 1)
            .max(frame + 1))
    }

    /// Where everyone else on stage stands (stage x, z) at a frame
    async fn others_positions(
        &self,
        who: &str,
        frame: i64,
        timeout: Duration,
    ) -> anyhow::Result<Vec<(f64, f64)>> {
        let scene = self
            .bridge_call("describe_scene", json!({"frame": frame}), timeout)
            .await?;
        Ok(scene["actors"]
            .as_array()
            .cloned()
            .unwrap_or_default()
            .iter()
            .filter(|a| a["name"].as_str() != Some(who) && a["on_stage"] == json!(true))
            .map(|a| {
                let stage = &a["position"]["stage"];
                (
                    stage["x"].as_f64().unwrap_or(0.0),
                    stage["z"].as_f64().unwrap_or(0.0),
                )
            })
            .collect())
    }

    /// An actor's stage position (x, z) at a frame
    async fn stage_position(
        &self,
        who: &str,
        frame: i64,
        timeout: Duration,
    ) -> anyhow::Result<(f64, f64)> {
        let scene = self
            .bridge_call("describe_scene", json!({"frame": frame}), timeout)
            .await?;
        let actor = scene["actors"]
            .as_array()
            .and_then(|a| a.iter().find(|a| a["name"].as_str() == Some(who)))
            .ok_or_else(|| anyhow::anyhow!("{who} isn't in the scene at frame {frame}"))?;
        let stage = &actor["position"]["stage"];
        Ok((
            stage["x"].as_f64().unwrap_or(0.0),
            stage["z"].as_f64().unwrap_or(0.0),
        ))
    }

    /// Builds a whole scene from a script: set, cast, blocking, every line in
    /// order, title, camera cuts and transition, then reviews it.
    async fn script_scene(
        &self,
        args: Value,
        timeout: Duration,
    ) -> anyhow::Result<Vec<ContentBlock>> {
        let mut script = script::parse(&args).map_err(anyhow::Error::msg)?;
        script::assign_voices(&mut script.cast, &speech::installed_voices().await);
        // Each step's failure names the step, so the caller knows what exists
        let step = |what: String| {
            move |err: anyhow::Error| anyhow::anyhow!("{what}: {}", describe_error(&err))
        };

        let state = self.bridge_call("get_state", json!({}), timeout).await?;
        let mut notes: Vec<String> = Vec::new();
        if state["movie"]["scene_count"].as_i64().unwrap_or(0) > 0 {
            self.bridge_call(
                "checkpoint",
                json!({"name": "before script_scene"}),
                timeout,
            )
            .await
            .map_err(step("checkpoint".into()))?;
            notes.push(
                "restore_checkpoint(\"before script_scene\") goes back to the movie as it was"
                    .into(),
            );
        }

        let mut add = json!({"background": script.background});
        if let Some(camera) = script.camera {
            add["camera"] = json!(camera);
        }
        let scene = self
            .bridge_call("add_scene", add, timeout)
            .await
            .map_err(step("add_scene".into()))?;
        let scene_index = scene["scene_index"].clone();
        for member in &script.cast {
            self.bridge_call(
                "add_actor",
                json!({"template": member.actor, "name": member.name}),
                timeout,
            )
            .await
            .map_err(step(format!(
                "casting {} as '{}'",
                member.name, member.actor
            )))?;
        }
        let arranged = self
            .bridge_call(
                "arrange_actors",
                json!({"layout": script.layout, "frame": 1}),
                timeout,
            )
            .await
            .map_err(step("arrange_actors".into()))?;
        if let Some(framing) = arranged["framing"].as_str() {
            notes.push(format!("blocking: {framing}"));
        }

        // Everyone's place in the blocking (stage x, z), and where the
        // screen edges are, for entrances and exits
        let blocked = self
            .bridge_call("describe_scene", json!({"frame": 1}), timeout)
            .await
            .map_err(step("describe_scene".into()))?;
        let mut marks: HashMap<String, (f64, f64)> = HashMap::new();
        for actor in blocked["actors"].as_array().cloned().unwrap_or_default() {
            if let Some(name) = actor["name"].as_str() {
                let stage = &actor["position"]["stage"];
                marks.insert(
                    name.to_string(),
                    (
                        stage["x"].as_f64().unwrap_or(0.0),
                        stage["z"].as_f64().unwrap_or(0.0),
                    ),
                );
            }
        }
        let floor = blocked["camera"]["visible_floor"].clone();
        let edge = |key: &str| {
            (
                floor[key]["x"].as_f64().unwrap_or(0.0),
                floor[key]["z"].as_f64().unwrap_or(0.0),
            )
        };
        let offstage = |side: script::Side, z: f64| match side {
            script::Side::Left => {
                script::offstage_x(edge("bottom_left"), edge("middle_left"), z, 1.0, side)
            }
            script::Side::Right => {
                script::offstage_x(edge("bottom_right"), edge("middle_right"), z, 1.0, side)
            }
        };
        // Characters who enter later leave the blocking and come back on cue
        let mut on_stage: Vec<String> = Vec::new();
        for member in &script.cast {
            if script::starts_off_stage(&member.name, &script.lines) {
                self.bridge_call(
                    "remove_actor",
                    json!({"actor": member.name, "mode": "delete"}),
                    timeout,
                )
                .await
                .map_err(step(format!(
                    "holding {} back for their entrance",
                    member.name
                )))?;
            } else {
                on_stage.push(member.name.clone());
            }
        }
        let template_of = |who: &str| {
            script
                .cast
                .iter()
                .find(|c| c.name == who)
                .map(|c| c.actor.clone())
                .unwrap_or_default()
        };
        let mut walks: HashMap<String, Value> = HashMap::new();

        // The lines, one after another
        let mut frame: i64 = 1;
        let mut played = Vec::new();
        for (i, line) in script.lines.iter().enumerate() {
            match line {
                script::Line::Say { who, text, action } => {
                    let mut a = json!({"actor": who, "text": text, "frame": frame});
                    if let Some(voice) = script
                        .cast
                        .iter()
                        .find(|c| &c.name == who)
                        .and_then(|c| c.voice.clone())
                    {
                        a["voice"] = json!(voice);
                    }
                    if let Some(action) = action {
                        a["action"] = action.clone();
                    }
                    let r = self
                        .speak_line_value(a, timeout)
                        .await
                        .map_err(step(format!("lines[{i}] ({who} says)")))?;
                    let end = r["end_frame"].as_i64().unwrap_or(frame + 6);
                    played.push(
                        json!({"line": i, "who": who, "says": text, "from": frame, "to": end - 1}),
                    );
                    if let Some(note) = r["note"].as_str() {
                        notes.push(format!("lines[{i}]: {note}"));
                    }
                    frame = end;
                }
                script::Line::Do {
                    who,
                    action,
                    seconds,
                } => {
                    let r = self
                        .bridge_call(
                            "animate",
                            json!({"actor": who, "action": action, "seconds": seconds, "frame": frame}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] ({who} does {action})")))?;
                    let end = r["end_frame"].as_i64().unwrap_or(frame + 6);
                    played.push(json!({"line": i, "who": who, "does": r["action"], "from": frame, "to": end - 1}));
                    frame = end;
                }
                script::Line::Wait { seconds } => {
                    frame += (seconds * 6.0).round().max(1.0) as i64;
                }
                script::Line::Enter { who, side } => {
                    let (x, z) = marks.get(who).copied().unwrap_or((0.0, 1.5));
                    let facing = if *side == script::Side::Left {
                        "right"
                    } else {
                        "left"
                    };
                    self.bridge_call(
                        "add_actor",
                        json!({"template": template_of(who), "name": who, "facing": facing, "frame": frame,
                               "position": {"x": offstage(*side, z), "z": z}}),
                        timeout,
                    )
                    .await
                    .map_err(step(format!("lines[{i}] ({who} enters)")))?;
                    let walk = self
                        .walking_action(&template_of(who), &mut walks, timeout)
                        .await;
                    let end = self
                        .walk(who, (x, z), &walk, frame, timeout)
                        .await
                        .map_err(step(format!("lines[{i}] ({who} enters)")))?;
                    on_stage.push(who.clone());
                    if let Some(other) = nearest(&marks, &on_stage, who) {
                        self.bridge_call(
                            "rotate_actor",
                            json!({"actor": who, "face": other, "frame": end}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] ({who} turns to {other})")))?;
                    }
                    played.push(json!({"line": i, "who": who, "enters": format!("{side:?}").to_lowercase(), "from": frame, "to": end - 1}));
                    frame = end;
                }
                script::Line::Exit { who, side } => {
                    let (_, z) = self.stage_position(who, frame, timeout).await?;
                    let walk = self
                        .walking_action(&template_of(who), &mut walks, timeout)
                        .await;
                    let end = self
                        .walk(who, (offstage(*side, z), z), &walk, frame, timeout)
                        .await
                        .map_err(step(format!("lines[{i}] ({who} exits)")))?;
                    self.bridge_call(
                        "remove_actor",
                        json!({"actor": who, "mode": "exit", "frame": end}),
                        timeout,
                    )
                    .await
                    .map_err(step(format!("lines[{i}] ({who} leaves the stage)")))?;
                    on_stage.retain(|n| n != who);
                    played.push(json!({"line": i, "who": who, "exits": format!("{side:?}").to_lowercase(), "from": frame, "to": end - 1}));
                    frame = end;
                }
                script::Line::WalkTo { who, target } => {
                    let (dest, face) = match target {
                        script::Target::Point { x, z } => ((*x, *z), None),
                        script::Target::Actor(other) => {
                            // Stop a little short of them, on the side we come from
                            let me = self.stage_position(who, frame, timeout).await?;
                            let them = self.stage_position(other, frame, timeout).await?;
                            let (dx, dz) = (me.0 - them.0, me.1 - them.1);
                            let len = (dx * dx + dz * dz).sqrt().max(1e-6);
                            (
                                (them.0 + dx / len * 0.8, them.1 + dz / len * 0.8),
                                Some(other.clone()),
                            )
                        }
                    };
                    // Stop where nobody (or nothing) already stands
                    let others = self.others_positions(who, frame, timeout).await?;
                    let dest = script::clear_spot(dest, &others);
                    let walk = self
                        .walking_action(&template_of(who), &mut walks, timeout)
                        .await;
                    let end = self
                        .walk(who, dest, &walk, frame, timeout)
                        .await
                        .map_err(step(format!("lines[{i}] ({who} walks)")))?;
                    if let Some(other) = &face {
                        self.bridge_call(
                            "rotate_actor",
                            json!({"actor": who, "face": other, "frame": end}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] ({who} turns to {other})")))?;
                    }
                    marks.insert(who.clone(), dest);
                    let target_json = match &face {
                        Some(other) => json!(other),
                        None => json!({"x": dest.0, "z": dest.1}),
                    };
                    played.push(json!({"line": i, "who": who, "walks_to": target_json, "from": frame, "to": end - 1}));
                    frame = end;
                }
                script::Line::Throw {
                    who,
                    prop,
                    at,
                    reaction,
                } => {
                    let me = self.stage_position(who, frame, timeout).await?;
                    let them = self.stage_position(at, frame, timeout).await?;
                    let (dx, dz) = (them.0 - me.0, them.1 - me.1);
                    let len = (dx * dx + dz * dz).sqrt().max(1e-6);
                    // From the thrower's hand, a little toward the target
                    let name = format!("{prop} {}", i + 1);
                    self.bridge_call(
                        "add_actor",
                        json!({"template": prop, "name": name, "frame": frame,
                               "position": {"x": me.0 + dx / len * 0.45, "z": me.1 + dz / len * 0.45, "y": 1.0}}),
                        timeout,
                    )
                    .await
                    .map_err(step(format!("lines[{i}] ({who} throws a {prop})")))?;
                    let thrown = self
                        .bridge_call(
                            "physics",
                            json!({"actor": name, "to": at, "height": 0.6, "spin": "tumble", "frame": frame}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] ({who} throws at {at})")))?;
                    let impacts = thrown["impacts"].as_array().cloned().unwrap_or_default();
                    let hit = impacts
                        .iter()
                        .find(|imp| imp["hit"].as_str() == Some(at.as_str()));
                    let mut next = impacts
                        .first()
                        .and_then(|imp| imp["frame"].as_i64())
                        .unwrap_or(thrown["end_frame"].as_i64().unwrap_or(frame + 6))
                        + 3;
                    if let (Some(hit), Some(reaction)) = (hit, reaction) {
                        let hit_frame = hit["frame"].as_i64().unwrap_or(frame);
                        let reacted = self
                            .bridge_call(
                                "animate",
                                json!({"actor": at, "action": reaction, "seconds": 1.5, "frame": hit_frame}),
                                timeout,
                            )
                            .await
                            .map_err(step(format!("lines[{i}] ({at} reacts)")))?;
                        next = next.max(reacted["end_frame"].as_i64().unwrap_or(hit_frame + 9));
                    }
                    if hit.is_none() {
                        notes.push(format!("lines[{i}]: the {prop} missed {at}"));
                    }
                    played.push(json!({"line": i, "who": who, "throws": prop, "at": at, "hit": hit.is_some(), "from": frame, "to": next - 1}));
                    frame = next;
                }
            }
        }

        // A closing pause needs something on stage to last that long
        let now = self.bridge_call("get_state", json!({}), timeout).await?;
        let last = now["movie"]["scene"]["last_frame"]
            .as_i64()
            .unwrap_or(frame);
        if last < frame - 1 {
            // Someone still on stage stands at rest through it
            match on_stage.first() {
                Some(holder) => {
                    self.bridge_call(
                        "animate",
                        json!({"actor": holder, "action": 0, "frames": frame - 1 - last, "frame": last}),
                        timeout,
                    )
                    .await
                    .map_err(step("holding the final pause".into()))?;
                }
                None => {
                    notes.push("everyone has left, so the scene ends with the last exit".into())
                }
            }
        }

        if let Some(title) = &script.title {
            let until = 1 + (script::title_seconds(title) * 6.0).ceil() as i64;
            self.bridge_call(
                "add_text",
                json!({"text": title, "kind": "caption", "rect": "top", "frame": 1, "until_frame": until}),
                timeout,
            )
            .await
            .map_err(step("title".into()))?;
        }
        let mut shots = Value::Null;
        if script.camera_style != "none" {
            let cut = self
                .bridge_call(
                    "auto_camera",
                    json!({"scene": scene_index, "style": script.camera_style}),
                    timeout,
                )
                .await
                .map_err(step("auto_camera".into()))?;
            shots = cut["shots"].clone();
        }
        if let Some(transition) = &script.transition {
            self.bridge_call("set_transition", json!({"transition": transition}), timeout)
                .await
                .map_err(step("set_transition".into()))?;
        }
        let review = self
            .bridge_call("review_movie", json!({"scene": scene_index}), timeout)
            .await
            .map_err(step("review_movie".into()))?;

        let cast: Vec<Value> = script
            .cast
            .iter()
            .map(|c| json!({"name": c.name, "actor": c.actor, "voice": c.voice}))
            .collect();
        let result = json!({
            "scene_index": scene_index,
            "frames": frame - 1,
            "seconds": ((frame - 1) as f64 / 6.0 * 10.0).round() / 10.0,
            "cast": cast,
            "lines": played,
            "shots": shots,
            "review": {"verdict": review["verdict"], "issues": review["issues"]},
            "notes": notes,
        });
        Ok(vec![ContentBlock::text(result.to_string())])
    }

    async fn export_video(
        &self,
        args: Value,
        timeout: Duration,
    ) -> anyhow::Result<Vec<ContentBlock>> {
        let path = args.get("path").and_then(Value::as_str).unwrap_or_default();
        if path.is_empty() {
            anyhow::bail!("pass 'path' (an absolute .mp4 path)");
        }
        let mut out = std::path::PathBuf::from(path);
        if out.extension().is_none() {
            out.set_extension("mp4");
        }
        if !out.is_absolute() {
            anyhow::bail!("'path' must be absolute");
        }
        video::find_ffmpeg()?;
        let options = video::VideoOptions {
            scale: args
                .get("scale")
                .and_then(Value::as_u64)
                .unwrap_or(2)
                .clamp(1, 3) as u32,
            transition_seconds: args
                .get("transition_seconds")
                .and_then(Value::as_f64)
                .unwrap_or(1.0)
                .clamp(0.0, 5.0),
            audio: args.get("audio").and_then(Value::as_bool).unwrap_or(true),
            smooth: args.get("smooth").and_then(Value::as_bool).unwrap_or(false),
        };

        // Frames and sounds go to a scratch folder that is removed afterwards
        static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(1);
        let work = std::env::temp_dir().join(format!(
            "3dmm-export-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&work)?;
        let mut media_args = json!({"dir": work, "audio": options.audio});
        if let Some(scene) = args.get("scene") {
            media_args["scene"] = scene.clone();
        }
        let result = async {
            let manifest = self
                .bridge_call("export_media", media_args, timeout)
                .await?;
            let mut summary = video::render(&manifest, &out, &options).await?;
            if let Some(notes) = manifest.get("notes") {
                summary["notes"] = notes.clone();
            }
            anyhow::Ok(summary)
        }
        .await;
        let _ = std::fs::remove_dir_all(&work);
        Ok(vec![ContentBlock::text(result?.to_string())])
    }

    /// Runs one tool (anything but batch).
    async fn run_tool(&self, spec: &ToolSpec, args: Value) -> anyhow::Result<Vec<ContentBlock>> {
        crate::tools::check_args(spec, &args).map_err(anyhow::Error::msg)?;
        let timeout = Duration::from_secs(spec.timeout_secs);
        match spec.kind {
            Kind::Launch => {
                let state = self.launch().await?;
                Ok(vec![ContentBlock::text(state.to_string())])
            }
            Kind::Bridge(cmd) => {
                let result = self.bridge_call(cmd, args, timeout).await?;
                Ok(vec![ContentBlock::text(result.to_string())])
            }
            Kind::SpeakLine => self.speak_line(args, timeout).await,
            Kind::ScriptScene => self.script_scene(args, timeout).await,
            Kind::BridgeImage(cmd) => {
                let result = self.bridge_call(cmd, args, timeout).await?;
                let path = result
                    .get("path")
                    .and_then(Value::as_str)
                    .ok_or_else(|| anyhow::anyhow!("{cmd} returned no image path"))?;
                let png = tokio::fs::read(path).await?;
                let data = base64::engine::general_purpose::STANDARD.encode(png);
                Ok(vec![
                    ContentBlock::image(data, "image/png"),
                    ContentBlock::text(result.to_string()),
                ])
            }
            Kind::ExportVideo => self.export_video(args, timeout).await,
            Kind::Batch => anyhow::bail!("batch steps cannot themselves be batches"),
        }
    }

    /// Runs batch steps in order. Returns the content and whether any step
    /// failed.
    async fn run_batch(&self, args: Value) -> (Vec<ContentBlock>, bool) {
        let steps = args
            .get("steps")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let stop_on_error = args
            .get("stop_on_error")
            .and_then(Value::as_bool)
            .unwrap_or(true);
        if steps.is_empty() {
            return (
                vec![ContentBlock::text("'steps' must be a non-empty array")],
                true,
            );
        }
        let mut content = Vec::new();
        let mut failed = false;
        for (index, step) in steps.iter().enumerate() {
            let name = step.get("tool").and_then(Value::as_str).unwrap_or_default();
            let step_args = step.get("args").cloned().unwrap_or_else(|| json!({}));
            let outcome = match self.by_name.get(name) {
                Some(&i) => self.run_tool(&self.tools[i], step_args).await,
                None => Err(anyhow::anyhow!("unknown tool '{name}'")),
            };
            match outcome {
                Ok(blocks) => {
                    for block in blocks {
                        // Prefix text results with the step so they can be matched up
                        let text = block.as_text().map(|t| t.text.clone());
                        match text {
                            Some(text) => content
                                .push(ContentBlock::text(format!("[{index}] {name}: {text}"))),
                            None => content.push(block),
                        }
                    }
                }
                Err(err) => {
                    failed = true;
                    content.push(ContentBlock::text(format!(
                        "[{index}] {name} FAILED: {}",
                        describe_error(&err)
                    )));
                    if stop_on_error {
                        content.push(ContentBlock::text(format!(
                            "stopped after step {index}; steps {}..{} were not run",
                            index + 1,
                            steps.len() - 1
                        )));
                        break;
                    }
                }
            }
        }
        (content, failed)
    }
}

/// The on-stage character standing nearest to `who`, by their marks
fn nearest(marks: &HashMap<String, (f64, f64)>, on_stage: &[String], who: &str) -> Option<String> {
    let (x, z) = *marks.get(who)?;
    on_stage
        .iter()
        .filter(|n| n.as_str() != who)
        .filter_map(|n| marks.get(n).map(|(ox, oz)| (n, (ox - x).hypot(oz - z))))
        .min_by(|a, b| a.1.total_cmp(&b.1))
        .map(|(n, _)| n.clone())
}

/// Error text for the model: bridge errors with their code, others with context.
fn describe_error(err: &anyhow::Error) -> String {
    match err.downcast_ref::<BridgeError>() {
        Some(bridge) => format!("{} [{}]", bridge.message, bridge.code),
        None => {
            let text = format!("{err:#}");
            if text.contains("did not answer") {
                format!(
                    "{text}. 3DMMEx may be showing a dialog (debug builds show assertion dialogs); check its window"
                )
            } else {
                text
            }
        }
    }
}

fn tool_definition(spec: &ToolSpec) -> Tool {
    let schema = match &spec.schema {
        Value::Object(map) => map.clone(),
        _ => serde_json::Map::new(),
    };
    Tool::new(spec.name, spec.description.clone(), Arc::new(schema))
}

impl ServerHandler for ThreeDmmServer {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(
            ServerCapabilities::builder()
                .enable_tools()
                .enable_resources()
                .enable_prompts()
                .build(),
        )
        .with_server_info(Implementation::new("3dmm-mcp", env!("CARGO_PKG_VERSION")))
        .with_instructions(INSTRUCTIONS)
    }

    async fn list_tools(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListToolsResult, McpError> {
        Ok(ListToolsResult::with_all_items(
            self.tools.iter().map(tool_definition).collect(),
        ))
    }

    fn get_tool(&self, name: &str) -> Option<Tool> {
        self.by_name
            .get(name)
            .map(|&i| tool_definition(&self.tools[i]))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let Some(&index) = self.by_name.get(request.name.as_ref()) else {
            return Err(McpError::invalid_params(
                format!("unknown tool '{}'", request.name),
                None,
            ));
        };
        let spec = &self.tools[index];
        let args = Value::Object(request.arguments.unwrap_or_default());
        let result = if spec.kind == Kind::Batch {
            let (content, failed) = self.run_batch(args).await;
            if failed {
                CallToolResult::error(content)
            } else {
                CallToolResult::success(content)
            }
        } else {
            match self.run_tool(spec, args).await {
                Ok(content) => CallToolResult::success(content),
                // Report failures as tool errors so the model can read and react
                Err(err) => CallToolResult::error(vec![ContentBlock::text(describe_error(&err))]),
            }
        };
        Ok(result.into())
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, McpError> {
        Ok(ListResourcesResult::with_all_items(vec![Resource::new(
            GUIDE_URI,
            "3D Movie Maker workflow guide",
        )
        .with_description("How to go from an idea to a saved .3mm movie with these tools.")
        .with_mime_type("text/markdown")]))
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResponse, McpError> {
        if request.uri != GUIDE_URI {
            return Err(McpError::resource_not_found(
                format!("no resource {}", request.uri),
                None,
            ));
        }
        Ok(ReadResourceResult::new(vec![ResourceContents::text(WORKFLOW_GUIDE, GUIDE_URI)]).into())
    }

    async fn list_prompts(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListPromptsResult, McpError> {
        Ok(ListPromptsResult::with_all_items(vec![Prompt::new(
            "make_movie",
            Some("Make a short 3D Movie Maker movie from an idea, step by step."),
            Some(vec![PromptArgument::new("idea")
                .with_description("What the movie is about, e.g. \"two guys arguing in a diner\"")
                .with_required(true)]),
        )]))
    }

    async fn get_prompt(
        &self,
        request: GetPromptRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<GetPromptResponse, McpError> {
        if request.name != "make_movie" {
            return Err(McpError::invalid_params(
                format!("no prompt {}", request.name),
                None,
            ));
        }
        let idea = request
            .arguments
            .as_ref()
            .and_then(|args| args.get("idea"))
            .and_then(Value::as_str)
            .unwrap_or("a short silly movie");
        let text = format!(
            "Make a short movie in 3D Movie Maker: {idea}.\n\nFollow this workflow, rendering frames to check your \
             work as you go, and save the result as a .3mm file.\n\n{WORKFLOW_GUIDE}"
        );
        Ok(
            GetPromptResult::new(vec![PromptMessage::new_text(Role::User, text)])
                .with_description("Make a 3D Movie Maker movie")
                .into(),
        )
    }
}
