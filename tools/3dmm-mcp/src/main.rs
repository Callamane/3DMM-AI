//! `3dmm-mcp`: stdio MCP server for 3D Movie Maker.
//!
//! Environment:
//! - `THREEDMM_PORT`: bridge port (default 7341)
//! - `THREEDMM_APP`: path to the 3DMMEx executable or .app (default: the
//!   repository's dist/ folder)
//! - `THREEDMM_NO_AUTOLAUNCH=1`: never start the app; require it running
//! - `RUST_LOG`: log filter (logs go to stderr)

use anyhow::{Context, Result};
use rmcp::ServiceExt;
use threedmm_mcp::bridge::DEFAULT_PORT;
use threedmm_mcp::server::ThreeDmmServer;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();

    let port = match std::env::var("THREEDMM_PORT") {
        Ok(value) => value
            .parse()
            .with_context(|| format!("THREEDMM_PORT={value} is not a port number"))?,
        Err(_) => DEFAULT_PORT,
    };
    let autolaunch = std::env::var("THREEDMM_NO_AUTOLAUNCH").map_or(true, |v| v.is_empty() || v == "0");

    let service = ThreeDmmServer::new(port, autolaunch)
        .serve(rmcp::transport::stdio())
        .await
        .context("MCP initialization failed")?;
    service.waiting().await?;
    Ok(())
}
