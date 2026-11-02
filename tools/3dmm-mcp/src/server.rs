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

/// How far in front of a thrower (metres) a thrown prop appears: clear of
/// their body, so it doesn't start out inside them, but no more than a
/// third of the way to the target
const THROW_RELEASE: f64 = 0.6;

/// Metres between neighbours when the script has a throw in it
const THROW_SPACING: f64 = 2.0;

const INSTRUCTIONS: &str = "Tools for making movies in Microsoft 3D Movie Maker (1995) through its automation \
bridge. Fastest: script_movie(scenes: [...], export: \"/abs/path.mp4\") builds and exports a whole movie from scene \
scripts (set, cast, lines: says / does / wait / enters / exits / walks_to / throws / sound; subtitles, music) and \
returns a review plus a storyboard picture; script_scene does one scene. Then refine with the individual tools \
(add_actor, move_actor, speak_line, animate, physics, arrange_actors, auto_camera, add_text...), check with \
review_movie and storyboard, and save_movie / export_video. Read the resource 3dmm://guide/workflow for details. \
The app starts automatically on the first call.";

pub struct ThreeDmmServer {
    port: u16,
    autolaunch: bool,
    tools: Arc<Vec<ToolSpec>>,
    by_name: HashMap<&'static str, usize>,
    client: Arc<Mutex<Option<BridgeClient>>>,
    recovery: Arc<std::sync::Mutex<Recovery>>,
}

/// What the client needs to hear if the app had to be started again: the
/// movie in it is gone, but the last save or checkpoint file can be reopened
#[derive(Default)]
struct Recovery {
    connection_lost: bool,
    restarted: bool,
    last_saved: Option<String>,
    last_checkpoint: Option<String>,
}

impl Recovery {
    /// Notes what a successful command says about where the movie lives
    fn record(&mut self, cmd: &str, result: &Value) {
        let path = |key: &str| result[key].as_str().map(String::from);
        match cmd {
            "save_movie" | "open_movie" => self.last_saved = path("path"),
            "new_movie" => self.last_saved = None,
            "checkpoint" => self.last_checkpoint = path("file").or(self.last_checkpoint.take()),
            _ => {}
        }
    }

