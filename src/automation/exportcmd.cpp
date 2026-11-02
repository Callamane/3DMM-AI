/***************************************************************************
    Automation bridge: exporting a movie as media (frames + audio cues), the
    raw material for building a video file (the MCP server's export_video
    feeds it to ffmpeg).

    Frames are rendered exactly like render_frame. Audio is reconstructed
    from the scene and actor sound events at each frame: WAV/MP3 data is
    written out as stored in the movie or content files; MIDI music is
    rendered to WAV with FluidSynth and the soundfont the app ships with.
***************************************************************************/
#include "bridge.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#ifdef KAUAI_MIDI_FLUIDSYNTH
#include <fluidsynth.h>
#endif

#include "pngwrite.h"

ASSERTNAME

namespace automation
{

const int32_t kcfrmExportMax = 6 * 60 * 30; // half an hour
const int32_t klwSampleRate = 44100;
const uint32_t kdtsMidiMax = 10 * 60 * 1000; // ten minutes of music
const uint32_t kdtsMidiTail = 2000;          // let notes ring out

static std::string _StrJoinPath(const std::string &strDir, const std::string &strLeaf)
{
    if (!strDir.empty() && (strDir.back() == '/' || strDir.back() == '\\'))
        return strDir + strLeaf;
    return strDir + "/" + strLeaf;
}

static bool _FWriteWav16Stereo(const std::string &strPath, const std::vector<int16_t> &vecSamples,
                               std::string *pstrError)
{
    std::vector<uint8_t> rgb;
    auto put32 = [&](uint32_t lu) {
        for (int i = 0; i < 4; i++)
            rgb.push_back(static_cast<uint8_t>(lu >> (8 * i)));
    };
    auto put16 = [&](uint16_t su) {
        rgb.push_back(static_cast<uint8_t>(su));
        rgb.push_back(static_cast<uint8_t>(su >> 8));
    };
    uint32_t cbData = static_cast<uint32_t>(vecSamples.size() * 2);
    rgb.insert(rgb.end(), {'R', 'I', 'F', 'F'});
    put32(36 + cbData);
    rgb.insert(rgb.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(16);
    put16(1); // PCM
    put16(2); // stereo
    put32(klwSampleRate);
    put32(klwSampleRate * 4);
    put16(4);
    put16(16);
    rgb.insert(rgb.end(), {'d', 'a', 't', 'a'});
    put32(cbData);
    for (int16_t sw : vecSamples)
        put16(static_cast<uint16_t>(sw));
    return FWriteFileBytes(strPath, rgb, pstrError);
}

#ifdef KAUAI_MIDI_FLUIDSYNTH
// Renders a Kauai MIDI stream to a 16-bit stereo WAV, offline.
static bool _FRenderMidi(PBLCK pblck, const std::string &strPath, std::string *pstrError)
{
    PMIDS pmids = MIDS::PmidsRead(pblck);
    if (pmids == pvNil)
    {
        *pstrError = "could not read the MIDI data";
        return fFalse;
    }

    fluid_settings_t *psettings = new_fluid_settings();
    fluid_settings_setnum(psettings, "synth.sample-rate", klwSampleRate);
    fluid_synth_t *psynth = new_fluid_synth(psettings);

    // The soundfont the app ships next to its resources
    FNI fni;
    STN stn = PszLit("soundfont.sf3");
    bool fSoundfont = fni.FGetResourcesDir() && fni.FSetLeaf(&stn);
    if (fSoundfont)
    {
        fni.GetStnPath(&stn);
        fSoundfont = fluid_synth_sfload(psynth, StrFromStn(&stn).c_str(), 1) != FLUID_FAILED;
    }
    if (!fSoundfont)
    {
        *pstrError = "could not load soundfont.sf3 for rendering music";
        delete_fluid_synth(psynth);
        delete_fluid_settings(psettings);
        ReleasePpo(&pmids);
        return fFalse;
    }

    std::vector<int16_t> vecSamples;
    uint32_t cframeDone = 0;
    auto renderTo = [&](uint32_t ts) {
        uint32_t cframeTarget = static_cast<uint32_t>(static_cast<uint64_t>(ts) * klwSampleRate / 1000);
        while (cframeDone < cframeTarget)
        {
            int16_t rgsw[2 * 1024];
            int cframe = static_cast<int>(LwMin(1024, static_cast<int32_t>(cframeTarget - cframeDone)));
            fluid_synth_write_s16(psynth, cframe, rgsw, 0, 2, rgsw, 1, 2);
            vecSamples.insert(vecSamples.end(), rgsw, rgsw + 2 * cframe);
            cframeDone += cframe;
        }
    };

    MSTP mstp;
    MIDEV midev;
    uint32_t tsLast = 0;
    mstp.Init(pmids, 0, 500000);
    while (mstp.FGetEvent(&midev) && midev.ts < kdtsMidiMax)
    {
        renderTo(midev.ts);
        tsLast = midev.ts;
        if (midev.cb <= 0)
            continue; // tempo change
        uint8_t bStatus = midev.rgbSend[0];
        int chan = bStatus & 0x0F;
        int b1 = midev.cb > 1 ? midev.rgbSend[1] : 0;
        int b2 = midev.cb > 2 ? midev.rgbSend[2] : 0;
        switch (bStatus & 0xF0)
        {
        case 0x80:
            fluid_synth_noteoff(psynth, chan, b1);
            break;
        case 0x90:
            if (b2 == 0)
                fluid_synth_noteoff(psynth, chan, b1);
            else
                fluid_synth_noteon(psynth, chan, b1, b2);
            break;
        case 0xA0:
            fluid_synth_key_pressure(psynth, chan, b1, b2);
            break;
        case 0xB0:
            fluid_synth_cc(psynth, chan, b1, b2);
            break;
        case 0xC0:
            fluid_synth_program_change(psynth, chan, b1);
            break;
        case 0xD0:
            fluid_synth_channel_pressure(psynth, chan, b1);
            break;
        case 0xE0:
            fluid_synth_pitch_bend(psynth, chan, b1 | (b2 << 7));
            break;
        default:
            break;
        }
    }
    renderTo(tsLast + kdtsMidiTail);

    delete_fluid_synth(psynth);
    delete_fluid_settings(psettings);
    ReleasePpo(&pmids);
    return _FWriteWav16Stereo(strPath, vecSamples, pstrError);
}
#endif // KAUAI_MIDI_FLUIDSYNTH

struct SoundFile
{
    std::string path; // empty for silent sounds ("sound stopper") or failures
    std::string name;
    std::string note;
    int32_t dtsLength = -1; // milliseconds, -1 if unknown
};

// Length in milliseconds of a PCM WAV file's audio, or -1.
static int32_t _DtsWav(const std::vector<uint8_t> &rgb)
{
    if (rgb.size() < 12 || memcmp(rgb.data(), "RIFF", 4) != 0 || memcmp(rgb.data() + 8, "WAVE", 4) != 0)
        return -1;
    uint32_t cbPerSec = 0;
    size_t ib = 12;
    auto lu = [&](size_t i) {
        return static_cast<uint32_t>(rgb[i] | (rgb[i + 1] << 8) | (rgb[i + 2] << 16) | (rgb[i + 3] << 24));
    };
    while (ib + 8 <= rgb.size())
    {
        uint32_t cb = lu(ib + 4);
        if (memcmp(rgb.data() + ib, "fmt ", 4) == 0 && ib + 20 <= rgb.size())
            cbPerSec = lu(ib + 16);
        else if (memcmp(rgb.data() + ib, "data", 4) == 0 && cbPerSec > 0)
            return static_cast<int32_t>(static_cast<uint64_t>(LwMin(cb, static_cast<uint32_t>(rgb.size() - ib - 8))) *
                                        1000 / cbPerSec);
        ib += 8 + cb + (cb & 1);
    }
    return -1;
}

// Writes the sound's data to dir (once per data chunk) and describes it.
static SoundFile _SoundFileFor(PMSND pmsnd, const std::string &strDir, std::map<std::string, SoundFile> *pmapCache)
{
    SoundFile sf;
    CTG ctg;
    BLCK blck;
    PCRF pcrf = pvNil;
    CNO cno = cnoNil;
    std::string strError;

    sf.name = StrFromStn(pmsnd->Pstn());
    if (pmsnd->FNoSound() == tribool::tYes)
    {
        sf.note = "silent (stops other sounds)";
        return sf;
    }
    if (!pmsnd->FFindSndData(&ctg, &blck, &pcrf, &cno))
    {
        sf.note = "sound data not found";
        return sf;
    }

    char rgch[64];
    snprintf(rgch, sizeof(rgch), "%p-%u", static_cast<void *>(pcrf), static_cast<uint32_t>(cno));
    std::string strKey = rgch;
    auto it = pmapCache->find(strKey);
    if (it != pmapCache->end())
        return it->second;

    std::string strLeaf = "sound-" + std::to_string(pmapCache->size());
    if (ctg == kctgMidi)
    {
#ifdef KAUAI_MIDI_FLUIDSYNTH
        std::string strPath = _StrJoinPath(strDir, strLeaf + ".wav");
        if (_FRenderMidi(&blck, strPath, &strError))
        {
            sf.path = strPath;
            // Rendered at 44.1 kHz 16-bit stereo: 176400 bytes per second
            std::error_code ec;
            uintmax_t cb = std::filesystem::file_size(strPath, ec);
            if (!ec && cb > 44)
                sf.dtsLength = static_cast<int32_t>((cb - 44) * 1000 / 176400);
        }
        else
        {
            sf.note = strError;
        }
#else
        sf.note = "this build has no MIDI synthesizer, so music can't be exported";
#endif
    }
    else
    {
        // WAV or MP3 file bytes, stored as imported
        std::vector<uint8_t> rgb(blck.Cb());
        if (!rgb.empty() && blck.FReadRgb(rgb.data(), blck.Cb(), 0))
        {
            bool fWav = rgb.size() > 4 && memcmp(rgb.data(), "RIFF", 4) == 0;
            std::string strPath = _StrJoinPath(strDir, strLeaf + (fWav ? ".wav" : ".mp3"));
            if (FWriteFileBytes(strPath, rgb, &strError))
            {
                sf.path = strPath;
                if (fWav)
                    sf.dtsLength = _DtsWav(rgb);
            }
            else
            {
                sf.note = strError;
            }
        }
        else
        {
            sf.note = "could not read sound data";
        }
    }
    (*pmapCache)[strKey] = sf;
    return sf;
}

// One sound as playback would start it: taken from the movie's sound queue
// (MSQ) after the engine processed a frame, so it includes motion-matched
// sounds and follows the engine's rules.
struct Cue
{
    int32_t ifrm; // 0-based frame within the scene
    std::string channel;
    std::string target; // "scene" or actor id
    int32_t sty;
    bool fLoop;
    bool fQueue;
    int32_t vlm;
    uint32_t dtsOffset; // start this far into the sound
    SoundFile sf;
};

static const char *_PszSty(int32_t sty)
{
    return sty == styMidi ? "music" : (sty == stySpeech ? "speech" : "sfx");
}

static const char *_PszTrans(TRANS trans)
{
    switch (trans)
    {
    case transFadeToBlack:
        return "fade_to_black";
    case transFadeToWhite:
        return "fade_to_white";
    case transDissolve:
        return "dissolve";
    case transBlack:
        return "black";
    default:
        return "cut";
    }
}

// Moves the movie's queued sounds into vecCues and empties the queue
// without playing anything.
static void _DrainQueue(PMSQ pmsq, int32_t ifrm, const std::string &strDir, std::map<std::string, SoundFile> *pmapCache,
                        std::vector<Cue> *pvecCues)
{
    for (int32_t isqe = 0; isqe < pmsq->Csqe(); isqe++)
    {
        SQE sqe;
        pmsq->GetSqe(isqe, &sqe);
        Cue cue;
        cue.ifrm = ifrm;
        cue.sty = sqe.pmsnd->Sty();
        cue.target = sqe.fActr ? std::to_string(sqe.objID) : std::string("scene");
        // The engine's sound channel: one per (actor or scene, sound type)
        cue.channel = cue.target + "/" + std::to_string(cue.sty);
        cue.fLoop = sqe.fLoop;
        cue.fQueue = sqe.fQueue;
        cue.vlm = sqe.vlmMod;
        cue.dtsOffset = sqe.dtsStart;
        cue.sf = _SoundFileFor(sqe.pmsnd, strDir, pmapCache);
        pvecCues->push_back(cue);
    }
    pmsq->FlushMsq();
}

// Turns one scene's cues into timed clips (milliseconds from the scene's
// start): a new sound on a channel cuts off the one playing unless it was
// queued, in which case it starts when the current one ends.
static void _ScheduleScene(int32_t iscen, int32_t cfrm, const std::vector<Cue> &vecCues, Json *pjsonClips,
                           Json *pjsonNotes)
{
    const int32_t dtsScene = cfrm * 1000 / kfps;
    struct Active
    {
        int32_t iclip;
        int32_t tsEnd; // -1 = until replaced
    };
    std::map<std::string, Active> mapActive;
    std::vector<Json> vecClips;

    for (const Cue &cue : vecCues)
    {
        if (cue.sf.path.empty())
        {
            std::string strNote = cue.sf.name + ": " + cue.sf.note;
            bool fSeen = fFalse;
            for (const Json &json : pjsonNotes->AsArray())
                fSeen |= json.AsString() == strNote;
            if (!cue.sf.note.empty() && cue.sf.note.find("silent") == std::string::npos && !fSeen)
                pjsonNotes->Push(strNote);
        }
        int32_t tsStart = cue.ifrm * 1000 / kfps;
        auto it = mapActive.find(cue.channel);
        if (it != mapActive.end())
        {
            Active &active = it->second;
            bool fStillPlaying = active.tsEnd < 0 || active.tsEnd > tsStart;
            if (cue.fQueue && fStillPlaying && active.tsEnd >= 0)
            {
                tsStart = active.tsEnd; // waits its turn
            }
            else if (fStillPlaying && active.iclip >= 0)
            {
                vecClips[active.iclip].Set("end_ms", tsStart);
            }
        }
        if (cue.sf.path.empty())
        {
            // A silent sound still stops the channel
            mapActive[cue.channel] = {-1, tsStart};
            continue;
        }
        if (tsStart >= dtsScene)
            continue;
        int32_t tsEnd = -1;
        if (!cue.fLoop && cue.sf.dtsLength >= 0)
            tsEnd = tsStart + LwMax(0, cue.sf.dtsLength - static_cast<int32_t>(cue.dtsOffset));

        Json json = Json::MakeObject();
        json.Set("scene", iscen);
        json.Set("start_ms", tsStart);
        json.Set("end_ms", tsEnd < 0 || tsEnd > dtsScene ? dtsScene : tsEnd);
        json.Set("offset_ms", static_cast<int32_t>(cue.dtsOffset));
        json.Set("file", cue.sf.path);
        json.Set("name", cue.sf.name);
        json.Set("type", _PszSty(cue.sty));
        json.Set("target", cue.target);
        json.Set("loop", cue.fLoop);
        json.Set("volume", cue.vlm > 0 ? static_cast<double>(cue.vlm) / kvlmFull : 1.0);
        mapActive[cue.channel] = {static_cast<int32_t>(vecClips.size()), tsEnd};
        vecClips.push_back(json);
    }
    for (Json &json : vecClips)
        pjsonClips->Push(json);
}

static bool _FCmdExportMedia(Request &req)
{
    PMVIE pmvie;
    std::string strDir;
    bool fAudio = fTrue;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("dir", &strDir) || !req.FArgBool("audio", &fAudio, fFalse))
        return fFalse;
    if (pmvie->Cscen() < 1)
        return req.Fail(kszErrBadState, "the movie has no scenes");
    if (strDir.compare(0, 2, "~/") == 0 && getenv("HOME") != nullptr)
        strDir = std::string(getenv("HOME")) + strDir.substr(1);

    int32_t iscenFirst = 0, iscenLast = pmvie->Cscen() - 1;
    if (req.FHasArg("scene"))
    {
        if (!req.FArgInt("scene", &iscenFirst))
            return fFalse;
        if (iscenFirst < 0 || iscenFirst >= pmvie->Cscen())
            return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscenFirst) + " does not exist");
        iscenLast = iscenFirst;
    }

    int32_t iscenRestore = pmvie->Iscen();
    int32_t nfrmRestore = pmvie->Pscen() != pvNil ? pmvie->Pscen()->Nfrm() : ivNil;
    int32_t cfrmTotal = 0;
    int32_t dxp = 0, dyp = 0;
    Json jsonScenes = Json::MakeArray();
    Json jsonClips = Json::MakeArray();
    Json jsonNotes = Json::MakeArray();
    std::map<std::string, SoundFile> mapCache;
    PMSQ pmsq = pmvie->Pmsq();

    // Queue sounds without playing them: enqueueing is skipped while the
    // queue is "off", and anything left over is dropped
    pmsq->SndOnShort();
    pmsq->FlushMsq();

    for (int32_t iscen = iscenFirst; iscen <= iscenLast && !req.Failed(); iscen++)
    {
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
        {
            FFailEngine(req, "could not switch to scene " + std::to_string(iscen));
            break;
        }
        PSCEN pscen = pmvie->Pscen();
        int32_t nfrmFirst = pscen->NfrmFirst(), nfrmLast = pscen->NfrmLast();
        if (cfrmTotal + (nfrmLast - nfrmFirst + 1) > kcfrmExportMax)
        {
            req.Fail(kszErrBadArgs,
                     "movie too long to export in one go (limit " + std::to_string(kcfrmExportMax) + " frames)");
            break;
        }
        char rgch[40];
        snprintf(rgch, sizeof(rgch), "s%02d_%%05d.png", iscen);
        std::string strPattern = _StrJoinPath(strDir, rgch);
        std::vector<Cue> vecCues;

        // Start at the first frame and replay its sounds (moving forward to
        // a frame queues that frame's sounds; the first frame needs a replay)
        pmsq->FlushMsq();
        if (!pscen->FGotoFrm(nfrmFirst))
        {
            FFailEngine(req, "could not go to the first frame");
            break;
        }
        pmsq->FlushMsq();
        if (fAudio)
        {
            pscen->FReplayFrm(fscenSounds | fscenActrs);
            _DrainQueue(pmsq, 0, strDir, &mapCache, &vecCues);
        }

        for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
        {
            int32_t ifrm = nfrm - nfrmFirst;
            if (nfrm != nfrmFirst)
            {
                if (!pscen->FGotoFrm(nfrm))
                {
                    FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
                    break;
                }
                if (fAudio)
                    _DrainQueue(pmsq, ifrm, strDir, &mapCache, &vecCues);
                else
                    pmsq->FlushMsq();
            }
            snprintf(rgch, sizeof(rgch), "s%02d_%05d.png", iscen, ifrm);
            if (!FRenderToPng(req, _StrJoinPath(strDir, rgch), &dxp, &dyp, fFalse))
                break;
        }
        _ScheduleScene(iscen, nfrmLast - nfrmFirst + 1, vecCues, &jsonClips, &jsonNotes);

        Json jsonScene = Json::MakeObject();
        jsonScene.Set("index", iscen);
        jsonScene.Set("frames", nfrmLast - nfrmFirst + 1);
        jsonScene.Set("pattern", strPattern);
        jsonScene.Set("transition_after", _PszTrans(pscen->Trans()));
        jsonScenes.Push(jsonScene);
        cfrmTotal += nfrmLast - nfrmFirst + 1;
    }
    pmsq->FlushMsq();

    if (iscenRestore != ivNil && iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    if (nfrmRestore != ivNil && pmvie->Pscen() != pvNil)
        pmvie->Pscen()->FGotoFrm(nfrmRestore);
    pmsq->FlushMsq();
    RefreshViews(pmvie);
    if (req.Failed())
        return fFalse;

    req.result.Set("frames_per_second", kfps);
    req.result.Set("width", dxp);
    req.result.Set("height", dyp);
    req.result.Set("total_frames", cfrmTotal);
    req.result.Set("scenes", jsonScenes);
    req.result.Set("clips", jsonClips);
    if (jsonNotes.Size() > 0)
        req.result.Set("notes", jsonNotes);
    return fTrue;
}

const CommandInfo krgcmdExport[] = {
    {"export_media", _FCmdExportMedia, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
