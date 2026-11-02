/***************************************************************************
    Automation bridge: session commands (handshake, state, movie files,
    undo/redo).
***************************************************************************/
#include "bridge.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "buildgob.h"
#include "sharutil.chh"
#include "version.h"

ASSERTNAME

namespace automation
{

const uint32_t kdtsStudioTimeout = 60 * kdtsSecond;

// Where save_movie writes by default after restore_checkpoint (the restored
// movie is detached from its checkpoint file)
static std::string _strPathBeforeRestore;

// Builds an FNI from a user-supplied path. Expands a leading "~/".
bool FFniFromPath(Request &req, const std::string &strPathIn, FTG ftg, FNI *pfni)
{
    std::string strPath = strPathIn;
    STN stn;

    if (strPath.empty())
        return req.Fail(kszErrBadArgs, "path is empty");
    if (strPath.compare(0, 2, "~/") == 0)
    {
        const char *pszHome = getenv("HOME");
        if (pszHome == nullptr)
            pszHome = getenv("USERPROFILE");
        if (pszHome != nullptr)
            strPath = std::string(pszHome) + strPath.substr(1);
    }
    if (strPath.size() > kcchMaxStn)
        return req.Fail(kszErrBadArgs, "path is longer than " + std::to_string(kcchMaxStn) + " characters");

    StnFromStr(strPath, &stn);
    if (!pfni->FBuildFromPath(&stn, ftg))
        return req.Fail(kszErrBadArgs, "invalid path '" + strPathIn + "' (use an absolute path)");
    return fTrue;
}

static void _DiscardChanges(PMVIE pmvie)
{
    // MVIE tracks two dirty flags; clear both so closing doesn't prompt
    pmvie->SetDirty(fFalse);
    pmvie->DOCB::SetDirty(fFalse);
}

/***************************************************************************
    State
***************************************************************************/
Json JsonMovieState(PMVIE pmvie)
{
    Json json = Json::MakeObject();
    STN stn;
    FNI fni;

    pmvie->GetName(&stn);
    json.Set("title", StrFromStn(&stn));
    if (pmvie->FGetFni(&fni))
    {
        fni.GetStnPath(&stn);
        json.Set("path", StrFromStn(&stn));
    }
    else
    {
        json.Set("path", Json());
    }
    json.Set("dirty", pmvie->FDirty());
    json.Set("playing", pmvie->FPlaying());
    json.Set("scene_count", pmvie->Cscen());
    json.Set("scene_index", pmvie->Iscen() == ivNil ? Json() : Json(pmvie->Iscen()));

    PSCEN pscen = pmvie->Pscen();
    if (pscen != pvNil)
    {
        Json jsonScene = Json::MakeObject();
        jsonScene.Set("frame", pscen->Nfrm());
        jsonScene.Set("first_frame", pscen->NfrmFirst());
        jsonScene.Set("last_frame", pscen->NfrmLast());
        jsonScene.Set("frames_per_second", kfps);

        PBKGD pbkgd = pscen->Pbkgd();
        TAG tag;
        if (pbkgd != pvNil)
        {
            pbkgd->GetName(&stn);
            Json jsonBkgd = Json::MakeObject();
            if (pscen->FGetTagBkgd(&tag))
                jsonBkgd.Set("id",
                             "bkgd:" + std::to_string(tag.sid) + ":" + std::to_string(static_cast<uint32_t>(tag.cno)));
            jsonBkgd.Set("name", StrFromStn(&stn));
            jsonScene.Set("background", jsonBkgd);
            jsonScene.Set("camera", pbkgd->Icam());
            jsonScene.Set("camera_count", pbkgd->Ccam());
        }

        PACTR pactr = pscen->PactrSelected();
        if (pactr != pvNil)
        {
            Json jsonActr = Json::MakeObject();
            jsonActr.Set("id", pactr->Arid());
            if (pmvie->FGetName(pactr->Arid(), &stn))
                jsonActr.Set("name", StrFromStn(&stn));
            jsonScene.Set("selected_actor", jsonActr);
        }
        else
        {
            jsonScene.Set("selected_actor", Json());
        }
        jsonScene.Set("actor_count", pscen->Cactr());
        jsonScene.Set("text_box_count", pscen->Ctbox());
        json.Set("scene", jsonScene);
    }
    return json;
}

static bool _FCmdHello(Request &req)
{
    req.result.Set("protocol", kProtocolVersion);
    req.result.Set("app", "3DMMEx");
#ifdef GIT_VERSION
    req.result.Set("version", GIT_VERSION);
#endif
    req.result.Set("studio_open", PmvieCur() != pvNil);
    req.result.Set("frames_per_second", kfps);
    req.result.Set("units", "stage coordinates are metres: x = right, z = away from the camera, y = up; "
                            "origin is the background's default actor placement point");
    return fTrue;
}

// Frame range of every scene plus the movie's total length. Visits each
// scene (only the current one is in memory), then returns to where it was.
static bool _FAddSceneSummary(Request &req, PMVIE pmvie)
{
    int32_t iscenRestore = pmvie->Iscen();
    int32_t nfrmRestore = pmvie->Pscen() != pvNil ? pmvie->Pscen()->Nfrm() : ivNil;
    int32_t cfrmTotal = 0;
    Json jsonScenes = Json::MakeArray();
    STN stn;

    if (pmvie->FPlaying())
        return req.Fail(kszErrBadState, "the movie is playing; call stop first");
    for (int32_t iscen = 0; iscen < pmvie->Cscen(); iscen++)
    {
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
            return FFailEngine(req, "could not read scene " + std::to_string(iscen));
        PSCEN pscen = pmvie->Pscen();
        int32_t cfrm = pscen->NfrmLast() - pscen->NfrmFirst() + 1;
        Json json = Json::MakeObject();
        json.Set("index", iscen);
        pscen->Pbkgd()->GetName(&stn);
        json.Set("background", StrFromStn(&stn));
        json.Set("first_frame", pscen->NfrmFirst());
        json.Set("last_frame", pscen->NfrmLast());
        json.Set("seconds", static_cast<double>(cfrm) / kfps);
        json.Set("actor_count", pscen->Cactr());
        jsonScenes.Push(json);
        cfrmTotal += cfrm;
    }
    if (iscenRestore != ivNil && iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    if (nfrmRestore != ivNil && pmvie->Pscen() != pvNil)
        pmvie->Pscen()->FGotoFrm(nfrmRestore);
    req.result.Set("scenes", jsonScenes);
    req.result.Set("total_frames", cfrmTotal);
    req.result.Set("total_seconds", static_cast<double>(cfrmTotal) / kfps);
    return fTrue;
}

static bool _FCmdGetState(Request &req)
{
    PMVIE pmvie = PmvieCur();
    bool fAllScenes = fFalse;

    if (!req.FArgBool("all_scenes", &fAllScenes, fFalse))
        return fFalse;
    req.result.Set("screen", pmvie != pvNil ? "studio" : "building");
    req.result.Set("modal_dialog_open", vpappb->CactModal() > 0);
    if (pmvie != pvNil)
    {
        req.result.Set("movie", JsonMovieState(pmvie));
        if (fAllScenes && !_FAddSceneSummary(req, pmvie))
            return fFalse;
    }
    return fTrue;
}

static bool _FPollStudioOpen(Request &req)
{
    if (PmvieCur() != pvNil)
    {
        req.result.Set("screen", "studio");
        return fTrue;
    }
    if (TsCurrent() - req.tsStart > kdtsStudioTimeout)
    {
        req.Fail(kszErrBadState, "timed out waiting for the Studio to open");
        return fTrue;
    }
    return fFalse;
}

static bool _FCmdGotoStudio(Request &req)
{
    if (PmvieCur() != pvNil)
    {
        req.result.Set("screen", "studio");
        return fTrue;
    }

    // Same as clicking into the Studio from the building: the Util gob's
    // "Load Studio" script (util.cht, kchidScript8) tears down the current
    // place and enqueues cidLoadStudio.
    PGOK pgokUtil = (PGOK)vapp.Pkwa()->PgobFromHid(kgobUtil);
    if (pgokUtil == pvNil || !pgokUtil->FIs(kclsGOK))
        return req.Fail(kszErrBadState, "cannot reach the Studio from the current screen");

    int32_t rglw[3] = {kgobStudio1, kstDefault, (int32_t)chidNil};
    if (!pgokUtil->FRunScript(kchidScript8, rglw, 3))
        return FFailEngine(req, "the Studio could not be loaded");

    req.poll = _FPollStudioOpen;
    return fTrue;
}

static bool _FCheckCanClose(Request &req, PMVIE pmvie)
{
    bool fDiscard = fFalse;
    if (!req.FArgBool("discard_changes", &fDiscard, fFalse))
        return fFalse;
    if (pmvie->FPlaying())
        pmvie->Play(); // toggles playback off
    if (pmvie->FDirty() && pmvie->Cscen() > 0)
    {
        if (!fDiscard)
        {
            return req.Fail(kszErrBadState, "the current movie has unsaved changes; call save_movie first or pass "
                                            "\"discard_changes\": true");
        }
        _DiscardChanges(pmvie);
    }
    return fTrue;
}

static bool _FCmdNewMovie(Request &req)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie) || !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (!PstdioCur()->FLoadMovie(pvNil))
        return FFailEngine(req, "could not create a new movie");
    _strPathBeforeRestore.clear();
    req.result = JsonMovieState(PmvieCur());
    return fTrue;
}

