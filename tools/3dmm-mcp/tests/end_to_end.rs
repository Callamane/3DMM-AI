//! End-to-end test: starts 3DMMEx with the automation bridge, builds a
//! two-scene movie, saves it, reopens it and renders frames.
//!
//! It opens a window, so it is ignored by default:
//!     cargo test --test end_to_end -- --ignored --nocapture
//! Set THREEDMM_APP to choose the build; THREEDMM_TEST_OUT to keep outputs.

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{ensure, Context, Result};
use serde_json::{json, Value};
use threedmm_mcp::bridge::BridgeClient;
use threedmm_mcp::launcher;

const PORT: u16 = 7399;

struct Session {
    client: BridgeClient,
}

impl Session {
    async fn call(&mut self, cmd: &str, args: Value) -> Result<Value> {
        let result = self
            .client
            .call(cmd, args.clone(), Duration::from_secs(300))
            .await
            .with_context(|| format!("{cmd} {args}"))?;
        println!("{cmd}: ok");
        Ok(result)
    }
}

/// A one-second 440 Hz tone as a 16-bit mono WAV.
fn write_test_wav(path: &PathBuf) -> Result<()> {
    let rate: u32 = 22050;
    let samples: Vec<i16> = (0..rate)
        .map(|i| {
            let t = i as f64 / rate as f64;
            let envelope = (1.0 - t).min(t * 20.0).min(1.0);
            ((t * 440.0 * std::f64::consts::TAU).sin() * envelope * 12000.0) as i16
        })
        .collect();
    let data_len = (samples.len() * 2) as u32;
    let mut bytes = Vec::with_capacity(44 + data_len as usize);
    bytes.extend_from_slice(b"RIFF");
    bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
    bytes.extend_from_slice(b"WAVEfmt ");
    bytes.extend_from_slice(&16u32.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes()); // PCM
    bytes.extend_from_slice(&1u16.to_le_bytes()); // mono
    bytes.extend_from_slice(&rate.to_le_bytes());
    bytes.extend_from_slice(&(rate * 2).to_le_bytes());
    bytes.extend_from_slice(&2u16.to_le_bytes());
    bytes.extend_from_slice(&16u16.to_le_bytes());
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&data_len.to_le_bytes());
    for sample in samples {
        bytes.extend_from_slice(&sample.to_le_bytes());
    }
    std::fs::write(path, bytes)?;
    Ok(())
}

fn out_dir() -> Result<PathBuf> {
    let dir = match std::env::var("THREEDMM_TEST_OUT") {
        Ok(dir) => PathBuf::from(dir),
        Err(_) => std::env::temp_dir().join("3dmm-e2e"),
    };
    std::fs::create_dir_all(dir.join("frames"))?;
    Ok(dir)
}

fn png_ok(path: &str) -> Result<()> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {path}"))?;
    ensure!(
        bytes.starts_with(b"\x89PNG\r\n\x1a\n"),
        "{path} is not a PNG"
    );
    ensure!(bytes.len() > 10_000, "{path} is suspiciously small");
    Ok(())
}

