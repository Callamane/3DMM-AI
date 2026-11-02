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
use crate::speech;
use crate::tools::{all_tools, Kind, ToolSpec, WORKFLOW_GUIDE};

const GUIDE_URI: &str = "3dmm://guide/workflow";

const INSTRUCTIONS: &str = "Tools for making movies in Microsoft 3D Movie Maker (1995) through its automation \
bridge. Typical order: list_backgrounds/preview_cameras -> add_scene -> list_templates/list_actions -> add_actor -> \
render_frame(overlay) to block -> move_actor/speak_line/animate per actor in time order -> add_text -> \
describe_timeline and render_frame to check -> save_movie. Read the resource 3dmm://guide/workflow for details. \
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
        Ok(vec![ContentBlock::text(result.to_string())])
    }

    /// Runs one tool (anything but batch).
    async fn run_tool(&self, spec: &ToolSpec, args: Value) -> anyhow::Result<Vec<ContentBlock>> {
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
