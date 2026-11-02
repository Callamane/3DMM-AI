//! Drives the real MCP server over stdio, as an AI client would: a scene
//! built from a script with script_scene, then checked.
//!
//! Needs a built 3DMMEx with the automation bridge and text-to-speech, so
//! it is ignored by default:
//!   cargo test --test mcp_script -- --ignored

use std::process::Stdio;
use std::time::Duration;

use anyhow::{bail, ensure, Context, Result};
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader, Lines};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};

const PORT: u16 = 7398;

struct Client {
    _child: Child,
    stdin: ChildStdin,
    lines: Lines<BufReader<ChildStdout>>,
    next_id: u64,
}

impl Client {
    async fn start() -> Result<Self> {
        let mut child = Command::new(env!("CARGO_BIN_EXE_3dmm-mcp"))
            .env("THREEDMM_PORT", PORT.to_string())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .spawn()
            .context("starting the MCP server")?;
        let stdin = child.stdin.take().context("stdin")?;
        let lines = BufReader::new(child.stdout.take().context("stdout")?).lines();
        let mut client = Client {
            _child: child,
            stdin,
            lines,
            next_id: 1,
        };
        client
            .rpc(
                "initialize",
                json!({"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}}),
            )
            .await?;
        client
            .send(json!({"jsonrpc": "2.0", "method": "notifications/initialized"}))
            .await?;
        Ok(client)
    }

    async fn send(&mut self, message: Value) -> Result<()> {
        let mut line = serde_json::to_string(&message)?;
        line.push('\n');
        self.stdin.write_all(line.as_bytes()).await?;
        Ok(())
    }

    async fn rpc(&mut self, method: &str, params: Value) -> Result<Value> {
        let id = self.next_id;
        self.next_id += 1;
        self.send(json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}))
            .await?;
        loop {
            let line = tokio::time::timeout(Duration::from_secs(600), self.lines.next_line())
                .await
                .context("the server did not answer")??
                .context("the server closed its output")?;
            let reply: Value = serde_json::from_str(&line)?;
            if reply["id"] == json!(id) {
                return Ok(reply);
            }
        }
    }

    /// Calls a tool; returns its JSON result, or the error text as Err
    async fn tool(&mut self, name: &str, args: Value) -> Result<Value> {
        let reply = self
            .rpc("tools/call", json!({"name": name, "arguments": args}))
            .await?;
        let result = &reply["result"];
        let text = result["content"]
            .as_array()
            .and_then(|c| c.iter().rev().find(|b| b["type"] == "text"))
            .and_then(|b| b["text"].as_str())
            .unwrap_or_default()
            .to_string();
        if result["isError"] == json!(true) || reply.get("error").is_some() {
            bail!("{name} failed: {text}{}", reply["error"]);
        }
        Ok(serde_json::from_str(&text).unwrap_or(Value::String(text)))
    }
}