static bool _FCmdOpenMovie(Request &req)
{
    PMVIE pmvie;
    std::string strPath;
    FNI fni;

    if (!req.FArgString("path", &strPath) || !FFniFromPath(req, strPath, kftg3mm, &fni))
        return fFalse;
    if (fni.TExists() != tYes)
        return req.Fail(kszErrIo, "no such file: " + strPath);
    if (!FRequireMovie(req, &pmvie) || !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (!PstdioCur()->FLoadMovie(&fni))
        return FFailEngine(req, "could not open '" + strPath + "'");
    _strPathBeforeRestore.clear();
    req.result = JsonMovieState(PmvieCur());
    return fTrue;
}

static bool _FCmdSaveMovie(Request &req)
{
    PMVIE pmvie;
    std::string strPath;
    FNI fni;
    STN stn;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (pmvie->Cscen() < 1)
        return req.Fail(kszErrBadState, "an empty movie cannot be saved; add a scene first");
    if (!req.FArgString("path", &strPath, fFalse))
        return fFalse;

    if (strPath.empty() && !pmvie->FGetFni(&fni))
    {
        // A movie restored from a checkpoint saves back to where it came from
        if (_strPathBeforeRestore.empty())
            return req.Fail(kszErrBadArgs, "this movie has never been saved; pass 'path' (an absolute .3mm path)");
        strPath = _strPathBeforeRestore;
    }
    if (!strPath.empty())
    {
        if (StrLower(strPath).size() < 4 || StrLower(strPath).compare(StrLower(strPath).size() - 4, 4, ".3mm") != 0)
            strPath += ".3mm";
        if (!FFniFromPath(req, strPath, kftg3mm, &fni))
            return fFalse;
        bool fOverwrite = fTrue;
        if (!req.FArgBool("overwrite", &fOverwrite, fFalse))
            return fFalse;
        FNI fniCur;
        bool fSameFile = pmvie->FGetFni(&fniCur) && fniCur.FEqual(&fni);
        if (!fOverwrite && !fSameFile && fni.TExists() == tYes)
            return req.Fail(kszErrIo, "file exists: " + strPath + " (pass \"overwrite\": true to replace it)");
    }

    if (!pmvie->FSaveToFni(&fni, fTrue))
        return FFailEngine(req, "save failed");
    // Push buffered writes to disk now, so the file is complete even if the
    // app is killed before it next flushes (part of the header is buffered)
    pmvie->Flush();
    pmvie->Pmcc()->UpdateTitle(pmvie->PstnTitle());
    _strPathBeforeRestore.clear();

    fni.GetStnPath(&stn);
    req.result.Set("path", StrFromStn(&stn));
    req.result.Set("scene_count", pmvie->Cscen());
    req.result.Set("note", "saving clears the undo history (same as the Studio's Save)");
    return fTrue;
}

/***************************************************************************
    Checkpoints

    3D Movie Maker keeps a single level of undo, so clients can snapshot the
    movie before trying something and roll back. A checkpoint is the
    Studio's "Save a Copy" into a temp folder: the working movie is not
    redirected to the copy (the engine switches its working file to a temp
    file before the next edit).
***************************************************************************/
struct Checkpoint
{
    std::string name;
    std::string path;
    std::string pathMovie; // where the movie was saved when the checkpoint was made
    int32_t cscen;
};

static std::vector<Checkpoint> _vecCheckpoints;

static std::string _StrCheckpointDir(const std::string &strName)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::temp_directory_path(ec);
    if (ec)
        path = ".";
    path /= "3dmm-checkpoints";
    path /= std::to_string(TsCurrent()) + "-" + strName;
    std::filesystem::create_directories(path, ec);
    return path.string();
}

