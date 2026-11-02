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
}

#[derive(Debug, Clone, PartialEq)]
pub struct Script {
    pub background: String,
    pub camera: Option<i64>,
    pub title: Option<String>,
    pub layout: String,
    pub camera_style: String,
    pub transition: Option<String>,
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
        let kinds = ["says", "does", "wait"]
            .iter()
            .filter(|k| line.get(**k).is_some())
            .count();
        if kinds != 1 {
            return Err(format!(
                "{at}: give exactly one of 'says' (dialogue), 'does' (an action) or 'wait' (seconds)"
            ));
        }
        if line.get("wait").is_some() {
            lines.push(Line::Wait {
                seconds: seconds(line, "wait", &at)?,
            });
            continue;
        }
        let who = string(line, "who").ok_or(format!("{at}: 'who' is required"))?;
        let Some(member) = cast.iter().find(|c| c.name.eq_ignore_ascii_case(&who)) else {
            return Err(format!("{at}: '{who}' is not in the cast ({})", names()));
        };
        let who = member.name.clone();
        if let Some(text) = string(line, "says") {
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
    if !lines.iter().any(|l| !matches!(l, Line::Wait { .. })) {
        return Err("'lines' needs at least one line that someone says or does".into());
    }

    Ok(Script {
        background,
        camera: args.get("camera").and_then(Value::as_i64),
        title: string(args, "title"),
        layout,
        camera_style,
        transition: string(args, "transition"),
        cast,
        lines,
    })
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

/// How long a title stays up: long enough to read (a second plus three
/// words a second), at least two seconds
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
