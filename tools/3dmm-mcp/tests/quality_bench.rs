//! A quality benchmark for scripted scenes: the same two scripts (a
//! two-person argument and a three-person scene with an entrance and an
//! exit) staged on every stock set, scored on what a viewer notices:
//! warnings (from review_movie and the shot list), how many camera angles
//! a scene cuts between, and the longest a single shot is held. It prints
//! a table and fails if quality drops below the bar it set when written.
//!
//! Needs a built 3DMMEx with the automation bridge and text-to-speech, and
//! takes a few minutes, so it is ignored by default:
//!   cargo test --release --test quality_bench -- --ignored --nocapture

mod common;

use anyhow::{Context, Result};
use serde_json::{json, Value};

use common::Client;

const PORT: u16 = 7397;

/// The bar: scenes with no warnings at all, and the longest any shot may be
/// held (seconds), except where holding is the right call
const MIN_CLEAN_SHARE: f64 = 0.9;
const MAX_HOLD_SECONDS: f64 = 10.0;
/// (set, script) pairs where one shot is right, checked by staging the
/// script on every camera by hand
const HOLD_IS_RIGHT: &[(&str, &str)] = &[
    // One view with different backdrops: any cut is a jump cut
    ("GE-Black", "dialogue"),
    ("GE-Black", "trio"),
    // Only camera 0 shows both faces at a readable size; every other
    // staging cuts to a far shot where a face is a few pixels
    ("AD-Cave", "dialogue"),
];

struct Row {
    set: String,
    script: &'static str,
    warnings: usize,
    angles: usize,
    longest_hold: f64,
    seconds: f64,
    build_seconds: f64,
    messages: Vec<String>,
}

fn scripts() -> Vec<(&'static str, Value)> {
    let two = json!([{"name": "Ann", "actor": "Nakita"}, {"name": "Bob", "actor": "Patrick"}]);
    let three = json!([{"name": "Ann", "actor": "Nakita"}, {"name": "Bob", "actor": "Patrick"},
                       {"name": "Cy", "actor": "Gregory"}]);
    vec![
        (
            "dialogue",
            json!({"cast": two, "subtitles": true, "preview": false, "lines": [
                {"who": "Ann", "says": "Did you hear that noise last night?", "mood": "scared"},
                {"who": "Bob", "says": "That was me. I was practising the tuba.", "mood": "happy"},
                {"who": "Ann", "says": "At three in the morning?", "mood": "angry", "reaction": "Laugh"},
                {"who": "Bob", "says": "Inspiration doesn't keep office hours.", "mood": "thinking"},
                {"who": "Ann", "does": "Mad, Sad", "seconds": 1.5}]}),
        ),
        (
            "trio",
            json!({"cast": three, "subtitles": true, "preview": false, "lines": [
                {"who": "Cy", "enters": "right"},
                {"who": "Cy", "says": "Sorry I'm late. Who's hungry?", "mood": "excited"},
                {"who": "Ann", "says": "We were hungry an hour ago.", "mood": "angry"},
                {"who": "Bob", "says": "Now we're just grumpy.", "mood": "sad"},
                {"who": "Cy", "exits": "right"}]}),
        ),
    ]
}

fn score(set: &str, script: &'static str, built: &Value, build_seconds: f64) -> Result<Row> {
    let mut messages: Vec<String> = built["review"]["issues"]
        .as_array()
        .context("review issues")?
        .iter()
        .filter(|i| i["severity"] == "warning")
        .filter_map(|i| i["message"].as_str().map(String::from))
        .collect();
    let shots = built["shots"].as_array().context("shots")?;
    messages.extend(
        shots
            .iter()
            .filter_map(|s| s["warning"].as_str().map(String::from)),
    );
    let mut cameras: Vec<i64> = shots.iter().filter_map(|s| s["camera"].as_i64()).collect();
    cameras.sort_unstable();
    cameras.dedup();
    let longest = shots
        .iter()
        .filter_map(|s| Some(s["to"].as_i64()? - s["from"].as_i64()? + 1))
        .max()
        .unwrap_or(0);
    Ok(Row {
        set: set.to_string(),
        script,
        warnings: messages.len(),
        angles: cameras.len(),
        longest_hold: longest as f64 / 6.0,
        seconds: built["seconds"].as_f64().unwrap_or(0.0),
        build_seconds,
        messages,
    })
}

#[tokio::test]
#[ignore = "needs a built 3DMMEx with the automation bridge and text-to-speech; takes minutes"]
async fn scripted_scenes_meet_the_quality_bar() -> Result<()> {
    let mut c = Client::start(PORT).await?;
    let sets: Vec<String> = c.tool("list_backgrounds", json!({})).await?["items"]
        .as_array()
        .context("backgrounds")?
        .iter()
        .filter_map(|b| b["name"].as_str())
        .map(|name| name.trim_end_matches(" Background").to_string())
        .collect();
    let mut rows = Vec::new();
    for set in &sets {
        for (name, script) in scripts() {
            c.tool("new_movie", json!({"discard_changes": true}))
                .await?;
            let mut args = script.clone();
            args["background"] = json!(set);
            let started = std::time::Instant::now();
            let built = c
                .tool("script_scene", args)
                .await
                .with_context(|| format!("{set} {name}"))?;
            rows.push(score(set, name, &built, started.elapsed().as_secs_f64())?);
        }
    }
    c.tool("quit", json!({"discard_changes": true})).await.ok();

    println!(
        "{:<14} {:<9} {:>8} {:>6} {:>9} {:>7} {:>7}",
        "set", "script", "warnings", "angles", "longest s", "length", "build s"
    );
    for r in &rows {
        println!(
            "{:<14} {:<9} {:>8} {:>6} {:>9.1} {:>7.1} {:>7.1}  {}",
            r.set,
            r.script,
            r.warnings,
            r.angles,
            r.longest_hold,
            r.seconds,
            r.build_seconds,
            r.messages.join("; ")
        );
    }
    let clean = rows.iter().filter(|r| r.warnings == 0).count();
    let angles = rows.iter().map(|r| r.angles).sum::<usize>() as f64 / rows.len() as f64;
    let held: Vec<&Row> = rows
        .iter()
        .filter(|r| {
            r.longest_hold > MAX_HOLD_SECONDS
                && !HOLD_IS_RIGHT.contains(&(r.set.as_str(), r.script))
        })
        .collect();
    let build: f64 = rows.iter().map(|r| r.build_seconds).sum();
    println!(
        "{} scenes: {clean} clean, {:.2} angles per scene, {} held over {MAX_HOLD_SECONDS} s, built in {build:.0} s",
        rows.len(),
        angles,
        held.len()
    );
    anyhow::ensure!(
        clean as f64 >= MIN_CLEAN_SHARE * rows.len() as f64,
        "only {clean} of {} scenes have no warnings",
        rows.len()
    );
    anyhow::ensure!(
        held.is_empty(),
        "shots held over {MAX_HOLD_SECONDS} s: {:?}",
        held.iter()
            .map(|r| format!("{} {}", r.set, r.script))
            .collect::<Vec<_>>()
    );
    Ok(())
}
