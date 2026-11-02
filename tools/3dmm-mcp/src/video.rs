//! Turns the bridge's `export_media` output (per-scene PNG frames and sound
//! cues) into a video file with ffmpeg: scenes joined with their
//! transitions, every sound placed at its frame, mixed to stereo.

use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde_json::{json, Value};
use tokio::process::Command;

/// Output frame rate (the movie's 6 fps is duplicated up; players and editors
/// cope better with a standard rate).
const OUTPUT_FPS: u32 = 30;

pub struct VideoOptions {
    pub scale: u32,
    pub transition_seconds: f64,
    pub audio: bool,
}

/// Finds ffmpeg: `THREEDMM_FFMPEG`, then the PATH, then Homebrew's prefixes.
pub fn find_ffmpeg() -> Result<PathBuf> {
    if let Ok(path) = std::env::var("THREEDMM_FFMPEG") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
        bail!("THREEDMM_FFMPEG={} does not exist", path.display());
    }
    let exe = if cfg!(windows) {
        "ffmpeg.exe"
    } else {
        "ffmpeg"
    };
    let mut dirs: Vec<PathBuf> = std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).collect())
        .unwrap_or_default();
    dirs.extend(["/opt/homebrew/bin", "/usr/local/bin"].map(PathBuf::from));
    for dir in dirs {
        let candidate = dir.join(exe);
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    bail!("export_video needs ffmpeg (macOS: `brew install ffmpeg`), or set THREEDMM_FFMPEG to its path")
}

/// ffmpeg's xfade transition for each 3D Movie Maker transition, and how long
/// it lasts relative to the requested transition time.
fn xfade_for(transition: &str) -> Option<(&'static str, f64)> {
    match transition {
        "dissolve" => Some(("dissolve", 1.0)),
        "fade_to_black" => Some(("fadeblack", 1.0)),
        "fade_to_white" => Some(("fadewhite", 1.0)),
        "black" => Some(("fadeblack", 0.5)),
        _ => None, // cut
    }
}

#[derive(Debug, PartialEq)]
pub struct Timeline {
    /// Output start time of each scene, in seconds
    pub starts: Vec<f64>,
    /// Overlap with the previous scene (0 for a cut)
    pub overlaps: Vec<f64>,
    pub total: f64,
}

/// Lays scenes end to end; a transition overlaps the end of one scene with
/// the start of the next.
pub fn timeline(durations: &[f64], transitions: &[String], transition_seconds: f64) -> Timeline {
    let mut starts = Vec::with_capacity(durations.len());
    let mut overlaps = Vec::with_capacity(durations.len());
    let mut length = 0.0f64;
    for (i, &duration) in durations.iter().enumerate() {
        let overlap = if i == 0 {
            0.0
        } else {
            match xfade_for(&transitions[i - 1]) {
                Some((_, factor)) => {
                    // Never longer than half of either scene
                    let limit = durations[i - 1].min(duration) / 2.0;
                    (transition_seconds * factor).min(limit).max(0.0)
                }
                None => 0.0,
            }
        };
        let start = length - overlap;
        starts.push(start);
        overlaps.push(overlap);
        length = start + duration;
    }
    Timeline {
        starts,
        overlaps,
        total: length,
    }
}

