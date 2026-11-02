//! Screenplays for `script_scene`: parsing and checking a scene description
//! (set, cast, lines) before anything is built, plus the small decisions
//! the builder needs (voices, how long a title stays up).

use serde_json::Value;

#[derive(Debug, Clone, PartialEq)]
pub struct CastMember {
    pub name: String,
    /// Actor template (name, description word or id) for add_actor
    pub actor: String,
    pub voice: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Line {
    /// `who` speaks `text`, acting it with `action` (default: talk)
    Say {
        who: String,
        text: String,
        action: Option<Value>,
    },
    /// `who` does `action` on the spot for `seconds`
    Do {
        who: String,
        action: Value,
        seconds: f64,
    },
    /// Nobody starts anything for `seconds`
    Wait { seconds: f64 },
    /// `who` walks on from off-screen `side` to their place in the group
    Enter { who: String, side: Side },
    /// `who` walks off-screen to `side` and leaves the stage
    Exit { who: String, side: Side },
    /// `who` walks to a point or up to another character, then faces them
    WalkTo { who: String, target: Target },
    /// `who` throws a new `prop` at another character, who may react
    Throw {
        who: String,
        prop: String,
        at: String,
        reaction: Option<Value>,
        /// Sound effect played on the target when it hits
        hit_sound: Option<String>,
    },
    /// A sound effect starts, on a character or on the scene; the next line
    /// doesn't wait for it
    Sound { sound: String, on: Option<String> },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Side {
    Left,
    Right,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Target {
    Point { x: f64, z: f64 },
    Actor(String),
}

impl Line {
    /// The character the line is about, if any
    pub fn who(&self) -> Option<&str> {
        match self {
            Line::Say { who, .. }
            | Line::Do { who, .. }
            | Line::Enter { who, .. }
            | Line::Exit { who, .. }
            | Line::WalkTo { who, .. }
            | Line::Throw { who, .. } => Some(who),
            Line::Sound { on, .. } => on.as_deref(),
            Line::Wait { .. } => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Script {
    pub background: String,
    pub camera: Option<i64>,
    pub title: Option<String>,
    pub layout: String,
    pub camera_style: String,
    pub transition: Option<String>,
    /// Caption every spoken line at the bottom while it is said
    pub subtitles: bool,
    /// Scene music by name ("none" silences the set's own music)
    pub music: Option<String>,
    pub cast: Vec<CastMember>,
    pub lines: Vec<Line>,
}

const MAX_CAST: usize = 8;
const MAX_LINES: usize = 200;
const MAX_SECONDS: f64 = 60.0;

fn string(v: &Value, key: &str) -> Option<String> {
    v.get(key)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(String::from)
}

fn seconds(v: &Value, key: &str, at: &str) -> Result<f64, String> {
    let s = v
        .get(key)
        .and_then(Value::as_f64)
        .ok_or_else(|| format!("{at}: '{key}' must be a number of seconds"))?;
    if !(s > 0.0 && s <= MAX_SECONDS) {
        return Err(format!(
            "{at}: '{key}' must be between 0 and {MAX_SECONDS} seconds"
        ));
    }
    Ok(s)
}

/// Parses and checks a script, naming the exact entry at fault.
pub fn parse(args: &Value) -> Result<Script, String> {
    let background = string(args, "background")
        .ok_or("'background' is required (a set name or description word, e.g. \"diner\")")?;
    let layout = string(args, "layout").unwrap_or_else(|| "conversation".into());
    if layout != "conversation" && layout != "line" {
        return Err("'layout' must be \"conversation\" or \"line\"".into());
    }
    let camera_style = string(args, "camera_style").unwrap_or_else(|| "coverage".into());
    if !["coverage", "wide", "none"].contains(&camera_style.as_str()) {
        return Err("'camera_style' must be \"coverage\", \"wide\" or \"none\"".into());
    }

    let cast_json = args
        .get("cast")
        .and_then(Value::as_array)
        .filter(|a| !a.is_empty())
        .ok_or("'cast' must list at least one character: [{\"name\", \"actor\", \"voice\"}]")?;
    if cast_json.len() > MAX_CAST {
        return Err(format!("at most {MAX_CAST} characters per scene"));
    }
    let mut cast: Vec<CastMember> = Vec::new();
    for (i, member) in cast_json.iter().enumerate() {
        let at = format!("cast[{i}]");
        let name = string(member, "name").ok_or(format!("{at}: 'name' is required"))?;
        let actor = string(member, "actor").ok_or(format!(
            "{at}: 'actor' is required (a template such as \"Frank\", or a description word like \"robot\")"
        ))?;
        if cast.iter().any(|c| c.name.eq_ignore_ascii_case(&name)) {
            return Err(format!("{at}: two characters are called '{name}'"));
        }
        cast.push(CastMember {
            name,
            actor,
            voice: string(member, "voice"),
        });
    }
    let names = || {
        cast.iter()
            .map(|c| c.name.as_str())
            .collect::<Vec<_>>()
            .join(", ")
    };

    let lines_json = args
        .get("lines")
        .and_then(Value::as_array)
        .ok_or("'lines' must be a list of {who, says} / {who, does, seconds} / {wait}")?;
    if lines_json.len() > MAX_LINES {
        return Err(format!("at most {MAX_LINES} lines per scene"));
    }
    let mut lines = Vec::new();
    for (i, line) in lines_json.iter().enumerate() {
        let at = format!("lines[{i}]");
        let kinds = LINE_KINDS
            .iter()
            .filter(|k| line.get(**k).is_some())
            .count();
        if kinds != 1 {
            return Err(format!(
                "{at}: give exactly one of 'says' (dialogue), 'does' (an action), 'wait' (seconds), \
                 'enters' / 'exits' (\"left\" or \"right\"), 'walks_to' (a character or {{x, z}}), 'throws' \
                 (a prop, with 'at') or 'sound' (an effect)"
            ));
        }
        if line.get("wait").is_some() {
            lines.push(Line::Wait {
                seconds: seconds(line, "wait", &at)?,
            });
            continue;
        }
        if line.get("sound").is_some() {
            let sound = string(line, "sound").ok_or(format!(
                "{at}: 'sound' must name a sound effect (list_sounds type sfx), e.g. \"bump\""
            ))?;
            let on = match string(line, "on") {
                Some(name) => Some(
                    cast.iter()
                        .find(|c| c.name.eq_ignore_ascii_case(&name))
                        .map(|c| c.name.clone())
                        .ok_or(format!("{at}: '{name}' is not in the cast ({})", names()))?,
                ),
                None => None,
            };
            lines.push(Line::Sound { sound, on });
            continue;
        }
        let who = string(line, "who").ok_or(format!("{at}: 'who' is required"))?;
        let Some(member) = cast.iter().find(|c| c.name.eq_ignore_ascii_case(&who)) else {
            return Err(format!("{at}: '{who}' is not in the cast ({})", names()));
        };
        let who = member.name.clone();
        let me = who.clone();
        let cast_name = |key: &str| -> Result<String, String> {
            let name = string(line, key).ok_or(format!("{at}: '{key}' must name a character"))?;
            let found = cast
                .iter()
                .find(|c| c.name.eq_ignore_ascii_case(&name))
                .ok_or(format!("{at}: '{name}' is not in the cast ({})", names()))?;
            if found.name == me {
                return Err(format!("{at}: '{me}' can't {key} themselves"));
            }
            Ok(found.name.clone())
        };
        if line.get("enters").is_some() || line.get("exits").is_some() {
            let key = if line.get("enters").is_some() {
                "enters"
            } else {
                "exits"
            };
            let side = match string(line, key)
                .as_deref()
                .map(str::to_lowercase)
                .as_deref()
            {
                Some("left") => Side::Left,
                Some("right") => Side::Right,
                _ => return Err(format!("{at}: '{key}' must be \"left\" or \"right\"")),
            };
            lines.push(if key == "enters" {
                Line::Enter { who, side }
            } else {
                Line::Exit { who, side }
            });
        } else if let Some(target) = line.get("walks_to") {
            let target = if target.is_string() {
                Target::Actor(cast_name("walks_to")?)
            } else {
                match (
                    target.get("x").and_then(Value::as_f64),
                    target.get("z").and_then(Value::as_f64),
                ) {
                    (Some(x), Some(z)) => Target::Point { x, z },
                    _ => {
                        return Err(format!(
                        "{at}: 'walks_to' must be a character or a stage point {{x, z}} in metres"
                    ))
                    }
                }
            };
            lines.push(Line::WalkTo { who, target });
        } else if line.get("throws").is_some() {
            let prop = string(line, "throws").ok_or(format!(
                "{at}: 'throws' must be a prop to throw (\"Sphere\" makes a ball; list_templates type prop)"
            ))?;
            lines.push(Line::Throw {
                who,
                prop,
                at: cast_name("at")?,
                reaction: line.get("reaction").cloned(),
                hit_sound: string(line, "hit_sound"),
            });
        } else if let Some(text) = string(line, "says") {
            lines.push(Line::Say {
                who,
                text,
                action: line.get("action").cloned(),
            });
        } else if line.get("says").is_some() {
            return Err(format!("{at}: 'says' must be the words to speak"));
        } else {
            let action = line.get("does").cloned().unwrap_or(Value::Null);
            if !(action.is_string() || action.is_number()) {
                return Err(format!(
                    "{at}: 'does' must be an action name (list_actions shows them)"
                ));
            }
            lines.push(Line::Do {
                who,
                action,
                seconds: if line.get("seconds").is_some() {
                    seconds(line, "seconds", &at)?
                } else {
                    2.0
                },
            });
        }
    }
    if !lines
        .iter()
        .any(|l| !matches!(l, Line::Wait { .. } | Line::Sound { .. }))
    {
        return Err("'lines' needs at least one line that someone says or does".into());
    }
    check_staging(&cast, &lines)?;

    Ok(Script {
        background,
        camera: args.get("camera").and_then(Value::as_i64),
        title: string(args, "title"),
        layout,
        camera_style,
        transition: string(args, "transition"),
        subtitles: args
            .get("subtitles")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        music: string(args, "music"),
        cast,
        lines,
    })
}

const LINE_KINDS: [&str; 8] = [
    "says", "does", "wait", "enters", "exits", "walks_to", "throws", "sound",
];

/// Characters whose first line is an entrance start off stage
pub fn starts_off_stage(cast_member: &str, lines: &[Line]) -> bool {
    lines
        .iter()
        .find(|l| {
            l.who() == Some(cast_member)
                || matches!(l, Line::Throw { at, .. } if at == cast_member)
                || matches!(l, Line::WalkTo { target: Target::Actor(a), .. } if a == cast_member)
        })
        .is_some_and(|l| matches!(l, Line::Enter { who, .. } if who == cast_member))
}

/// Characters can only act while on stage: not before they enter, not
/// after they exit (unless they enter again), and nobody throws at or walks
/// up to someone who isn't there.
fn check_staging(cast: &[CastMember], lines: &[Line]) -> Result<(), String> {
    let mut on_stage: Vec<(String, bool)> = cast
        .iter()
        .map(|c| (c.name.clone(), !starts_off_stage(&c.name, lines)))
        .collect();
    let here = |on_stage: &Vec<(String, bool)>, name: &str| {
        on_stage.iter().any(|(n, on)| n == name && *on)
    };
    for (i, line) in lines.iter().enumerate() {
        let at = format!("lines[{i}]");
        let Some(who) = line.who() else { continue };
        match line {
            Line::Enter { .. } => {
                if here(&on_stage, who) {
                    return Err(format!("{at}: {who} enters but is already on stage"));
                }
            }
            _ if !here(&on_stage, who) => {
                return Err(format!(
                    "{at}: {who} isn't on stage here (they haven't entered yet, or have exited)"
                ));
            }
            _ => {}
        }
        for other in match line {
            Line::Throw { at, .. } => vec![at.as_str()],
            Line::WalkTo {
                target: Target::Actor(a),
                ..
            } => vec![a.as_str()],
            _ => vec![],
        } {
            if !here(&on_stage, other) {
                return Err(format!("{at}: {other} isn't on stage at this point"));
            }
        }
        if let Some(entry) = on_stage.iter_mut().find(|(n, _)| n == who) {
            match line {
                Line::Enter { .. } => entry.1 = true,
                Line::Exit { .. } => entry.1 = false,
                _ => {}
            }
        }
    }
    Ok(())
}

/// Stage x just past the left or right screen edge at depth z, from the
/// camera's visible floor (x at the bottom and middle of the screen and the
/// depths where they were measured)
pub fn offstage_x(
    edge_near: (f64, f64),
    edge_far: (f64, f64),
    z: f64,
    margin: f64,
    side: Side,
) -> f64 {
    let ((x0, z0), (x1, z1)) = (edge_near, edge_far);
    let t = if (z1 - z0).abs() > 1e-6 {
        (z - z0) / (z1 - z0)
    } else {
        0.0
    };
    let x = x0 + (x1 - x0) * t;
    match side {
        Side::Left => x - margin,
        Side::Right => x + margin,
    }
}

/// Clearance kept between a walker's stopping spot and anyone else
const STOP_CLEARANCE: f64 = 0.65;

/// The free spot nearest `spot` (stage x, z): `spot` itself if nobody stands
/// within STOP_CLEARANCE of it, else the closest of rings of candidates
/// around it. `taken` are others' positions.
pub fn clear_spot(spot: (f64, f64), taken: &[(f64, f64)]) -> (f64, f64) {
    let free = |p: (f64, f64)| {
        taken
            .iter()
            .all(|t| (t.0 - p.0).hypot(t.1 - p.1) >= STOP_CLEARANCE)
    };
    if free(spot) {
        return spot;
    }
    for ring in 1..=6 {
        let r = ring as f64 * 0.25;
        // Toward the camera first (lower z), so the walker stays in view
        let mut best: Option<(f64, f64)> = None;
        for k in 0..16 {
            let a = std::f64::consts::TAU * k as f64 / 16.0;
            let p = (spot.0 + r * a.sin(), spot.1 - r * a.cos());
            if free(p) && best.is_none_or(|b| p.1 < b.1 - 1e-9) {
                best = Some(p);
            }
        }
        if let Some(p) = best {
            return p;
        }
    }
    spot
}

/// Installed voices from `say -v '?'` output: (name, locale)
pub fn parse_say_voices(output: &str) -> Vec<(String, String)> {
    output
        .lines()
        .filter_map(|line| {
            let line = line.split('#').next()?.trim_end();
            // The name may contain spaces; the locale is the last column
            let (name, locale) = line.rsplit_once(char::is_whitespace)?;
            let locale = locale.trim();
            let name = name.trim();
            let looks_like_locale = locale.len() >= 5 && locale.as_bytes()[2] == b'_';
            (looks_like_locale && !name.is_empty()).then(|| (name.to_string(), locale.to_string()))
        })
        .collect()
}

/// Voices that sound like ordinary characters, in the order they are handed
/// out when the script doesn't choose
const PREFERRED_VOICES: &[&str] = &[
    "Daniel", "Samantha", "Fred", "Karen", "Ralph", "Moira", "Rishi", "Tessa", "Albert", "Kathy",
];

/// Gives every character without a voice a different installed voice, not
/// one already chosen. Leaves them unset when there's nothing to choose from.
pub fn assign_voices(cast: &mut [CastMember], installed: &[(String, String)]) {
    let is_installed = |v: &str| installed.iter().any(|(name, _)| name == v);
    let mut pool: Vec<String> = PREFERRED_VOICES
        .iter()
        .filter(|v| is_installed(v))
        .map(|v| v.to_string())
        .collect();
    // Then any other English voice
    for (name, locale) in installed {
        if locale.starts_with("en_") && !pool.contains(name) {
            pool.push(name.clone());
        }
    }
    pool.retain(|v| !cast.iter().any(|c| c.voice.as_deref() == Some(v.as_str())));
    let mut next = pool.into_iter();
    for member in cast.iter_mut().filter(|c| c.voice.is_none()) {
        member.voice = next.next();
    }
}

/// How long a title or subtitle stays up: long enough to read (a second
/// plus three words a second), at least two seconds
pub fn title_seconds(text: &str) -> f64 {
    (1.0 + text.split_whitespace().count() as f64 / 3.0).max(2.0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn sample() -> Value {
        json!({
            "background": "diner",
            "cast": [
                {"name": "Frank", "actor": "Frank", "voice": "Daniel"},
                {"name": "Greg", "actor": "Gregory"}
            ],
            "lines": [
                {"who": "Frank", "says": "You took my fries."},
                {"who": "greg", "does": "Laugh", "seconds": 1.5},
                {"wait": 1},
                {"who": "Greg", "says": "Prove it.", "action": "Yell"}
            ]
        })
    }

    #[test]
    fn parses_a_script() {
        let script = parse(&sample()).unwrap();
        assert_eq!(script.cast.len(), 2);
        assert_eq!(script.layout, "conversation");
        assert_eq!(script.camera_style, "coverage");
        assert_eq!(script.lines.len(), 4);
        // Names match case-insensitively and come back as the cast spells them
        assert!(
            matches!(&script.lines[1], Line::Do { who, seconds, .. } if who == "Greg" && *seconds == 1.5)
        );
        assert!(matches!(&script.lines[2], Line::Wait { seconds } if *seconds == 1.0));
        assert!(matches!(
            &script.lines[3],
            Line::Say {
                action: Some(_),
                ..
            }
        ));
    }

    #[test]
    fn points_at_the_broken_entry() {
        let mut bad = sample();
        bad["lines"][3]["who"] = json!("Gerg");
        let err = parse(&bad).unwrap_err();
        assert!(
            err.contains("lines[3]") && err.contains("Frank, Greg"),
            "{err}"
        );

        let mut both = sample();
        both["lines"][0]["does"] = json!("Wave");
        assert!(parse(&both).unwrap_err().contains("exactly one"));

        let mut dup = sample();
        dup["cast"][1]["name"] = json!("frank");
        assert!(parse(&dup).unwrap_err().contains("two characters"));

        let mut long = sample();
        long["lines"][2]["wait"] = json!(500);
        assert!(parse(&long).unwrap_err().contains("lines[2]"));

        assert!(parse(&json!({"cast": [], "lines": []}))
            .unwrap_err()
            .contains("background"));
        let only_waits = json!({"background": "diner", "cast": [{"name": "A", "actor": "Frank"}], "lines": [{"wait": 2}]});
        assert!(parse(&only_waits)
            .unwrap_err()
            .contains("at least one line"));
    }

    fn staged(lines: Value) -> Result<Script, String> {
        parse(&json!({
            "background": "diner",
            "cast": [{"name": "Ann", "actor": "Bella"}, {"name": "Bob", "actor": "Gregory"}],
            "lines": lines
        }))
    }

    #[test]
    fn parses_staging_lines() {
        let script = staged(json!([
            {"who": "Bob", "enters": "left"},
            {"who": "Bob", "walks_to": "ann"},
            {"who": "Ann", "throws": "Sphere", "at": "Bob", "reaction": "Mad"},
            {"who": "Ann", "walks_to": {"x": 1, "z": 2}},
            {"who": "Bob", "exits": "RIGHT"}
        ]))
        .unwrap();
        assert!(matches!(
            &script.lines[0],
            Line::Enter {
                side: Side::Left,
                ..
            }
        ));
        assert!(
            matches!(&script.lines[1], Line::WalkTo { target: Target::Actor(a), .. } if a == "Ann")
        );
        assert!(
            matches!(&script.lines[2], Line::Throw { at, reaction: Some(_), .. } if at == "Bob")
        );
        assert!(
            matches!(&script.lines[3], Line::WalkTo { target: Target::Point { x, z }, .. } if *x == 1.0 && *z == 2.0)
        );
        assert!(matches!(
            &script.lines[4],
            Line::Exit {
                side: Side::Right,
                ..
            }
        ));
        assert!(starts_off_stage("Bob", &script.lines));
        assert!(!starts_off_stage("Ann", &script.lines));
    }

    #[test]
    fn rejects_impossible_staging() {
        let err = staged(json!([{"who": "Bob", "says": "Hi"}, {"who": "Bob", "enters": "left"}]))
            .unwrap_err();
        assert!(
            err.contains("lines[1]") && err.contains("already on stage"),
            "{err}"
        );
        let err = staged(json!([{"who": "Bob", "exits": "left"}, {"who": "Bob", "says": "Bye"}]))
            .unwrap_err();
        assert!(
            err.contains("lines[1]") && err.contains("isn't on stage"),
            "{err}"
        );
        let err = staged(
            json!([{"who": "Bob", "exits": "left"}, {"who": "Ann", "throws": "Cube", "at": "Bob"}]),
        )
        .unwrap_err();
        assert!(err.contains("Bob isn't on stage"), "{err}");
        let err = staged(json!([{"who": "Ann", "walks_to": "Ann"}])).unwrap_err();
        assert!(err.contains("themselves"), "{err}");
        let err = staged(json!([{"who": "Ann", "enters": "up"}])).unwrap_err();
        assert!(err.contains("\"left\" or \"right\""), "{err}");
        // Exit and come back is fine
        assert!(staged(json!([{"who": "Bob", "exits": "left"}, {"who": "Bob", "enters": "right"}, {"who": "Bob", "says": "Back!"}])).is_ok());
    }

    #[test]
    fn parses_sound_and_subtitles() {
        let script = parse(&json!({
            "background": "diner",
            "subtitles": true,
            "music": "funny moment",
            "cast": [{"name": "Ann", "actor": "Bella"}, {"name": "Bob", "actor": "Gregory"}],
            "lines": [
                {"sound": "bell service"},
                {"who": "Ann", "says": "Order up!"},
                {"sound": "chair creak", "on": "bob"},
                {"who": "Ann", "throws": "Sphere", "at": "Bob", "hit_sound": "bump"}
            ]
        }))
        .unwrap();
        assert!(script.subtitles);
        assert_eq!(script.music.as_deref(), Some("funny moment"));
        assert!(matches!(&script.lines[0], Line::Sound { on: None, .. }));
        assert!(matches!(&script.lines[2], Line::Sound { on: Some(n), .. } if n == "Bob"));
        assert!(matches!(&script.lines[3], Line::Throw { hit_sound: Some(s), .. } if s == "bump"));
        // Sounds alone aren't a scene
        let err = parse(&json!({"background": "diner", "cast": [{"name": "A", "actor": "Frank"}], "lines": [{"sound": "bump"}]}))
            .unwrap_err();
        assert!(err.contains("at least one line"), "{err}");
        // A sound on someone off stage is caught
        let err = parse(&json!({"background": "diner", "cast": [{"name": "A", "actor": "Frank"}, {"name": "B", "actor": "Bella"}],
                                "lines": [{"who": "A", "says": "hi"}, {"who": "B", "exits": "left"}, {"sound": "bump", "on": "B"}]}))
            .unwrap_err();
        assert!(err.contains("lines[2]"), "{err}");
    }

    #[test]
    fn walks_stop_on_a_free_spot() {
        assert_eq!(clear_spot((1.0, 2.0), &[(3.0, 2.0)]), (1.0, 2.0));
        let taken = [(1.0, 2.0), (1.6, 2.0)];
        let spot = clear_spot((1.1, 2.0), &taken);
        for t in taken {
            assert!(
                (t.0 - spot.0).hypot(t.1 - spot.1) >= STOP_CLEARANCE - 1e-9,
                "{spot:?}"
            );
        }
        assert!(spot.1 < 2.0, "steps toward the camera: {spot:?}");
    }

    #[test]
    fn offstage_is_past_the_screen_edge() {
        // Left edge at x=-2 near (z=0) widening to x=-5 far (z=3)
        let x = offstage_x((-2.0, 0.0), (-5.0, 3.0), 1.5, 1.0, Side::Left);
        assert!((x - -4.5).abs() < 1e-9, "{x}");
        let x = offstage_x((2.0, 0.0), (5.0, 3.0), 3.0, 1.0, Side::Right);
        assert!((x - 6.0).abs() < 1e-9, "{x}");
    }

    #[test]
    fn reads_say_voice_list() {
        let out = "Albert              en_US    # Hello! My name is Albert.\n\
                   Eddy (English (UK)) en_GB    # Hello! My name is Eddy.\n\
                   Amélie              fr_CA    # Bonjour!\n\
                   garbage\n";
        let voices = parse_say_voices(out);
        assert_eq!(voices.len(), 3);
        assert_eq!(voices[1], ("Eddy (English (UK))".into(), "en_GB".into()));
    }

    #[test]
    fn hands_out_different_voices() {
        let installed: Vec<(String, String)> = [
            ("Daniel", "en_GB"),
            ("Fred", "en_US"),
            ("Zoe", "en_US"),
            ("Anna", "de_DE"),
        ]
        .iter()
        .map(|(n, l)| (n.to_string(), l.to_string()))
        .collect();
        let mut cast = parse(&sample()).unwrap().cast; // Frank already has Daniel
        cast.push(CastMember {
            name: "Zed".into(),
            actor: "Zack".into(),
            voice: None,
        });
        assign_voices(&mut cast, &installed);
        assert_eq!(cast[0].voice.as_deref(), Some("Daniel"));
        assert_eq!(cast[1].voice.as_deref(), Some("Fred")); // preferred, and not Daniel again
        assert_eq!(cast[2].voice.as_deref(), Some("Zoe")); // then other English voices
        let mut lonely = vec![CastMember {
            name: "A".into(),
            actor: "Frank".into(),
            voice: None,
        }];
        assign_voices(&mut lonely, &[]);
        assert_eq!(lonely[0].voice, None);
    }

    #[test]
    fn titles_stay_up_long_enough_to_read() {
        assert_eq!(title_seconds("THE END"), 2.0);
        assert_eq!(
            title_seconds("one two three four five six seven eight nine"),
            4.0
        );
    }
}
