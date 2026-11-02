//! Finding and starting 3DMMEx with the automation bridge enabled.

use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};

use crate::bridge::BridgeClient;

/// How long to wait for a freshly started app to accept connections.
const STARTUP_TIMEOUT: Duration = Duration::from_secs(90);

/// Resolves the 3DMMEx executable.
///
/// Order: `THREEDMM_APP` (an executable, or a macOS `.app` bundle), then the
/// build output next to this crate (`dist/<preset>/...`), newest first.
pub fn find_app() -> Result<PathBuf> {
    if let Ok(path) = std::env::var("THREEDMM_APP") {
        let path = PathBuf::from(path);
        return executable_in(&path)
            .with_context(|| format!("THREEDMM_APP={} is not a 3DMMEx executable or .app", path.display()));
    }

    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let dist = repo.join("dist");
    let mut candidates: Vec<PathBuf> = std::fs::read_dir(&dist)
        .map(|entries| entries.filter_map(|e| e.ok().map(|e| e.path())).collect())
        .unwrap_or_default();
    // Prefer release-ish builds, then most recently modified
    candidates.sort_by_key(|p| {
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("").to_string();
        let release = name.contains("rel");
        let mtime = std::fs::metadata(p).and_then(|m| m.modified()).ok();
        (std::cmp::Reverse(release), std::cmp::Reverse(mtime))
    });
    for dir in candidates {
        for leaf in ["3D Movie Maker.app", "3dmovie.exe", "3DMOVIE.EXE", "3dmovie"] {
            if let Ok(exe) = executable_in(&dir.join(leaf)) {
                return Ok(exe);
            }
        }
    }
    bail!(
        "could not find 3DMMEx. Build it (cmake --build build/<preset> --target install) or set THREEDMM_APP \
         to the app or executable"
    )
}

fn executable_in(path: &Path) -> Result<PathBuf> {
    if path.extension().and_then(|e| e.to_str()) == Some("app") {
        let exe = path.join("Contents/MacOS/3D Movie Maker");
        if exe.is_file() {
            return Ok(exe);
        }
        bail!("no executable inside {}", path.display());
    }
    if path.is_file() {
        return Ok(path.to_path_buf());
    }
    bail!("{} does not exist", path.display())
}

/// Starts 3DMMEx with `--automation=<port>` and waits until the bridge answers.
pub async fn launch(port: u16) -> Result<(BridgeClient, PathBuf)> {
    let exe = find_app()?;
    let mut command = std::process::Command::new(&exe);
    command
        .arg(format!("--automation={port}"))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        // The app logs a lot of debug noise; keep MCP's stdio clean
        .stderr(Stdio::null());
    if let Some(dir) = exe.parent() {
        command.current_dir(dir);
    }
    command
        .spawn()
        .with_context(|| format!("failed to start {}", exe.display()))?;

    let started = Instant::now();
    loop {
        match BridgeClient::connect(port).await {
            Ok(client) => return Ok((client, exe)),
            Err(err) if started.elapsed() < STARTUP_TIMEOUT => {
                tracing::debug!("waiting for 3DMMEx: {err:#}");
                tokio::time::sleep(Duration::from_millis(500)).await;
            }
            Err(err) => {
                return Err(err.context(format!(
                    "3DMMEx was started but its automation bridge did not answer within {}s",
                    STARTUP_TIMEOUT.as_secs()
                )))
            }
        }
    }
}
