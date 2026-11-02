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

/// Rate the in-between frames are made at when smoothing (the movie's
/// own frames stay; three motion-interpolated ones go between each pair)
const SMOOTH_FPS: u32 = 24;

pub struct VideoOptions {
    pub scale: u32,
    pub transition_seconds: f64,
    pub audio: bool,
    /// Motion-interpolate between the movie's 6 fps frames for smoother
    /// movement (some smearing on fast limbs)
    pub smooth: bool,
    /// Fit the file under this size (two-pass encode at the bitrate that
    /// fits) instead of encoding at constant quality
    pub max_megabytes: Option<f64>,
}

/// Audio bitrate of exports, kbit/s
const AUDIO_KBPS: f64 = 160.0;

/// Video bitrate (kbit/s) that keeps a file of `seconds` under
/// `max_megabytes` with the soundtrack included, leaving 4% for the
/// container; never below a floor that still shows a picture
pub fn video_kbps_for(max_megabytes: f64, seconds: f64, has_audio: bool) -> f64 {
    let total_kbit = max_megabytes * 8.0 * 1024.0 * 0.96;
    let audio_kbit = if has_audio { AUDIO_KBPS * seconds } else { 0.0 };
    ((total_kbit - audio_kbit) / seconds.max(0.1)).max(100.0)
}

async fn run_ffmpeg(ffmpeg: &Path, args: &[String]) -> Result<()> {
    let output = Command::new(ffmpeg)
        .args(args)
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
    Ok(())
}

/// Filter chain for one scene's frames: optional in-betweens at the movie's
/// own resolution, then pixel-exact scaling to the output size and rate
pub fn scene_filter(input: usize, width: u32, height: u32, smooth: bool) -> String {
    let tween = if smooth {
        format!("minterpolate=fps={SMOOTH_FPS}:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1,")
    } else {
        String::new()
    };
    format!(
        "[{input}:v]{tween}scale={width}:{height}:flags=neighbor,fps={OUTPUT_FPS},format=yuv420p,settb=AVTB[v{input}]"
    )
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
        filters.push(scene_filter(i, width, height, options.smooth));
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
            format!("{AUDIO_KBPS}k"),
        ]);
    }
    args.extend([
        "-c:v".into(),
        "libx264".into(),
        "-pix_fmt".into(),
        "yuv420p".into(),
        "-movflags".into(),
        "+faststart".into(),
        "-t".into(),
        format!("{:.3}", plan.total),
    ]);
    match options.max_megabytes {
        None => {
            args.extend([
                "-crf".into(),
                "18".into(),
                out.to_string_lossy().into_owned(),
            ]);
            run_ffmpeg(&ffmpeg, &args).await?;
        }
        Some(megabytes) => {
            // Two passes: the first measures, the second spends the bits
            let kbps = video_kbps_for(megabytes, plan.total, has_audio);
            let log = out.with_extension("2pass");
            let scratch = out.with_extension("pass1.mp4");
            let pass = |n: &str, target: &Path| {
                let mut a = args.clone();
                a.extend([
                    "-b:v".into(),
                    format!("{kbps:.0}k"),
                    "-pass".into(),
                    n.into(),
                    "-passlogfile".into(),
                    log.to_string_lossy().into_owned(),
                    target.to_string_lossy().into_owned(),
                ]);
                a
            };
            let first = run_ffmpeg(&ffmpeg, &pass("1", &scratch)).await;
            let _ = std::fs::remove_file(&scratch);
            let second = match first {
                Ok(()) => run_ffmpeg(&ffmpeg, &pass("2", out)).await,
                Err(err) => Err(err),
            };
            for suffix in ["2pass-0.log", "2pass-0.log.mbtree"] {
                let _ = std::fs::remove_file(out.with_extension(suffix));
            }
            second?;
        }
    }
    let bytes = std::fs::metadata(out).map(|m| m.len()).unwrap_or(0);
    Ok(json!({
        "path": out.to_string_lossy(),
        "seconds": (plan.total * 100.0).round() / 100.0,
        "smooth": options.smooth,
        "max_megabytes": options.max_megabytes,
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
    fn size_cap_leaves_room_for_the_soundtrack() {
        // 25 MB for 2 minutes with sound: about 1.5 Mbit/s of picture
        let kbps = video_kbps_for(25.0, 120.0, true);
        let total_mb = (kbps + AUDIO_KBPS) * 120.0 / 8.0 / 1024.0;
        assert!(total_mb < 25.0 && total_mb > 23.0, "{total_mb}");
        assert!(video_kbps_for(25.0, 120.0, false) > kbps);
        // A cap too small for the length still asks for a picture
        assert_eq!(video_kbps_for(0.1, 600.0, true), 100.0);
    }

    #[test]
    fn smoothing_adds_inbetweens_before_scaling() {
        let plain = scene_filter(0, 1088, 612, false);
        assert!(!plain.contains("minterpolate"));
        assert!(plain.starts_with("[0:v]scale=1088:612"));
        let smooth = scene_filter(2, 1088, 612, true);
        let tween = smooth.find("minterpolate").unwrap();
        assert!(tween < smooth.find("scale=").unwrap(), "{smooth}");
        assert!(smooth.ends_with("[v2]"));
    }

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
