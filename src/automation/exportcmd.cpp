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
};

// Writes the sound behind a tag to dir (once per tag) and describes it.
static SoundFile _SoundFileFor(PMVIE pmvie, TAG tag, const std::string &strDir,
                               std::map<std::string, SoundFile> *pmapCache)
{
    std::string strKey = std::to_string(tag.sid) + "-" + std::to_string(static_cast<uint32_t>(tag.cno));
    auto it = pmapCache->find(strKey);
    if (it != pmapCache->end())
        return it->second;

    SoundFile sf;
    if (tag.sid == ksidUseCrf && tag.pcrf == pvNil)
        pmvie->FEnsureAutosave(&tag.pcrf);
    if (tag.sid != ksidUseCrf)
        vptagm->FCacheTagToHD(&tag);
    PMSND pmsnd = (PMSND)vptagm->PbacoFetch(&tag, MSND::FReadMsnd);
    if (pmsnd == pvNil)
    {
        sf.note = "could not load sound";
        (*pmapCache)[strKey] = sf;
        return sf;
    }
    sf.name = StrFromStn(pmsnd->Pstn());

    CTG ctg;
    BLCK blck;
    std::string strError;
    if (pmsnd->FNoSound() == tribool::tYes)
    {
        sf.note = "silent (stops other sounds)";
    }
    else if (!pmsnd->FFindSndData(&ctg, &blck))
    {
        sf.note = "sound data not found";
    }
    else if (ctg == kctgMidi)
    {
#ifdef KAUAI_MIDI_FLUIDSYNTH
        std::string strPath = _StrJoinPath(strDir, "sound-" + strKey + ".wav");
        if (_FRenderMidi(&blck, strPath, &strError))
            sf.path = strPath;
        else
            sf.note = strError;
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
            bool fMp3 = rgb.size() > 3 && !(rgb[0] == 'R' && rgb[1] == 'I' && rgb[2] == 'F' && rgb[3] == 'F');
            std::string strPath = _StrJoinPath(strDir, "sound-" + strKey + (fMp3 ? ".mp3" : ".wav"));
            if (FWriteFileBytes(strPath, rgb, &strError))
                sf.path = strPath;
            else
                sf.note = strError;
        }
        else
        {
            sf.note = "could not read sound data";
        }
    }
    ReleasePpo(&pmsnd);
    (*pmapCache)[strKey] = sf;
    return sf;
}

struct Cue
{
    int32_t iscen;
    int32_t ifrm; // 0-based frame within the scene
    int32_t sty;
    std::string target; // "scene" or actor id
    TAG tag;
    int32_t vlm;
    bool fLoop;
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

static void _CollectCues(std::vector<Cue> *pvecCues, int32_t iscen, int32_t ifrm, const std::string &strTarget,
                         int32_t sty, PGL pgltag, int32_t vlm, bool fLoop)
{
    for (int32_t itag = 0; pgltag != pvNil && itag < pgltag->IvMac(); itag++)
    {
        Cue cue;
        cue.iscen = iscen;
        cue.ifrm = ifrm;
        cue.sty = sty;
        cue.target = strTarget;
        pgltag->Get(itag, &cue.tag);
        cue.vlm = vlm;
        cue.fLoop = fLoop;
        pvecCues->push_back(cue);
    }
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
    std::vector<Cue> vecCues;

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

        for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
        {
            int32_t ifrm = nfrm - nfrmFirst;
            if (!pscen->FGotoFrm(nfrm))
            {
                FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
                break;
            }
            snprintf(rgch, sizeof(rgch), "s%02d_%05d.png", iscen, ifrm);
            if (!FRenderToPng(req, _StrJoinPath(strDir, rgch), &dxp, &dyp, fFalse))
                break;
            if (!fAudio)
                continue;

            for (int32_t sty = stySfx; sty < styLim; sty++)
            {
                PGL pgltag = pvNil;
                int32_t vlm;
                bool fLoop;
                if (pscen->FQuerySnd(sty, &pgltag, &vlm, &fLoop))
                    _CollectCues(&vecCues, iscen, ifrm, "scene", sty, pgltag, vlm, fLoop);
                ReleasePpo(&pgltag);

                PGL pglpactr = pscen->PglRollCall();
                for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
                {
                    PACTR pactr;
                    pglpactr->Get(ipactr, &pactr);
                    if (!pactr->FSoundInFrm())
                        continue;
                    if (pactr->FQuerySnd(sty, fFalse, &pgltag, &vlm, &fLoop))
                        _CollectCues(&vecCues, iscen, ifrm, std::to_string(pactr->Arid()), sty, pgltag, vlm, fLoop);
                    ReleasePpo(&pgltag);
                }
            }
        }

        Json jsonScene = Json::MakeObject();
        jsonScene.Set("index", iscen);
        jsonScene.Set("frames", nfrmLast - nfrmFirst + 1);
        jsonScene.Set("pattern", strPattern);
        jsonScene.Set("transition_after", _PszTrans(pscen->Trans()));
        jsonScenes.Push(jsonScene);
        cfrmTotal += nfrmLast - nfrmFirst + 1;
    }

    // Write each distinct sound once and describe the cues. A cue lasts
    // until the next cue on the same target and sound type (3D Movie Maker
    // replaces a target's sound of a type with the next one) or the end of
    // its scene.
    Json jsonCues = Json::MakeArray();
    Json jsonNotes = Json::MakeArray();
    std::map<std::string, SoundFile> mapCache;
    for (size_t icue = 0; icue < vecCues.size() && !req.Failed(); icue++)
    {
        const Cue &cue = vecCues[icue];
        int32_t ifrmUntil = -1;
        for (size_t jcue = icue + 1; jcue < vecCues.size(); jcue++)
        {
            const Cue &next = vecCues[jcue];
            if (next.iscen == cue.iscen && next.target == cue.target && next.sty == cue.sty && next.ifrm > cue.ifrm)
            {
                ifrmUntil = next.ifrm;
                break;
            }
        }
        SoundFile sf = _SoundFileFor(pmvie, cue.tag, strDir, &mapCache);
        if (sf.path.empty())
        {
            if (!sf.note.empty() && sf.note.find("silent") == std::string::npos)
                jsonNotes.Push(sf.name + ": " + sf.note);
            continue;
        }
        Json json = Json::MakeObject();
        json.Set("scene", cue.iscen);
        json.Set("frame", cue.ifrm); // 0-based within the scene
        if (ifrmUntil >= 0)
            json.Set("until_frame", ifrmUntil);
        json.Set("file", sf.path);
        json.Set("name", sf.name);
        json.Set("type", _PszSty(cue.sty));
        json.Set("target", cue.target);
        json.Set("loop", cue.fLoop);
        json.Set("volume", cue.vlm > 0 ? static_cast<double>(cue.vlm) / kvlmFull : 1.0);
        jsonCues.Push(json);
    }

    if (iscenRestore != ivNil && iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    if (nfrmRestore != ivNil && pmvie->Pscen() != pvNil)
        pmvie->Pscen()->FGotoFrm(nfrmRestore);
    RefreshViews(pmvie);
    if (req.Failed())
        return fFalse;

    req.result.Set("frames_per_second", kfps);
    req.result.Set("width", dxp);
    req.result.Set("height", dyp);
    req.result.Set("total_frames", cfrmTotal);
    req.result.Set("scenes", jsonScenes);
    req.result.Set("cues", jsonCues);
    if (jsonNotes.Size() > 0)
        req.result.Set("notes", jsonNotes);
    return fTrue;
}

const CommandInfo krgcmdExport[] = {
    {"export_media", _FCmdExportMedia, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