#[tokio::test]
#[ignore = "needs a built 3DMMEx with the automation bridge and text-to-speech"]
async fn script_scene_builds_a_reviewed_scene() -> Result<()> {
    let mut c = Client::start().await?;
    c.tool("new_movie", json!({"discard_changes": true}))
        .await?;

    // A bad script is rejected before anything is built, naming the entry
    let err = c
        .tool(
            "script_scene",
            json!({"background": "diner", "cast": [{"name": "A", "actor": "Frank"}],
                   "lines": [{"who": "B", "says": "hi"}]}),
        )
        .await
        .unwrap_err()
        .to_string();
    ensure!(err.contains("lines[0]"), "{err}");
    let state = c.tool("get_state", json!({})).await?;
    ensure!(state["movie"]["scene_count"] == 0, "nothing built: {state}");

    let scene = c
        .tool(
            "script_scene",
            json!({
                "background": "diner",
                "title": "TEST",
                "subtitles": true,
                "music": "funny moment",
                "cast": [{"name": "Ann", "actor": "Bella"}, {"name": "Bob", "actor": "Gregory"}],
                "lines": [
                    {"sound": "bell service"},
                    {"who": "Ann", "says": "Is this seat taken?"},
                    {"who": "Bob", "does": "Laugh", "seconds": 1},
                    {"who": "Bob", "says": "Only by my sandwich."},
                    {"wait": 1}
                ]
            }),
        )
        .await?;
    let lines: Vec<Value> = scene["lines"]
        .as_array()
        .context("lines")?
        .iter()
        .filter(|l| l.get("sound").is_none())
        .cloned()
        .collect();
    ensure!(lines.len() == 3, "{scene}");
    // The title plus a subtitle for each of the two spoken lines
    let timeline = c.tool("describe_timeline", json!({"scene": 0})).await?;
    ensure!(
        timeline["text_boxes"].as_array().map(Vec::len) == Some(3),
        "{timeline}"
    );
    // Lines follow one another without overlapping
    for pair in lines.windows(2) {
        ensure!(
            pair[1]["from"].as_i64() > pair[0]["to"].as_i64(),
            "overlap: {pair:?}"
        );
    }
    // Each character got their own voice where voices can be listed
    let voices: Vec<&Value> = scene["cast"]
        .as_array()
        .context("cast")?
        .iter()
        .map(|m| &m["voice"])
        .collect();
    if voices.iter().all(|v| v.is_string()) {
        ensure!(voices[0] != voices[1], "{scene}");
    }
    ensure!(
        scene["shots"].as_array().is_some_and(|s| !s.is_empty()),
        "{scene}"
    );
    ensure!(scene["review"]["verdict"].is_string(), "{scene}");
    // The closing pause is held: the scene lasts past the last line
    let state = c.tool("get_state", json!({"all_scenes": true})).await?;
    let last = state["scenes"][0]["last_frame"].as_i64().unwrap_or(0);
    ensure!(
        last >= lines[2]["to"].as_i64().unwrap_or(0) + 6,
        "the final wait was kept: {state}"
    );

    // Staging: an entrance, a cross to someone and an exit
    let staged = c
        .tool(
            "script_scene",
            json!({
                "background": "diner",
                "camera_style": "wide",
                "cast": [{"name": "Ann", "actor": "Bella"}, {"name": "Cal", "actor": "Gregory"}],
                "lines": [
                    {"who": "Ann", "says": "Anyone here?"},
                    {"who": "Cal", "enters": "right"},
                    {"who": "Cal", "walks_to": "Ann"},
                    {"who": "Cal", "exits": "left"}
                ]
            }),
        )
        .await?;
    let lines = staged["lines"].as_array().context("lines")?;
    ensure!(lines.len() == 4, "{staged}");
    ensure!(
        lines[1]["enters"] == "right" && lines[3]["exits"] == "left",
        "{staged}"
    );
    for issue in staged["review"]["issues"].as_array().context("issues")? {
        ensure!(
            issue["kind"] != "collision",
            "staging left people inside each other: {issue}"
        );
    }
    // Cal joins on his cue (not before) and has left after his exit
    let scene_index = staged["scene_index"].as_i64().context("scene_index")?;
    c.tool("select_scene", json!({"index": scene_index}))
        .await?;
    let on_stage_at = |state: &Value| {
        state["actors"]
            .as_array()
            .and_then(|a| a.iter().find(|a| a["name"] == "Cal"))
            .map(|a| a["on_stage"] == json!(true))
            .unwrap_or(false)
    };
    let early = c.tool("describe_scene", json!({"frame": 1})).await?;
    ensure!(!on_stage_at(&early), "Cal waits for his cue: {early}");
    let late_frame = lines[3]["to"].as_i64().context("exit end")? + 2;
    let late = c.tool("describe_scene", json!({"frame": late_frame})).await;
    if let Ok(late) = late {
        ensure!(!on_stage_at(&late), "Cal has left: {late}");
    }

    // A whole movie: a bad later scene stops it before anything is built,
    // and a character keeps one voice across scenes
    c.tool("new_movie", json!({"discard_changes": true}))
        .await?;
    let cast = json!([{"name": "Ann", "actor": "Bella"}, {"name": "Bob", "actor": "Gregory"}]);
    let good =
        json!({"background": "diner", "cast": cast, "lines": [{"who": "Ann", "says": "Hi."}]});
    let bad =
        json!({"background": "diner", "cast": cast, "lines": [{"who": "Nobody", "says": "Hi."}]});
    let err = c
        .tool("script_movie", json!({"scenes": [good, bad]}))
        .await
        .unwrap_err()
        .to_string();
    ensure!(err.contains("scenes[1]"), "{err}");
    let state = c.tool("get_state", json!({})).await?;
    ensure!(state["movie"]["scene_count"] == 0, "nothing built: {state}");
    let second = json!({"background": "diner", "cast": cast, "subtitles": true,
                        "lines": [{"who": "Bob", "says": "Hello again."}]});
    let movie = c
        .tool(
            "script_movie",
            json!({"scenes": [good, second], "target_seconds": 60}),
        )
        .await?;
    ensure!(
        movie["scenes"].as_array().map(Vec::len) == Some(2),
        "{movie}"
    );
    ensure!(
        movie["length_note"].is_string(),
        "far from the target: {movie}"
    );
    let voice = |scene: usize, who: &str| {
        movie["scenes"][scene]["cast"]
            .as_array()
            .and_then(|c| c.iter().find(|m| m["name"] == who))
            .map(|m| m["voice"].clone())
    };
    ensure!(voice(0, "Bob") == voice(1, "Bob"), "{movie}");

    // The result comes with a picture of what was made
    let reply = c
        .rpc(
            "tools/call",
            json!({"name": "script_scene", "arguments": good}),
        )
        .await?;
    let kinds: Vec<&str> = reply["result"]["content"]
        .as_array()
        .context("content")?
        .iter()
        .filter_map(|b| b["type"].as_str())
        .collect();
    ensure!(
        kinds.contains(&"image") && kinds.last() == Some(&"text"),
        "{kinds:?}"
    );

    c.tool("quit", json!({"discard_changes": true})).await.ok();
    Ok(())
}