#[tokio::test(flavor = "multi_thread")]
#[ignore = "launches the 3DMMEx GUI"]
async fn builds_saves_and_reopens_a_two_scene_movie() -> Result<()> {
    let out = out_dir()?;
    let movie = out.join("e2e-two-scenes.3mm");
    let wav = out.join("e2e-beep.wav");
    write_test_wav(&wav)?;
    let _ = std::fs::remove_file(&movie);

    let (client, exe) = launcher::launch(PORT).await?;
    println!("launched {}", exe.display());
    let mut s = Session { client };

    let state = s.call("get_state", json!({})).await?;
    ensure!(
        state["screen"] == "studio",
        "automation should start in the Studio: {state}"
    );
    s.call("new_movie", json!({"discard_changes": true}))
        .await?;

    // ---- Scene 1: an actor walks across the floor and stops; caption; camera change
    s.call("add_scene", json!({"background": "MY-Rest", "camera": 0}))
        .await?;
    let added = s
        .call(
            "add_actor",
            json!({"template": "Billy", "name": "Walker", "position": {"x": -2, "z": 1.5}}),
        )
        .await?;
    ensure!(
        added["actor"]["position"]["stage"]["x"].as_f64().unwrap() < -1.9,
        "placed at x=-2: {added}"
    );
    let walk = s
        .call(
            "move_actor",
            json!({"actor": "Walker", "action": "Walk Fast", "path": [{"x": 2, "z": 1.5}]}),
        )
        .await?;
    let arrive = walk["end_frame"].as_i64().unwrap();
    ensure!(
        walk["distance_metres"].as_f64().unwrap() > 3.5,
        "walked across: {walk}"
    );
    ensure!(arrive > 10, "walking 4 m takes more than 10 frames: {walk}");
    s.call(
        "add_text",
        json!({"text": "Scene 1: the long walk", "kind": "caption", "rect": "top", "frame": 1, "until_frame": 12}),
    )
    .await?;
    s.call("set_camera", json!({"camera": 2, "frame": arrive / 2}))
        .await?;

    let scene1 = s.call("describe_scene", json!({"frame": arrive})).await?;
    ensure!(
        scene1["scene"]["last_frame"].as_i64().unwrap() >= arrive,
        "{scene1}"
    );
    let walker = &scene1["actors"][0];
    ensure!(
        walker["frozen"] == true,
        "actor stops at the end of the path: {walker}"
    );
    ensure!(
        scene1["text_boxes"].as_array().unwrap().len() == 1,
        "one caption: {scene1}"
    );
    s.call("set_transition", json!({"transition": "dissolve"}))
        .await?;

    // ---- Scene 2: two actors, different actions, background sound, imported WAV on an actor
    s.call("add_scene", json!({"background": "MY-Rest", "camera": 1}))
        .await?;
    s.call("add_actor", json!({"template": "Billy", "name": "Al", "position": {"x": -1, "z": 1}, "facing": "right"}))
        .await?;
    s.call(
        "add_actor",
        json!({"template": "Bino", "name": "Bo", "position": {"x": 1, "z": 1}, "facing": "left"}),
    )
    .await?;
    s.call(
        "animate",
        json!({"actor": "Al", "action": "Mad, Sad", "seconds": 3, "frame": 1}),
    )
    .await?;
    s.call(
        "animate",
        json!({"actor": "Bo", "action": "Laugh", "seconds": 3, "frame": 1}),
    )
    .await?;
    let sounds = s.call("list_sounds", json!({"type": "music"})).await?;
    let music = sounds["items"][0]["id"]
        .as_str()
        .context("no music")?
        .to_string();
    s.call(
        "attach_sound",
        json!({"sound": music, "target": "scene", "loop": true, "frame": 1}),
    )
    .await?;
    let imported = s
        .call(
            "import_sound",
            json!({"path": wav, "type": "sfx", "name": "beep"}),
        )
        .await?;
    let sound_id = imported["sound_id"].as_str().unwrap().to_string();
    s.call(
        "attach_sound",
        json!({"sound": sound_id, "target": "Bo", "frame": 6}),
    )
    .await?;

    let scene2 = s.call("describe_scene", json!({"frame": 6})).await?;
    let actions: Vec<&str> = scene2["actors"]
        .as_array()
        .unwrap()
        .iter()
        .map(|a| a["action"]["name"].as_str().unwrap())
        .collect();
    ensure!(
        actions.len() == 2 && actions[0] != actions[1],
        "different actions: {actions:?}"
    );

    // ---- Render, save
    let r1 = s
        .call(
            "render_frame",
            json!({"frame": 6, "path": out.join("scene2-f6.png")}),
        )
        .await?;
    png_ok(r1["path"].as_str().unwrap())?;
    s.call("select_scene", json!({"index": 0})).await?;
    for frame in [1, arrive / 2, arrive] {
        let path = out.join(format!("scene1-f{frame}.png"));
        let r = s
            .call("render_frame", json!({"frame": frame, "path": path}))
            .await?;
        png_ok(r["path"].as_str().unwrap())?;
    }
    let exported = s
        .call(
            "export_frames",
            json!({"scene": 0, "dir": out.join("frames"), "step": 4}),
        )
        .await?;
    ensure!(exported["count"].as_i64().unwrap() >= 3, "{exported}");

    s.call("save_movie", json!({"path": movie})).await?;
    ensure!(movie.is_file(), "movie saved");

    // ---- Reopen and check it round-trips
    s.call("open_movie", json!({"path": movie})).await?;
    let state = s.call("get_state", json!({})).await?;
    ensure!(state["movie"]["scene_count"] == 2, "{state}");
    let reopened = s.call("describe_scene", json!({"frame": 1})).await?;
    ensure!(
        reopened["actors"].as_array().unwrap().len() == 1,
        "{reopened}"
    );
    s.call("select_scene", json!({"index": 1})).await?;
    let reopened2 = s.call("describe_scene", json!({"frame": 1})).await?;
    ensure!(
        reopened2["actors"].as_array().unwrap().len() == 2,
        "{reopened2}"
    );

    // ---- Editing: duplicate, reorder, trim, text
    s.call("duplicate_scene", json!({"index": 1})).await?;
    s.call("move_scene", json!({"from": 2, "to": 0})).await?;
    let state = s.call("get_state", json!({"all_scenes": true})).await?;
    ensure!(state["movie"]["scene_count"] == 3, "{state}");
    s.call("select_scene", json!({"index": 1})).await?;
    let trimmed = s
        .call("trim_scene", json!({"keep_from": 1, "keep_to": 12}))
        .await?;
    ensure!(
        trimmed["last_frame"].as_i64().unwrap() - trimmed["first_frame"].as_i64().unwrap() == 11,
        "{trimmed}"
    );
    s.call(
        "edit_text",
        json!({"index": 0, "text": "Edited title", "color": "red"}),
    )
    .await?;

    // ---- Review: every scene checked, issues carry frames and a fix
    let review = s.call("review_movie", json!({})).await?;
    ensure!(review["summary"]["scenes"] == 3, "{review}");
    for issue in review["issues"].as_array().context("issues")? {
        ensure!(
            issue["message"].is_string() && issue["fix"].is_string(),
            "{issue}"
        );
    }

    // ---- Video: frames and audio cues, then ffmpeg (skipped without ffmpeg)
    let media_dir = out.join("media");
    std::fs::create_dir_all(&media_dir)?;
    let manifest = s.call("export_media", json!({"dir": media_dir})).await?;
    ensure!(
        manifest["scenes"].as_array().unwrap().len() == 3,
        "{manifest}"
    );
    let cue_files: Vec<&str> = manifest["clips"]
        .as_array()
        .unwrap()
        .iter()
        .filter_map(|c| c["file"].as_str())
        .collect();
    ensure!(
        cue_files.iter().any(|f| f.ends_with(".wav")),
        "imported WAV in the cues: {manifest}"
    );
    if threedmm_mcp::video::find_ffmpeg().is_ok() {
        let mp4 = out.join("e2e.mp4");
        let options = threedmm_mcp::video::VideoOptions {
            scale: 1,
            transition_seconds: 1.0,
            audio: true,
        };
        let summary = threedmm_mcp::video::render(&manifest, &mp4, &options).await?;
        println!("video: {summary}");
        ensure!(summary["sounds"].as_u64().unwrap() >= 2, "{summary}");
        ensure!(std::fs::metadata(&mp4)?.len() > 50_000, "video written");
    } else {
        println!("ffmpeg not found; skipping the MP4 step");
    }

    let played = s
        .call(
            "play",
            json!({"from_start": true, "wait": true, "timeout_seconds": 180}),
        )
        .await?;
    ensure!(played["finished"] == true, "{played}");

    // ---- A set with its own palette, exported without being on screen
    // first, must match a render made after the screen showed it
    s.call("add_scene", json!({"background": "HH-Dining"}))
        .await?;
    let iscen = 3;
    s.call("select_scene", json!({"index": 0})).await?;
    let palette_dir = out.join("palette");
    std::fs::create_dir_all(&palette_dir)?;
    let exported = s
        .call(
            "export_frames",
            json!({"dir": palette_dir, "scene": iscen, "from": 1, "to": 1}),
        )
        .await?;
    let exported_png = exported["sample_paths"][0]
        .as_str()
        .context("sample_paths")?
        .to_string();
    s.call("select_scene", json!({"index": iscen})).await?;
    let direct_png = palette_dir.join("direct.png");
    s.call("render_frame", json!({"frame": 1, "path": direct_png}))
        .await?;
    ensure!(
        std::fs::read(&exported_png)? == std::fs::read(&direct_png)?,
        "{exported_png} differs from {}",
        direct_png.display()
    );

    println!("movie: {}", movie.display());
    s.call("quit", json!({"discard_changes": true})).await?;
    Ok(())
}
