//! Local text-to-speech for dialogue: macOS `say`, Windows SAPI via
//! PowerShell. Produces a 16-bit PCM WAV the app can import.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use anyhow::{bail, Context, Result};
use tokio::process::Command;

static NEXT_LINE: AtomicU32 = AtomicU32::new(1);

/// Installed voices as (name, locale like "en_US"): macOS `say`, Windows
/// SAPI. Empty where they can't be listed.
pub async fn installed_voices() -> Vec<(String, String)> {
    if cfg!(target_os = "macos") {
        return match Command::new("say").arg("-v").arg("?").output().await {
            Ok(output) if output.status.success() => {
                crate::script::parse_say_voices(&String::from_utf8_lossy(&output.stdout))
            }
            _ => Vec::new(),
        };
    }
    if cfg!(target_os = "windows") {
        let script = "Add-Type -AssemblyName System.Speech; \
            $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; \
            $s.GetInstalledVoices() | Where-Object { $_.Enabled } | \
            ForEach-Object { $_.VoiceInfo.Name + '|' + $_.VoiceInfo.Culture.Name }; $s.Dispose()";
        return match Command::new("powershell")
            .args(["-NoProfile", "-NonInteractive", "-Command", script])
            .output()
            .await
        {
            Ok(output) if output.status.success() => {
                crate::script::parse_sapi_voices(&String::from_utf8_lossy(&output.stdout))
            }
            _ => Vec::new(),
        };
    }
    Vec::new()
}

/// Synthesizes `text` to a new WAV file and returns its path.
pub async fn synthesize(text: &str, voice: Option<&str>) -> Result<PathBuf> {
    if text.trim().is_empty() {
        bail!("'text' must not be empty");
    }
    let dir = std::env::temp_dir().join("3dmm-mcp");
    std::fs::create_dir_all(&dir)?;
    let path = dir.join(format!(
        "line-{}-{}.wav",
        std::process::id(),
        NEXT_LINE.fetch_add(1, Ordering::Relaxed)
    ));

    let status = if cfg!(target_os = "macos") {
        let mut command = Command::new("say");
        command
            .arg("-o")
            .arg(&path)
            .arg("--data-format=LEI16@22050");
        if let Some(voice) = voice {
            command.arg("-v").arg(voice);
        }
        // "--" so lines starting with '-' aren't taken as options
        command
            .arg("--")
            .arg(text)
            .status()
            .await
            .context("running `say`")?
    } else if cfg!(target_os = "windows") {
        // Text and path are passed through the environment, never spliced
        // into the script
        let script = "Add-Type -AssemblyName System.Speech; \
            $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; \
            if ($env:TDMM_VOICE) { $s.SelectVoice($env:TDMM_VOICE) }; \
            $s.SetOutputToWaveFile($env:TDMM_WAV); $s.Speak($env:TDMM_TEXT); $s.Dispose()";
        let mut command = Command::new("powershell");
        command
            .args(["-NoProfile", "-NonInteractive", "-Command", script])
            .env("TDMM_WAV", &path)
            .env("TDMM_TEXT", text);
        if let Some(voice) = voice {
            command.env("TDMM_VOICE", voice);
        }
        command
            .status()
            .await
            .context("running PowerShell speech synthesis")?
    } else {
        bail!("speak_line needs macOS (`say`) or Windows (SAPI); import a WAV with import_sound instead");
    };
    if !status.success() {
        bail!(
            "speech synthesis failed{}",
            voice
                .map(|v| format!(" (is '{v}' an installed voice?)"))
                .unwrap_or_default()
        );
    }
    Ok(path)
}

/// Duration of a PCM WAV file in seconds.
pub fn wav_seconds(path: &Path) -> Result<f64> {
    let bytes = std::fs::read(path)?;
    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        bail!("{} is not a WAV file", path.display());
    }
    let mut byte_rate = 0u32;
    let mut offset = 12;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes(bytes[offset + 4..offset + 8].try_into()?) as usize;
        let body = offset + 8;
        if id == b"fmt " && body + 12 <= bytes.len() {
            byte_rate = u32::from_le_bytes(bytes[body + 8..body + 12].try_into()?);
        } else if id == b"data" {
            if byte_rate == 0 {
                bail!("WAV data before format chunk");
            }
            let size = size.min(bytes.len() - body);
            return Ok(size as f64 / byte_rate as f64);
        }
        offset = body + size + (size & 1);
    }
    bail!("{} has no audio data", path.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_wav_duration() {
        let rate = 22050u32;
        let data = vec![0u8; (rate * 2 * 3 / 2) as usize]; // 1.5 s mono 16-bit
        let mut wav = Vec::new();
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16u32.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&rate.to_le_bytes());
        wav.extend_from_slice(&(rate * 2).to_le_bytes());
        wav.extend_from_slice(&2u16.to_le_bytes());
        wav.extend_from_slice(&16u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&(data.len() as u32).to_le_bytes());
        wav.extend_from_slice(&data);
        let path = std::env::temp_dir().join("3dmm-mcp-duration-test.wav");
        std::fs::write(&path, wav).unwrap();
        let seconds = wav_seconds(&path).unwrap();
        assert!((seconds - 1.5).abs() < 1e-9, "{seconds}");
    }
}
