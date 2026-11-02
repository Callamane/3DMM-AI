//! Local text-to-speech for dialogue: macOS `say`, Windows SAPI via
//! PowerShell. Produces a 16-bit PCM WAV the app can import.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use anyhow::{bail, Context, Result};
use tokio::process::Command;

use crate::script::Prosody;

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

/// Synthesizes `text` to a new WAV file and returns its path. `prosody`
/// changes the delivery (rate and volume by the synthesizer, pitch after,
/// with ffmpeg when it is installed).
pub async fn synthesize(
    text: &str,
    voice: Option<&str>,
    prosody: Option<&Prosody>,
) -> Result<PathBuf> {
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
        // Embedded speech commands set rate and volume; the line itself can't
        // contain any ("[[" is broken up)
        let mut spoken = text.replace("[[", "[ [");
        if let Some(p) = prosody {
            let mut commands = String::new();
            if let Some(rate) = p.rate {
                commands.push_str(&format!("[[rate {rate}]] "));
            }
            if let Some(volume) = p.volume {
                commands.push_str(&format!("[[volm {volume:.2}]] "));
            }
            spoken = commands + &spoken;
        }
        // "--" so lines starting with '-' aren't taken as options
        command
            .arg("--")
            .arg(spoken)
            .status()
            .await
            .context("running `say`")?
    } else if cfg!(target_os = "windows") {
        // Text and path are passed through the environment, never spliced
        // into the script
        // Stop on errors: a bad voice name must fail, not fall back silently
        let script = "$ErrorActionPreference = 'Stop'; Add-Type -AssemblyName System.Speech; \
            $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; \
            if ($env:TDMM_VOICE) { $s.SelectVoice($env:TDMM_VOICE) }; \
            if ($env:TDMM_RATE) { $s.Rate = [int]$env:TDMM_RATE }; \
            if ($env:TDMM_VOLUME) { $s.Volume = [int]$env:TDMM_VOLUME }; \
            $s.SetOutputToWaveFile($env:TDMM_WAV); $s.Speak($env:TDMM_TEXT); $s.Dispose()";
        let mut command = Command::new("powershell");
        command
            .args(["-NoProfile", "-NonInteractive", "-Command", script])
            .env("TDMM_WAV", &path)
            .env("TDMM_TEXT", text);
        if let Some(voice) = voice {
            command.env("TDMM_VOICE", voice);
        }
        if let Some(p) = prosody {
            // SAPI's rate runs -10..10 around its normal ~180 words a minute
            if let Some(rate) = p.rate {
                command.env(
                    "TDMM_RATE",
                    ((rate as i64 - 180) / 10).clamp(-10, 10).to_string(),
                );
            }
            if let Some(volume) = p.volume {
                command.env(
                    "TDMM_VOLUME",
                    ((volume * 100.0).round() as i64).clamp(0, 100).to_string(),
                );
            }
        }
        command
            .status()
            .await
            .context("running PowerShell speech synthesis")?
    } else {
        bail!("speak_line needs macOS (`say`) or Windows (SAPI); import a WAV with import_sound instead");
    };
    if !status.success() {
        let _ = std::fs::remove_file(&path);
        bail!(
            "speech synthesis failed{}",
            voice
                .map(|v| format!(" (is '{v}' an installed voice?)"))
                .unwrap_or_default()
        );
    }
    if let Some(p) = prosody {
        if (p.pitch - 1.0).abs() > 1e-3 {
            shift_pitch(&path, p.pitch).await;
        }
    }
    Ok(path)
}

/// Raises or lowers a WAV's pitch by `ratio` without changing its length,
/// in place. Skipped (the line keeps its natural pitch) if ffmpeg isn't
/// installed or fails: pitch is a nicety, not a requirement.
async fn shift_pitch(path: &Path, ratio: f64) {
    let Ok(ffmpeg) = crate::video::find_ffmpeg() else {
        return;
    };
    // The synthesizer's own sample rate (SAPI's isn't fixed)
    let Ok((rate, _)) = wav_info(path) else {
        return;
    };
    let shifted = path.with_extension("pitched.wav");
    let filter = format!(
        "asetrate={rate}*{ratio:.4},aresample={rate},atempo={:.4}",
        1.0 / ratio
    );
    let ok = Command::new(ffmpeg)
        .args(["-v", "error", "-y", "-i"])
        .arg(path)
        .args(["-af", &filter, "-ac", "1", "-c:a", "pcm_s16le"])
        .arg(&shifted)
        .status()
        .await
        .is_ok_and(|status| status.success());
    if ok {
        let _ = std::fs::rename(&shifted, path);
    } else {
        let _ = std::fs::remove_file(&shifted);
    }
}

/// Duration of a PCM WAV file in seconds.
pub fn wav_seconds(path: &Path) -> Result<f64> {
    Ok(wav_info(path)?.1)
}

/// A PCM WAV file's sample rate and duration in seconds.
fn wav_info(path: &Path) -> Result<(u32, f64)> {
    let bytes = std::fs::read(path)?;
    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        bail!("{} is not a WAV file", path.display());
    }
    let (mut sample_rate, mut byte_rate) = (0u32, 0u32);
    let mut offset = 12;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes(bytes[offset + 4..offset + 8].try_into()?) as usize;
        let body = offset + 8;
        if id == b"fmt " && body + 12 <= bytes.len() {
            sample_rate = u32::from_le_bytes(bytes[body + 4..body + 8].try_into()?);
            byte_rate = u32::from_le_bytes(bytes[body + 8..body + 12].try_into()?);
        } else if id == b"data" {
            if byte_rate == 0 {
                bail!("WAV data before format chunk");
            }
            let size = size.min(bytes.len() - body);
            return Ok((sample_rate, size as f64 / byte_rate as f64));
        }
        offset = body + size + (size & 1);
    }
    bail!("{} has no audio data", path.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A silent mono 16-bit WAV of `seconds` at `rate`
    fn write_wav(name: &str, rate: u32, seconds: f64) -> PathBuf {
        let data = vec![0u8; (rate as f64 * 2.0 * seconds) as usize];
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
        let path = std::env::temp_dir().join(name);
        std::fs::write(&path, wav).unwrap();
        path
    }

    #[test]
    fn reads_wav_duration() {
        let path = write_wav("3dmm-mcp-duration-test.wav", 22050, 1.5);
        let seconds = wav_seconds(&path).unwrap();
        assert!((seconds - 1.5).abs() < 1e-9, "{seconds}");
    }

    #[tokio::test]
    async fn pitch_shift_keeps_the_sample_rate_and_length() {
        if crate::video::find_ffmpeg().is_err() {
            return; // pitch is skipped without ffmpeg
        }
        // SAPI may write 16 kHz; a shift assuming 22050 Hz would resample it
        let path = write_wav("3dmm-mcp-pitch-test.wav", 16000, 2.0);
        shift_pitch(&path, 1.25).await;
        let (rate, seconds) = wav_info(&path).unwrap();
        std::fs::remove_file(&path).ok();
        assert_eq!(rate, 16000);
        assert!((seconds - 2.0).abs() < 0.05, "{seconds}");
    }
}