static bool _FCmdCheckpoint(Request &req)
{
    PMVIE pmvie;
    std::string strName;
    FNI fni, fniCur;
    STN stn;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie) || !req.FArgString("name", &strName))
        return fFalse;
    if (pmvie->Cscen() < 1)
        return req.Fail(kszErrBadState, "nothing to checkpoint yet; add a scene first");
    std::string strSafe;
    for (char ch : strName)
        strSafe.push_back(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ? ch : '_');
    if (strSafe.empty() || strSafe.size() > 40)
        return req.Fail(kszErrBadArgs, "'name' must be 1-40 characters");

    // Name the file after the movie title: loading a file takes its name as
    // the title, so restoring keeps the title
    std::string strTitle = StrFromStn(pmvie->PstnTitle());
    for (char &ch : strTitle)
    {
        if (ch == '/' || ch == '\\' || ch == ':')
            ch = '-';
    }
    if (strTitle.empty())
        strTitle = "movie";
    std::string strPath = _StrCheckpointDir(strSafe) + "/" + strTitle + ".3mm";
    if (!FFniFromPath(req, strPath, kftg3mm, &fni))
        return fFalse;

    std::string strPathMovie;
    if (pmvie->FGetFni(&fniCur))
    {
        fniCur.GetStnPath(&stn);
        strPathMovie = StrFromStn(&stn);
    }
    else
    {
        strPathMovie = _strPathBeforeRestore;
    }

    if (!pmvie->FSaveToFni(&fni, fFalse))
        return FFailEngine(req, "could not write the checkpoint");
    pmvie->Flush();
    // Saving a copy marks the movie clean; it isn't saved anywhere real
    pmvie->SetDirty(fTrue);

    for (auto it = _vecCheckpoints.begin(); it != _vecCheckpoints.end(); ++it)
    {
        if (it->name == strName)
        {
            _vecCheckpoints.erase(it);
            break;
        }
    }
    _vecCheckpoints.push_back({strName, strPath, strPathMovie, pmvie->Cscen()});
    req.result.Set("name", strName);
    req.result.Set("scene_count", pmvie->Cscen());
    req.result.Set("note", "restore_checkpoint returns to this state; making a checkpoint clears undo");
    return fTrue;
}

