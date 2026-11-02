//! Client for the 3DMMEx automation bridge: newline-delimited JSON over TCP
//! on 127.0.0.1. One request in flight at a time.

use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::net::TcpStream;

/// Bridge protocol version this client speaks.
pub const PROTOCOL_VERSION: i64 = 1;

/// Default bridge port (matches the app's default).
pub const DEFAULT_PORT: u16 = 7341;

/// Error reported by the bridge for a request (as opposed to a transport
/// failure).
#[derive(Debug, Clone)]
pub struct BridgeError {
    pub code: String,
    pub message: String,
}

impl std::fmt::Display for BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} ({})", self.message, self.code)
    }
}

impl std::error::Error for BridgeError {}

pub struct BridgeClient {
    reader: BufReader<OwnedReadHalf>,
    writer: OwnedWriteHalf,
    next_id: u64,
}

impl BridgeClient {
    /// Connects and performs the `hello` handshake.
    pub async fn connect(port: u16) -> Result<Self> {
        let stream = TcpStream::connect(("127.0.0.1", port))
            .await
            .with_context(|| format!("cannot connect to 3DMMEx on 127.0.0.1:{port}"))?;
        stream.set_nodelay(true)?;
        let (read, write) = stream.into_split();
        let mut client = Self {
            reader: BufReader::new(read),
            writer: write,
            next_id: 1,
        };
        let hello = client
            .call("hello", json!({}), Duration::from_secs(10))
            .await?;
        let version = hello.get("protocol").and_then(Value::as_i64).unwrap_or(0);
        if version != PROTOCOL_VERSION {
            bail!("3DMMEx speaks bridge protocol {version}, this server needs {PROTOCOL_VERSION}");
        }
        Ok(client)
    }

    /// Sends one request and waits for its reply. Bridge-level failures come
    /// back as `BridgeError` inside the anyhow error.
    pub async fn call(&mut self, cmd: &str, args: Value, timeout: Duration) -> Result<Value> {
        let id = self.next_id;
        self.next_id += 1;
        let mut line = serde_json::to_string(&json!({ "id": id, "cmd": cmd, "args": args }))?;
        line.push('\n');
        self.writer
            .write_all(line.as_bytes())
            .await
            .context("lost connection to 3DMMEx")?;

        let read = async {
            loop {
                let mut reply = String::new();
                let n = self.reader.read_line(&mut reply).await?;
                if n == 0 {
                    bail!("3DMMEx closed the connection");
                }
                let reply: Value = serde_json::from_str(reply.trim())
                    .with_context(|| format!("malformed reply from 3DMMEx: {reply}"))?;
                // Skip anything that isn't the reply to this request
                if reply.get("id").and_then(Value::as_u64) != Some(id) {
                    continue;
                }
                return Ok(reply);
            }
        };
        let reply = tokio::time::timeout(timeout, read).await.map_err(|_| {
            anyhow!(
                "3DMMEx did not answer '{cmd}' within {}s",
                timeout.as_secs()
            )
        })??;

        if reply.get("ok").and_then(Value::as_bool) == Some(true) {
            return Ok(reply.get("result").cloned().unwrap_or(Value::Null));
        }
        let error = reply.get("error").cloned().unwrap_or(Value::Null);
        Err(BridgeError {
            code: error
                .get("code")
                .and_then(Value::as_str)
                .unwrap_or("unknown")
                .to_string(),
            message: error
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("unknown error")
                .to_string(),
        }
        .into())
    }
}
