//! An MCP client for the integration tests: starts the server over stdio
//! and calls its tools as an AI client would.

use std::process::Stdio;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader, Lines};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};
pub struct Client {
    _child: Child,
    stdin: ChildStdin,
    lines: Lines<BufReader<ChildStdout>>,
    next_id: u64,
}

impl Client {
    pub async fn start(port: u16) -> Result<Self> {
        let mut child = Command::new(env!("CARGO_BIN_EXE_3dmm-mcp"))
            .env("THREEDMM_PORT", port.to_string())
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

    pub async fn rpc(&mut self, method: &str, params: Value) -> Result<Value> {
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
    pub async fn tool(&mut self, name: &str, args: Value) -> Result<Value> {
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