static bool _FCmdRestoreCheckpoint(Request &req)
{
    PMVIE pmvie;
    std::string strName;
    FNI fni;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie) || !req.FArgString("name", &strName))
        return fFalse;
    const Checkpoint *pcp = nullptr;
    std::string strNames;
    for (const Checkpoint &cp : _vecCheckpoints)
    {
        if (cp.name == strName)
            pcp = &cp;
        strNames += (strNames.empty() ? "'" : ", '") + cp.name + "'";
    }
    if (pcp == nullptr)
    {
        return req.Fail(kszErrNotFound,
                        "no checkpoint named '" + strName + "'" +
                            (strNames.empty() ? std::string(" (none made yet)") : ". Checkpoints: " + strNames));
    }
    if (!FFniFromPath(req, pcp->path, kftg3mm, &fni))
        return fFalse;

    // Restoring deliberately throws away the current state
    pmvie->SetDirty(fFalse);
    pmvie->DOCB::SetDirty(fFalse);
    std::string strPathMovie = pcp->pathMovie;
    if (!PstdioCur()->FLoadMovie(&fni))
        return FFailEngine(req, "could not load checkpoint '" + strName + "'");
    pmvie = PmvieCur();
    // Don't let a later save overwrite the checkpoint; save to the movie's
    // own file instead
    pmvie->ForceSaveAs();
    pmvie->SetDirty(fTrue);
    _strPathBeforeRestore = strPathMovie;
    req.result = JsonMovieState(pmvie);
    req.result.Set("restored", strName);
    return fTrue;
}