    /// The notice to give once after a restart, if there was one
    fn take_notice(&mut self) -> Option<String> {
        if !std::mem::take(&mut self.restarted) {
            return None;
        }
        let mut notice = String::from(
            "NOTE: 3D Movie Maker had stopped and was started again, so the movie open in it now is a new, empty \
             one and changes since the last save are gone.",
        );
        if let Some(saved) = &self.last_saved {
            notice.push_str(&format!(" Last saved: {saved}."));
        }
        if let Some(checkpoint) = &self.last_checkpoint {
            notice.push_str(&format!(" Latest checkpoint: {checkpoint}."));
        }
        if self.last_saved.is_some() || self.last_checkpoint.is_some() {
            notice.push_str(" Reopen one with open_movie(path) to carry on.");
        }
        Some(notice)
    }
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
            recovery: Arc::new(std::sync::Mutex::new(Recovery::default())),
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
                    // A fresh app after losing the old one: the movie is gone
                    let mut recovery = self.recovery.lock().expect("recovery lock");
                    if std::mem::take(&mut recovery.connection_lost) {
                        recovery.restarted = true;
                    }
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
        match &result {
            Ok(value) => self
                .recovery
                .lock()
                .expect("recovery lock")
                .record(cmd, value),
            // Bridge errors are normal answers; anything else means the
            // connection is unusable, so reconnect next time
            Err(err) if err.downcast_ref::<BridgeError>().is_none() => {
                *guard = None;
                self.recovery.lock().expect("recovery lock").connection_lost = true;
            }
            Err(_) => {}
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

    /// A built-in sound by a plain name: an exact match, else the first that
    /// contains it (noted, so the caller knows which was used)
    async fn find_sound(
        &self,
        kind: &str,
        query: &str,
        notes: &mut Vec<String>,
        timeout: Duration,
    ) -> anyhow::Result<Value> {
        let found = self
            .bridge_call(
                "list_sounds",
                json!({"type": kind, "filter": query}),
                timeout,
            )
            .await?;
        let items = found["items"].as_array().cloned().unwrap_or_default();
        let exact = items.iter().find(|i| {
            i["name"]
                .as_str()
                .is_some_and(|n| n.eq_ignore_ascii_case(query))
        });
        match exact.or(items.first()) {
            Some(item) => {
                if exact.is_none() {
                    notes.push(format!("used {kind} '{}' for '{query}'", item["name"].as_str().unwrap_or_default()));
                }
                Ok(item["id"].clone())
            }
            None => anyhow::bail!("no {kind} sound is called anything like '{query}' (list_sounds type {kind} shows them)"),
        }
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
        // Rebuilding a scene in place: check the index before building
        let replace = args.get("replace").and_then(Value::as_i64);
        if let Some(index) = replace {
            let state = self.bridge_call("get_state", json!({}), timeout).await?;
            let count = state["movie"]["scene_count"].as_i64().unwrap_or(0);
            if !(0..count).contains(&index) {
                anyhow::bail!(
                    "'replace' must be an existing scene's index; the movie has {count} scene(s) (0-{})",
                    count - 1
                );
            }
        }
        let mut result = self.build_scene(&script, true, timeout).await?;
        if let Some(index) = replace {
            // The new scene was added at the end: drop the old one and move
            // the new one into its place
            self.bridge_call("delete_scene", json!({"index": index}), timeout)
                .await?;
            let built = result["scene_index"].as_i64().unwrap_or(index + 1) - 1;
            if built != index {
                self.bridge_call("move_scene", json!({"from": built, "to": index}), timeout)
                    .await?;
            }
            result["scene_index"] = json!(index);
            result["replaced"] = json!(true);
            let review = self
                .bridge_call("review_movie", json!({"scene": index}), timeout)
                .await?;
            result["review"] = json!({"verdict": review["verdict"], "issues": review["issues"]});
        }
        let mut content = Vec::new();
        if args.get("preview").and_then(Value::as_bool).unwrap_or(true) {
            content.extend(
                self.storyboard_image(json!({"scene": result["scene_index"], "count": 8}), timeout)
                    .await,
            );
        }
        content.push(ContentBlock::text(result.to_string()));
        Ok(content)
    }

    /// A storyboard as image content, so a script's result shows what it
    /// made; None if it can't be drawn (the build itself still stands)
    async fn storyboard_image(&self, args: Value, timeout: Duration) -> Option<ContentBlock> {
        let board = self.bridge_call("storyboard", args, timeout).await.ok()?;
        let png = tokio::fs::read(board["path"].as_str()?).await.ok()?;
        Some(ContentBlock::image(
            base64::engine::general_purpose::STANDARD.encode(png),
            "image/png",
        ))
    }

    /// Builds a whole movie from scripted scenes: every scene is checked
    /// before any is built, characters keep one actor and voice throughout,
    /// and the movie can be exported at the end.
    async fn script_movie(
        &self,
        args: Value,
        timeout: Duration,
    ) -> anyhow::Result<Vec<ContentBlock>> {
        let mut scripts = script::parse_movie(&args).map_err(anyhow::Error::msg)?;
        script::assign_movie_voices(&mut scripts, &speech::installed_voices().await);
        let state = self.bridge_call("get_state", json!({}), timeout).await?;
        let mut notes: Vec<String> = Vec::new();
        if state["movie"]["scene_count"].as_i64().unwrap_or(0) > 0 {
            self.bridge_call(
                "checkpoint",
                json!({"name": "before script_movie"}),
                timeout,
            )
            .await?;
            notes.push(
                "restore_checkpoint(\"before script_movie\") goes back to the movie as it was"
                    .into(),
            );
        }
        let mut scenes = Vec::new();
        let mut warnings = 0;
        let mut seconds = 0.0;
        for (i, script) in scripts.iter().enumerate() {
            let built = self
                .build_scene(script, false, timeout)
                .await
                .map_err(|err| {
                    anyhow::anyhow!(
                        "scenes[{i}]: {} (scenes before it were built)",
                        describe_error(&err)
                    )
                })?;
            warnings += built["review"]["issues"]
                .as_array()
                .map(|issues| issues.iter().filter(|i| i["severity"] == "warning").count())
                .unwrap_or(0);
            seconds += built["seconds"].as_f64().unwrap_or(0.0);
            scenes.push(built);
        }
        let mut result = json!({
            "scenes": scenes,
            "seconds": (seconds * 10.0).round() / 10.0,
            "warnings": warnings,
            "notes": notes,
        });
        if let Some(target) = args.get("target_seconds").and_then(Value::as_f64) {
            if (seconds - target).abs() > target * 0.2 {
                result["length_note"] = json!(format!(
                    "the movie runs {seconds:.1} s against a target of {target} s; add or cut lines (waits, actions) to fit"
                ));
            }
        }
        if let Some(path) = args.get("export").and_then(Value::as_str) {
            let exported = self
                .export_video(json!({"path": path}), timeout)
                .await
                .map_err(|err| {
                    anyhow::anyhow!(
                        "export: {} (the movie itself was built)",
                        describe_error(&err)
                    )
                })?;
            if let Some(text) = exported
                .first()
                .and_then(|c| c.as_text())
                .map(|t| t.text.clone())
            {
                result["export"] = serde_json::from_str(&text).unwrap_or(Value::String(text));
            }
        }
        let mut content = Vec::new();
        if args.get("preview").and_then(Value::as_bool).unwrap_or(true) {
            content.extend(
                self.storyboard_image(json!({"all_scenes": true, "count": 12}), timeout)
                    .await,
            );
        }
        content.push(ContentBlock::text(result.to_string()));
        Ok(content)
    }

    /// Builds one scene; with `checkpoint`, saves one first if the movie
    /// already has scenes. Returns the scene's report.
    async fn build_scene(
        &self,
        script: &script::Script,
        checkpoint: bool,
        timeout: Duration,
    ) -> anyhow::Result<Value> {
        // Each step's failure names the step, so the caller knows what exists
        let step = |what: String| {
            move |err: anyhow::Error| anyhow::anyhow!("{what}: {}", describe_error(&err))
        };

        let state = self.bridge_call("get_state", json!({}), timeout).await?;
        let mut notes: Vec<String> = Vec::new();
        if checkpoint && state["movie"]["scene_count"].as_i64().unwrap_or(0) > 0 {
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
        if let Some(music) = &script.music {
            // "none" plays the stopper, silencing the set's own music
            let none = music.eq_ignore_ascii_case("none");
            let query = if none {
                "sound stopper"
            } else {
                music.as_str()
            };
            let id = self
                .find_sound("music", query, &mut notes, timeout)
                .await
                .map_err(step("music".into()))?;
            self.bridge_call(
                "attach_sound",
                json!({"sound": id, "target": "scene", "frame": 1, "loop": !none}),
                timeout,
            )
            .await
            .map_err(step("music".into()))?;
        }
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
        // A throw needs room between people
        let mut arrange = json!({"layout": script.layout, "frame": 1});
        if script
            .lines
            .iter()
            .any(|l| matches!(l, script::Line::Throw { .. }))
        {
            arrange["spacing"] = json!(THROW_SPACING);
        }
        let arranged = self
            .bridge_call("arrange_actors", arrange, timeout)
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
                    let mut end = r["end_frame"].as_i64().unwrap_or(frame + 6);
                    if script.subtitles {
                        // Quick lines are said faster than they are read: the
                        // next line waits until the caption has been up long enough
                        end = end.max(frame + (script::title_seconds(text) * 6.0).ceil() as i64);
                        self.bridge_call(
                            "add_text",
                            json!({"text": text, "kind": "caption", "rect": "bottom", "frame": frame, "until_frame": end, "color": "auto"}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] (subtitle)")))?;
                    }
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
                script::Line::Sound { sound, on } => {
                    let id = self
                        .find_sound("sfx", sound, &mut notes, timeout)
                        .await
                        .map_err(step(format!("lines[{i}] (sound)")))?;
                    let target = on.clone().map(Value::from).unwrap_or(json!("scene"));
                    self.bridge_call(
                        "attach_sound",
                        json!({"sound": id, "target": target, "frame": frame}),
                        timeout,
                    )
                    .await
                    .map_err(step(format!("lines[{i}] (sound {sound})")))?;
                    played.push(json!({"line": i, "sound": sound, "on": target, "at": frame}));
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
                    hit_sound,
                } => {
                    let me = self.stage_position(who, frame, timeout).await?;
                    let them = self.stage_position(at, frame, timeout).await?;
                    let (dx, dz) = (them.0 - me.0, them.1 - me.1);
                    let len = (dx * dx + dz * dz).sqrt().max(1e-6);
                    let release = THROW_RELEASE.min(len / 3.0);
                    // From the thrower's hand, a little toward the target
                    let name = format!("{prop} {}", i + 1);
                    self.bridge_call(
                        "add_actor",
                        json!({"template": prop, "name": name, "frame": frame,
                               "position": {"x": me.0 + dx / len * release, "z": me.1 + dz / len * release, "y": 1.0}}),
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
                    if let (Some(hit), Some(sound)) = (hit, hit_sound) {
                        let id = self
                            .find_sound("sfx", sound, &mut notes, timeout)
                            .await
                            .map_err(step(format!("lines[{i}] (hit sound)")))?;
                        self.bridge_call(
                            "attach_sound",
                            json!({"sound": id, "target": at, "frame": hit["frame"]}),
                            timeout,
                        )
                        .await
                        .map_err(step(format!("lines[{i}] (hit sound)")))?;
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
                json!({"text": title, "kind": "caption", "rect": "top", "frame": 1, "until_frame": until, "color": "auto"}),
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
        Ok(result)
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
            Kind::ScriptMovie => self.script_movie(args, timeout).await,
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
            let lost = [
                "closed the connection",
                "Broken pipe",
                "Connection reset",
                "connection refused",
            ];
            if lost.iter().any(|l| text.contains(l)) {
                format!(
                    "{text}. 3D Movie Maker seems to have stopped; the next call starts it again with an empty \
                     movie (reopen your last save or checkpoint with open_movie)"
                )
            } else if text.contains("did not answer") {
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
        let mut result = result;
        if let Some(notice) = self.recovery.lock().expect("recovery lock").take_notice() {
            result.content.insert(0, ContentBlock::text(notice));
        }
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
            "Make a short movie in 3D Movie Maker: {idea}.\n\nStart with script_movie (or script_scene per scene): write \
             the screenplay as scenes with a set, a cast and lines, and let it build, cut and review the movie. Look \
             at the storyboard it returns and fix any review warnings, then save the result as a .3mm file and \
             export an MP4.\n\n{WORKFLOW_GUIDE}"
        );
        Ok(
            GetPromptResult::new(vec![PromptMessage::new_text(Role::User, text)])
                .with_description("Make a 3D Movie Maker movie")
                .into(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn restart_notice_names_what_can_be_reopened() {
        let mut recovery = Recovery::default();
        recovery.record("save_movie", &json!({"path": "/m/a.3mm"}));
        recovery.record("checkpoint", &json!({"name": "x", "file": "/tmp/cp/x.3mm"}));
        assert_eq!(recovery.take_notice(), None, "no restart, no notice");
        recovery.restarted = true;
        let notice = recovery.take_notice().unwrap();
        assert!(
            notice.contains("/m/a.3mm") && notice.contains("/tmp/cp/x.3mm"),
            "{notice}"
        );
        assert_eq!(recovery.take_notice(), None, "said once");

        recovery.record("new_movie", &json!({}));
        recovery.restarted = true;
        let notice = recovery.take_notice().unwrap();
        assert!(
            !notice.contains("/m/a.3mm"),
            "a new movie forgets the old save: {notice}"
        );
    }
}
