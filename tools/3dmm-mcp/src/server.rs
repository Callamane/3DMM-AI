//! MCP server: exposes the tool table and forwards calls to the bridge.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use base64::Engine;
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, ContentBlock, GetPromptRequestParams,
    GetPromptResponse, GetPromptResult, Implementation, ListPromptsResult, ListResourcesResult,
    ListToolsResult, PaginatedRequestParams, Prompt, PromptArgument, PromptMessage,
    ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, Resource, ResourceContents,
    Role, ServerCapabilities, ServerConfig, Tool,
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
bridge. Typical order: list_backgrounds -> add_scene -> list_templates -> add_actor -> move_actor/animate -> \
attach_sound/add_text -> render_frame to check -> save_movie. Read the resource 3dmm://guide/workflow for details. \
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

    async fn bridge_call(&self, cmd: &str, args: Value, timeout: Duration) -> anyhow::Result<Value> {
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
        let state = self.bridge_call("get_state", json!({}), Duration::from_secs(30)).await?;
        if state.get("screen").and_then(Value::as_str) != Some("studio") {
            self.bridge_call("goto_studio", json!({}), Duration::from_secs(120)).await?;
            return self.bridge_call("get_state", json!({}), Duration::from_secs(30)).await;
        }
        Ok(state)
    }

    async fn speak_line(&self, args: Value, timeout: Duration) -> anyhow::Result<Vec<ContentBlock>> {
        let text = args.get("text").and_then(Value::as_str).unwrap_or_default().to_string();
        let voice = args.get("voice").and_then(Value::as_str);
        let actor = args.get("actor").cloned().unwrap_or(Value::Null);
        let action = args.get("action").cloned().unwrap_or_else(|| json!("Talk"));

        let wav = speech::synthesize(&text, voice).await?;
        let seconds = speech::wav_seconds(&wav)?;
        let label: String = text.chars().take(40).collect();
        let imported = self
            .bridge_call("import_sound", json!({"path": wav, "type": "speech", "name": label}), timeout)
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
        if action.as_str().map(str::to_lowercase).as_deref() != Some("none") {
            let animated = self
                .bridge_call(
                    "animate",
                    json!({"actor": actor, "action": action, "frames": frames, "frame": start}),
                    timeout,
                )
                .await;
            match animated {
                Ok(animated) => result["action"] = animated["action"].clone(),
                Err(err) => {
                    // The line is in; say why the acting part didn't happen
                    result["action_error"] = json!(format!("{err:#}"));
                }
            }
        }
        Ok(vec![ContentBlock::text(result.to_string())])
    }

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
            Kind::RenderImage => {
                let result = self.bridge_call("render_frame", args, timeout).await?;
                let path = result
                    .get("path")
                    .and_then(Value::as_str)
                    .ok_or_else(|| anyhow::anyhow!("render_frame returned no path"))?;
                let png = tokio::fs::read(path).await?;
                let data = base64::engine::general_purpose::STANDARD.encode(png);
                Ok(vec![
                    ContentBlock::image(data, "image/png"),
                    ContentBlock::text(result.to_string()),
                ])
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
        self.by_name.get(name).map(|&i| tool_definition(&self.tools[i]))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let Some(&index) = self.by_name.get(request.name.as_ref()) else {
            return Err(McpError::invalid_params(format!("unknown tool '{}'", request.name), None));
        };
        let spec = &self.tools[index];
        let args = Value::Object(request.arguments.unwrap_or_default());
        let result = match self.run_tool(spec, args).await {
            Ok(content) => CallToolResult::success(content),
            Err(err) => {
                // Report failures as tool errors so the model can read and react
                let message = match err.downcast_ref::<BridgeError>() {
                    Some(bridge) => format!("{} [{}]", bridge.message, bridge.code),
                    None => format!("{err:#}"),
                };
                CallToolResult::error(vec![ContentBlock::text(message)])
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
            return Err(McpError::resource_not_found(format!("no resource {}", request.uri), None));
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
            return Err(McpError::invalid_params(format!("no prompt {}", request.name), None));
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
        Ok(GetPromptResult::new(vec![PromptMessage::new_text(Role::User, text)])
            .with_description("Make a 3D Movie Maker movie")
            .into())
    }
}