static bool _FCmdListCheckpoints(Request &req)
{
    Json jsonList = Json::MakeArray();
    for (const Checkpoint &cp : _vecCheckpoints)
    {
        Json json = Json::MakeObject();
        json.Set("name", cp.name);
        json.Set("scene_count", cp.cscen);
        jsonList.Push(json);
    }
    req.result.Set("checkpoints", jsonList);
    return fTrue;
}

static bool _FCmdUndoRedo(Request &req, bool fRedo)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;

    int32_t cstep = 1;
    if (!req.FArgInt("steps", &cstep, fFalse))
        return fFalse;
    if (cstep < 1)
        return req.Fail(kszErrBadArgs, "'steps' must be at least 1");

    int32_t istep;
    for (istep = 0; istep < cstep; istep++)
    {
        if (!(fRedo ? pmvie->FRedo() : pmvie->FUndo()))
            break;
    }
    StrDrainEngineErrors();
    RefreshViews(pmvie);
    if (istep == 0)
        return req.Fail(kszErrBadState, fRedo ? "nothing to redo" : "nothing to undo");
    req.result.Set(fRedo ? "redone" : "undone", istep);
    req.result.Set("movie", JsonMovieState(pmvie));
    return fTrue;
}

static bool _FCmdUndo(Request &req)
{
    return _FCmdUndoRedo(req, fFalse);
}

static bool _FCmdRedo(Request &req)
{
    return _FCmdUndoRedo(req, fTrue);
}

static bool _FCmdQuit(Request &req)
{
    PMVIE pmvie = PmvieCur();
    if (pmvie != pvNil && !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (pmvie != pvNil)
        _DiscardChanges(pmvie);
    vapp.Quit(fTrue);
    req.result.Set("quitting", true);
    return fTrue;
}

const CommandInfo krgcmdSession[] = {
    {"hello", _FCmdHello, fFalse},
    {"get_state", _FCmdGetState, fFalse},
    {"goto_studio", _FCmdGotoStudio, fFalse},
    {"new_movie", _FCmdNewMovie, fTrue},
    {"open_movie", _FCmdOpenMovie, fTrue},
    {"save_movie", _FCmdSaveMovie, fTrue},
    {"checkpoint", _FCmdCheckpoint, fTrue},
    {"restore_checkpoint", _FCmdRestoreCheckpoint, fTrue},
    {"list_checkpoints", _FCmdListCheckpoints, fTrue},
    {"undo", _FCmdUndo, fTrue},
    {"redo", _FCmdRedo, fTrue},
    {"quit", _FCmdQuit, fFalse},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