/// Builds and runs the ffmpeg command. Returns a summary for the caller.
pub async fn render(manifest: &Value, out: &Path, options: &VideoOptions) -> Result<Value> {
    let ffmpeg = find_ffmpeg()?;
    let fps = manifest["frames_per_second"].as_f64().unwrap_or(6.0);
    let width = manifest["width"].as_u64().unwrap_or(544) as u32 * options.scale;
    let height = manifest["height"].as_u64().unwrap_or(306) as u32 * options.scale;
    let scenes = manifest["scenes"]
        .as_array()
        .context("export_media returned no scenes")?;
    if scenes.is_empty() {
        bail!("the movie has no scenes");
    }

    let durations: Vec<f64> = scenes
        .iter()
        .map(|s| s["frames"].as_f64().unwrap_or(1.0) / fps)
        .collect();
    let transitions: Vec<String> = scenes
        .iter()
        .map(|s| s["transition_after"].as_str().unwrap_or("cut").to_string())
        .collect();
    let plan = timeline(&durations, &transitions, options.transition_seconds);
    let scene_index: Vec<i64> = scenes
        .iter()
        .map(|s| s["index"].as_i64().unwrap_or(0))
        .collect();

    let mut args: Vec<String> = vec!["-y".into(), "-v".into(), "error".into()];
    let mut filters: Vec<String> = Vec::new();

    // Video inputs, one image sequence per scene
    for (i, scene) in scenes.iter().enumerate() {
        let pattern = scene["pattern"]
            .as_str()
            .context("scene without a frame pattern")?;
        args.extend([
            "-framerate".into(),
            format!("{fps}"),
            "-start_number".into(),
            "0".into(),
            "-i".into(),
            pattern.into(),
        ]);
        filters.push(format!(
            "[{i}:v]scale={width}:{height}:flags=neighbor,fps={OUTPUT_FPS},format=yuv420p,settb=AVTB[v{i}]"
        ));
    }
    let mut current = "v0".to_string();
    let mut length = durations[0];
    for i in 1..scenes.len() {
        let next = format!("v{i}");
        let joined = format!("j{i}");
        match xfade_for(&transitions[i - 1]) {
            Some((kind, _)) if plan.overlaps[i] > 0.0 => {
                let offset = length - plan.overlaps[i];
                filters.push(format!(
                    "[{current}][{next}]xfade=transition={kind}:duration={:.3}:offset={:.3}[{joined}]",
                    plan.overlaps[i], offset
                ));
            }
            _ => filters.push(format!("[{current}][{next}]concat=n=2:v=1:a=0[{joined}]")),
        }
        length = plan.starts[i] + durations[i];
        current = joined;
    }
    filters.push(format!("[{current}]null[vout]"));

    // Audio inputs, one per clip (timed by the bridge in milliseconds from
    // the start of its scene)
    let mut audio_labels = Vec::new();
    let mut input = scenes.len();
    let mut skipped = 0usize;
    if options.audio {
        for clip in manifest["clips"].as_array().cloned().unwrap_or_default() {
            let file = clip["file"].as_str().unwrap_or_default();
            let scene = clip["scene"].as_i64().unwrap_or(0);
            let Some(position) = scene_index.iter().position(|&s| s == scene) else {
                skipped += 1;
                continue;
            };
            let scene_ms = durations[position] * 1000.0;
            let start_ms = clip["start_ms"].as_f64().unwrap_or(0.0);
            let end_ms = clip["end_ms"].as_f64().unwrap_or(scene_ms).min(scene_ms);
            let offset_s = clip["offset_ms"].as_f64().unwrap_or(0.0) / 1000.0;
            let start = plan.starts[position] + start_ms / 1000.0;
            let duration = (end_ms - start_ms) / 1000.0;
            if file.is_empty() || duration <= 0.0 {
                skipped += 1;
                continue;
            }
            if clip["loop"].as_bool().unwrap_or(false) {
                args.extend(["-stream_loop".into(), "-1".into()]);
            }
            args.extend(["-i".into(), file.into()]);
            let delay_ms = (start * 1000.0).round().max(0.0) as i64;
            let volume = clip["volume"].as_f64().unwrap_or(1.0);
            let label = format!("a{input}");
            filters.push(format!(
                "[{input}:a]aformat=sample_rates=44100:channel_layouts=stereo,\
                 atrim={offset_s:.3}:{:.3},asetpts=PTS-STARTPTS,adelay={delay_ms}:all=1,volume={volume:.3}[{label}]",
                offset_s + duration
            ));
            audio_labels.push(label);
            input += 1;
        }
    }
    let has_audio = !audio_labels.is_empty();
    if has_audio {
        let inputs: String = audio_labels.iter().map(|l| format!("[{l}]")).collect();
        filters.push(format!(
            // Loudness-normalise the mix to the usual level for online video
            "{inputs}amix=inputs={}:normalize=0:duration=longest,atrim=0:{:.3},\
             loudnorm=I=-16:TP=-1.5:LRA=11,aresample=44100[aout]",
            audio_labels.len(),
            plan.total
        ));
    }

    args.extend(["-filter_complex".into(), filters.join(";")]);
    args.extend(["-map".into(), "[vout]".into()]);
    if has_audio {
        args.extend([
            "-map".into(),
            "[aout]".into(),
            "-c:a".into(),
            "aac".into(),
            "-b:a".into(),
            "160k".into(),
        ]);
    }
    args.extend([
        "-c:v".into(),
        "libx264".into(),
        "-pix_fmt".into(),
        "yuv420p".into(),
        "-crf".into(),
        "18".into(),
        "-movflags".into(),
        "+faststart".into(),
        "-t".into(),
        format!("{:.3}", plan.total),
    ]);
    args.push(out.to_string_lossy().into_owned());

    let output = Command::new(&ffmpeg)
        .args(&args)
        .output()
        .await
        .with_context(|| format!("running {}", ffmpeg.display()))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        bail!(
            "ffmpeg failed: {}",
            stderr.trim().lines().last().unwrap_or("unknown error")
        );
    }
    let bytes = std::fs::metadata(out).map(|m| m.len()).unwrap_or(0);
    Ok(json!({
        "path": out.to_string_lossy(),
        "seconds": (plan.total * 100.0).round() / 100.0,
        "width": width,
        "height": height,
        "scenes": scenes.len(),
        "sounds": audio_labels.len(),
        "sounds_skipped": skipped,
        "bytes": bytes,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cuts_butt_scenes_together() {
        let plan = timeline(&[5.0, 3.0], &["cut".into(), "cut".into()], 1.0);
        assert_eq!(plan.starts, vec![0.0, 5.0]);
        assert_eq!(plan.total, 8.0);
    }

    #[test]
    fn transitions_overlap_scenes() {
        let plan = timeline(
            &[5.0, 4.0, 1.0],
            &["dissolve".into(), "fade_to_black".into(), "cut".into()],
            1.0,
        );
        assert_eq!(plan.starts[1], 4.0);
        // Overlap is limited to half the shorter scene (1 s scene -> 0.5 s)
        assert_eq!(plan.overlaps[2], 0.5);
        assert_eq!(plan.starts[2], 7.5);
        assert_eq!(plan.total, 8.5);
    }
}
