/***************************************************************************
    Automation bridge: text boxes and sounds.
***************************************************************************/
#include "bridge.h"

#include <cmath>

ASSERTNAME

namespace automation
{

bool FFniFromPath(Request &req, const std::string &strPathIn, FTG ftg, FNI *pfni);

/***************************************************************************
    Colors
***************************************************************************/
struct NamedColor
{
    const char *psz;
    uint8_t r, g, b;
};

static const NamedColor _rgnamedcolor[] = {
    {"black", 0, 0, 0},      {"white", 255, 255, 255}, {"red", 220, 20, 20},    {"green", 20, 160, 40},
    {"blue", 30, 60, 220},   {"yellow", 255, 230, 0},  {"orange", 255, 140, 0}, {"purple", 140, 40, 180},
    {"pink", 255, 120, 180}, {"brown", 120, 70, 30},   {"gray", 128, 128, 128}, {"grey", 128, 128, 128},
    {"cyan", 0, 200, 220},   {"magenta", 220, 0, 200}, {"navy", 10, 20, 100},   {"gold", 230, 180, 30},
};

// Maps a color to the nearest entry in the current 8-bit palette. Text in
// 3DMM is palettized, so exact RGB isn't always available.
// Above this average brightness, "auto" text colour picks dark text
const double kdLumaBrightText = 0.55;

static bool _FParseColor(Request &req, const Json &json, ACR *pacr)
{
    int r = -1, g = -1, b = -1;

    if (json.IsString())
    {
        std::string str = StrLower(json.AsString());
        if (str == "transparent" || str == "none" || str == "clear")
        {
            *pacr = kacrClear;
            return fTrue;
        }
        if (str.size() == 7 && str[0] == '#')
        {
            unsigned int lw;
            if (sscanf(str.c_str() + 1, "%06x", &lw) == 1)
            {
                r = (lw >> 16) & 0xFF;
                g = (lw >> 8) & 0xFF;
                b = lw & 0xFF;
            }
        }
        for (const NamedColor &nc : _rgnamedcolor)
        {
            if (str == nc.psz)
            {
                r = nc.r;
                g = nc.g;
                b = nc.b;
            }
        }
    }
    else if (json.IsArray() && json.Size() == 3 && json.AsArray()[0].IsNumber())
    {
        r = static_cast<int>(json.AsArray()[0].AsNumber());
        g = static_cast<int>(json.AsArray()[1].AsNumber());
        b = static_cast<int>(json.AsArray()[2].AsNumber());
    }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
        return req.Fail(kszErrBadArgs,
                        "colors must be \"#rrggbb\", a name (red, white, ...), [r,g,b] or \"transparent\"");

    PMVIE pmvie = PmvieCur();
    PGL pglclr = PglclrScene(pmvie != pvNil && pmvie->Pscen() != pvNil ? pmvie->Pscen()->Pbkgd() : pvNil);
    if (pglclr == pvNil)
    {
        *pacr = ACR(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
        return fTrue;
    }
    int32_t iclrBest = 0;
    int32_t lwBest = klwMax;
    for (int32_t iclr = 0; iclr < pglclr->IvMac(); iclr++)
    {
        CLR clr;
        pglclr->Get(iclr, &clr);
        // Skip the reserved system colors at both ends of the palette
        if (iclr < 10 || iclr > 245)
            continue;
        int32_t dr = clr.bRed - r, dg = clr.bGreen - g, db = clr.bBlue - b;
        int32_t lw = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (lw < lwBest)
        {
            lwBest = lw;
            iclrBest = iclr;
        }
    }
    ReleasePpo(&pglclr);
    *pacr = ACR(static_cast<uint8_t>(iclrBest));
    return fTrue;
}

/***************************************************************************
    Text boxes
***************************************************************************/

// Default caption size when the Studio's text settings were never used; the
// engine default (14) is hard to read in a 544x306 frame
const int32_t kdypCaptionDefault = 20;
static std::string _StrFontNames(void)
{
    std::string str;
    STN stn;
    for (int32_t onn = 0; onn < vntl.OnnMac(); onn++)
    {
        vntl.GetStn(onn, &stn);
        if (!str.empty())
            str += ", ";
        str += "'" + StrFromStn(&stn) + "'";
    }
    return str;
}

static bool _FRectArg(Request &req, PMVU pmvu, bool fStory, RC *prc)
{
    RC rcView;
    pmvu->GetRc(&rcView, cooLocal);
    int32_t dxp = rcView.Dxp(), dyp = rcView.Dyp();
    const Json &json = req.args.Get("rect");

    if (json.IsNull() || json.IsString())
    {
        std::string strPlace = json.IsString() ? StrLower(json.AsString()) : (fStory ? "bottom" : "full");
        int32_t dxpMargin = dxp / 20;
        if (strPlace == "bottom")
            prc->Set(dxpMargin, dyp * 3 / 4, dxp - dxpMargin, dyp - dyp / 30);
        else if (strPlace == "top")
            prc->Set(dxpMargin, dyp / 30, dxp - dxpMargin, dyp / 4);
        else if (strPlace == "center" || strPlace == "middle")
            prc->Set(dxp / 6, dyp * 3 / 8, dxp - dxp / 6, dyp * 5 / 8);
        else if (strPlace == "full")
            prc->Set(dxpMargin, dyp / 20, dxp - dxpMargin, dyp - dyp / 20);
        else
            return req.Fail(kszErrBadArgs, "'rect' must be \"top\", \"bottom\", \"center\", \"full\" or "
                                           "{left, top, right, bottom} in pixels");
        return fTrue;
    }
    if (!json.IsObject())
        return req.Fail(kszErrBadArgs, "'rect' must be a preset name or {left, top, right, bottom}");
    int32_t rglw[4];
    const char *rgpsz[4] = {"left", "top", "right", "bottom"};
    for (int i = 0; i < 4; i++)
    {
        if (!json.Get(rgpsz[i]).IsNumber())
            return req.Fail(kszErrBadArgs, std::string("'rect.") + rgpsz[i] + "' must be a number of pixels");
        rglw[i] = static_cast<int32_t>(json.Get(rgpsz[i]).AsNumber());
    }
    prc->Set(rglw[0], rglw[1], rglw[2], rglw[3]);
    prc->FIntersect(&rcView);
    if (prc->Dxp() < 40 || prc->Dyp() < 20)
    {
        return req.Fail(kszErrBadArgs, "the text box rect is too small or outside the " + std::to_string(dxp) + "x" +
                                           std::to_string(dyp) + " movie view");
    }
    return fTrue;
}

// Inserts UTF-8 text at cp. TXRD takes achar runs; convert through STN in
// chunks since an STN holds at most kcchMaxStn characters.
static bool _FInsertText(PTBOX ptbox, const std::string &strText, int32_t cp, PCHP pchp)
{
    size_t ich = 0;
    while (ich < strText.size())
    {
        // Split on a UTF-8 boundary
        size_t cch = strText.size() - ich;
        if (cch > 200)
        {
            cch = 200;
            while (cch > 0 && (static_cast<unsigned char>(strText[ich + cch]) & 0xC0) == 0x80)
                cch--;
        }
        STN stn;
        StnFromStr(strText.substr(ich, cch), &stn);
        // 3DMM uses CR for paragraph breaks
        for (int32_t ichStn = 0; ichStn < stn.Cch(); ichStn++)
        {
            if (stn.Psz()[ichStn] == ChLit('\n'))
                stn.Psz()[ichStn] = kchReturn;
        }
        if (!ptbox->FReplaceRgch(stn.Psz(), stn.Cch(), cp, 0, pchp, pvNil, fdocUpdate))
            return fFalse;
        cp += stn.Cch();
        ich += cch;
    }
    return fTrue;
}

// Parses an "align" argument ("left", "center", "right"); *pjc is left alone
// when the argument is absent
static bool _FAlignArg(Request &req, int32_t *pjc)
{
    if (!req.FHasArg("align"))
        return fTrue;
    std::string strAlign;
    if (!req.FArgString("align", &strAlign))
        return fFalse;
    strAlign = StrLower(strAlign);
    if (strAlign == "left")
        *pjc = jcLeft;
    else if (strAlign == "center" || strAlign == "centre")
        *pjc = jcCenter;
    else if (strAlign == "right")
        *pjc = jcRight;
    else
        return req.Fail(kszErrBadArgs, "'align' must be \"left\", \"center\" or \"right\"");
    return fTrue;
}

static bool _FApplyAlign(PTBOX ptbox, int32_t jc)
{
    PAP pap;
    ptbox->FetchPap(0, &pap);
    pap.jc = static_cast<uint8_t>(jc);
    return ptbox->FApplyPap(0, ptbox->CpMac(), &pap, pvNil);
}

static bool _FCmdAddText(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strText, strKind = "caption", strFont;
    int32_t dypFont;
    bool fBold = fFalse, fItalic = fFalse, fUnderline = fFalse;
    RC rc;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    PMVU pmvu = PmvuCur();
    if (pmvu == pvNil)
        return req.Fail(kszErrBadState, "no movie view");
    if (!req.FArgString("text", &strText) || !req.FArgString("kind", &strKind, fFalse) ||
        !req.FArgString("font", &strFont, fFalse) || !req.FArgBool("bold", &fBold, fFalse) ||
        !req.FArgBool("italic", &fItalic, fFalse) || !req.FArgBool("underline", &fUnderline, fFalse))
    {
        return fFalse;
    }
    if (strText.empty())
        return req.Fail(kszErrBadArgs, "'text' must not be empty");

    // 3DMM calls static text boxes "story" boxes; the other kind scrolls up
    // like movie credits during playback.
    strKind = StrLower(strKind);
    bool fStory;
    if (strKind == "caption" || strKind == "story" || strKind == "static")
        fStory = fTrue;
    else if (strKind == "scrolling" || strKind == "credits" || strKind == "crawl")
        fStory = fFalse;
    else
        return req.Fail(kszErrBadArgs, "'kind' must be \"caption\" (static text) or \"scrolling\" (credits)");

    if (!_FRectArg(req, pmvu, fStory, &rc) || !FApplyFrameArg(req, pscen))
        return fFalse;

    CHP chp;
    chp.Clear();
    chp.onn = pmvu->OnnTextCur();
    chp.dypFont = pmvu->DypFontTextCur();
    chp.grfont = pmvu->GrfontStyleTextCur();
    chp.acrFore = pmvu->AcrPaint();
    chp.acrBack = kacrClear;
    if (chp.dypFont <= 0)
    {
        // The Studio's text settings are only initialized once its text
        // tools have been used; fall back to the engine's text box defaults
        chp.onn = vpappb->OnnDefVariable();
        chp.dypFont = LwMax(pmvie->Pmcc()->DypTboxDef(), kdypCaptionDefault);
        chp.grfont = fontNil;
        chp.acrFore = kacrBlack;
    }
    if (!strFont.empty())
    {
        STN stn;
        StnFromStr(strFont, &stn);
        if (!vntl.FGetOnn(&stn, &chp.onn))
            return req.Fail(kszErrNotFound, "no font named '" + strFont + "'. Fonts: " + _StrFontNames());
    }
    if (req.FHasArg("size"))
    {
        if (!req.FArgInt("size", &dypFont))
            return fFalse;
        if (dypFont < 6 || dypFont > 200)
            return req.Fail(kszErrBadArgs, "'size' must be between 6 and 200 (pixels)");
        chp.dypFont = dypFont;
    }
    if (req.FHasArg("bold") || req.FHasArg("italic") || req.FHasArg("underline"))
    {
        chp.grfont = (fBold ? fontBold : 0) | (fItalic ? fontItalic : 0) | (fUnderline ? fontUnderline : 0);
    }
    const Json &jsonColor = req.args.Get("color");
    if (jsonColor.IsString() && StrLower(jsonColor.AsString()) == "auto")
    {
        // Readable over whatever is behind it: dark text on a bright area,
        // the usual yellow on a dark one
        double dLuma = 0;
        bool fBright = FMeanLuminance(pmvie, rc, &dLuma) && dLuma > kdLumaBrightText;
        if (!_FParseColor(req, Json(fBright ? "#202050" : "#ffff40"), &chp.acrFore))
            return fFalse;
        req.result.Set("color", fBright ? "dark (bright background)" : "yellow (dark background)");
    }
    else if (req.FHasArg("color") && !_FParseColor(req, jsonColor, &chp.acrFore))
    {
        return fFalse;
    }
    ACR acrBox = kacrClear;
    if (req.FHasArg("background") && !_FParseColor(req, req.args.Get("background"), &acrBox))
        return fFalse;
    // Titles and subtitles placed with a preset read best centred
    const Json &jsonRect = req.args.Get("rect");
    std::string strRect = jsonRect.IsString() ? StrLower(jsonRect.AsString()) : (fStory ? "bottom" : "full");
    int32_t jc =
        strRect == "top" || strRect == "bottom" || strRect == "center" || strRect == "middle" ? jcCenter : jcLeft;
    if (!_FAlignArg(req, &jc))
        return fFalse;
    int32_t nfrmUntil = ivNil;
    if (req.FHasArg("until_frame"))
    {
        if (!req.FArgInt("until_frame", &nfrmUntil))
            return fFalse;
        if (nfrmUntil <= pscen->Nfrm())
            return req.Fail(kszErrBadArgs, "'until_frame' must be after the frame the text appears on");
    }

    int32_t nfrmStart = pscen->Nfrm();
    PTBOX ptbox = TBOX::PtboxNew(pscen, &rc, fStory);
    if (ptbox == pvNil)
        return FFailEngine(req, "could not create the text box");
    ptbox->SetDypFontDef(chp.dypFont);
    ptbox->SetOnnDef(chp.onn);
    if (!pscen->FAddTbox(ptbox))
    {
        ReleasePpo(&ptbox);
        return FFailEngine(req, "could not add the text box");
    }
    pscen->SelectTbox(ptbox);

    bool fOk = _FInsertText(ptbox, strText, 0, &chp);
    if (fOk && acrBox != kacrClear)
        fOk = ptbox->FSetAcrBack(acrBox);
    if (fOk && jc != jcLeft)
        fOk = _FApplyAlign(ptbox, jc);
    if (!fOk)
    {
        pscen->FRemTboxCore(ptbox);
        ReleasePpo(&ptbox);
        return FFailEngine(req, "could not set the text");
    }

    if (nfrmUntil != ivNil)
    {
        if (!FGotoFrame(req, pscen, nfrmUntil))
        {
            ReleasePpo(&ptbox);
            return fFalse;
        }
        pscen->SelectTbox(ptbox);
        if (!ptbox->FHide())
        {
            ReleasePpo(&ptbox);
            return FFailEngine(req, "the text was added but could not be hidden at frame " + std::to_string(nfrmUntil));
        }
        pscen->FGotoFrm(nfrmStart);
    }

    int32_t itbox = ivNil;
    for (int32_t i = 0; i < pscen->Ctbox(); i++)
    {
        if (pscen->PtboxFromItbox(i) == ptbox)
            itbox = i;
    }
    pscen->SelectTbox(pvNil);
    ReleasePpo(&ptbox);
    pmvie->SetDirty();
    pmvie->Pmcc()->EnableTboxTools();
    RefreshViews(pmvie);

    Json jsonRc = Json::MakeObject();
    jsonRc.Set("left", rc.xpLeft);
    jsonRc.Set("top", rc.ypTop);
    jsonRc.Set("right", rc.xpRight);
    jsonRc.Set("bottom", rc.ypBottom);
    req.result.Set("text_box_index", itbox);
    req.result.Set("kind", fStory ? "caption" : "scrolling");
    req.result.Set("rect", jsonRc);
    req.result.Set("align", jc == jcCenter ? "center" : (jc == jcRight ? "right" : "left"));
    req.result.Set("first_frame", nfrmStart);
    req.result.Set("until_frame", nfrmUntil == ivNil ? Json("end of scene") : Json(nfrmUntil));
    return fTrue;
}

static bool _FTboxArg(Request &req, PSCEN pscen, PTBOX *pptbox)
{
    int32_t itbox;
    if (!req.FArgInt("index", &itbox))
        return fFalse;
    *pptbox = itbox >= 0 && itbox < pscen->Ctbox() ? pscen->PtboxFromItbox(itbox) : pvNil;
    if (*pptbox == pvNil)
    {
        return req.Fail(kszErrNotFound, "no text box " + std::to_string(itbox) + "; this scene has " +
                                            std::to_string(pscen->Ctbox()) +
                                            " (indexes from describe_scene or describe_timeline)");
    }
    return fTrue;
}

static bool _FCmdEditText(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    PTBOX ptbox;
    std::string strText;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) || !_FTboxArg(req, pscen, &ptbox))
        return fFalse;
    if (!req.FArgString("text", &strText, fFalse))
        return fFalse;

    // Keep the existing look unless told otherwise
    CHP chp;
    ptbox->FetchChp(0, &chp);
    if (req.FHasArg("color") && !_FParseColor(req, req.args.Get("color"), &chp.acrFore))
        return fFalse;
    if (req.FHasArg("size"))
    {
        int32_t dypFont;
        if (!req.FArgInt("size", &dypFont))
            return fFalse;
        if (dypFont < 6 || dypFont > 200)
            return req.Fail(kszErrBadArgs, "'size' must be between 6 and 200 (pixels)");
        chp.dypFont = dypFont;
    }
    int32_t jc = ivNil;
    if (!_FAlignArg(req, &jc))
        return fFalse;

    bool fOk;
    if (req.FHasArg("text"))
    {
        if (strText.empty())
            return req.Fail(kszErrBadArgs, "'text' must not be empty (use remove_text to delete the box)");
        int32_t cpMac = ptbox->CpMac();
        fOk = ptbox->FReplaceRgch(pvNil, 0, 0, cpMac - 1, &chp, pvNil, fdocUpdate) &&
              _FInsertText(ptbox, strText, 0, &chp);
    }
    else
    {
        fOk = ptbox->FApplyChp(0, ptbox->CpMac() - 1, &chp);
    }
    if (fOk && jc != ivNil)
        fOk = _FApplyAlign(ptbox, jc);
    if (!fOk)
        return FFailEngine(req, "could not change the text");
    pmvie->SetDirty();
    RefreshViews(pmvie);
    req.result.Set("index", req.args.Get("index"));
    req.result.Set("note", "undo can't revert text edits made this way");
    return fTrue;
}

static bool _FCmdRemoveText(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    PTBOX ptbox;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) || !_FTboxArg(req, pscen, &ptbox))
        return fFalse;
    if (!pscen->FRemTbox(ptbox))
        return FFailEngine(req, "could not remove the text box");
    pmvie->SetDirty();
    RefreshViews(pmvie);
    req.result.Set("removed", req.args.Get("index"));
    req.result.Set("text_box_count", pscen->Ctbox());
    return fTrue;
}

/***************************************************************************
    Sounds
***************************************************************************/
static bool _FSoundTypeArg(Request &req, int32_t *psty)
{
    std::string strType;
    if (!req.FArgString("type", &strType))
        return fFalse;
    strType = StrLower(strType);
    if (strType == "sfx" || strType == "effect" || strType == "sound effect")
        *psty = stySfx;
    else if (strType == "speech" || strType == "voice" || strType == "dialogue")
        *psty = stySpeech;
    else if (strType == "music" || strType == "midi")
        *psty = styMidi;
    else
        return req.Fail(kszErrBadArgs, "'type' must be \"sfx\", \"speech\" or \"music\"");
    return fTrue;
}

static bool _FCmdImportSound(Request &req)
{
    PMVIE pmvie;
    std::string strPath, strName;
    int32_t sty;
    FNI fni;
    CNO cno;
    STN stn;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("path", &strPath) || !_FSoundTypeArg(req, &sty) || !req.FArgString("name", &strName, fFalse))
        return fFalse;
    if (!FFniFromPath(req, strPath, ftgNil, &fni))
        return fFalse;
    if (fni.TExists() != tYes)
        return req.Fail(kszErrIo, "no such file: " + strPath);

    FTG ftg = fni.Ftg();
    if (sty == styMidi && ftg != kftgMidi)
        return req.Fail(kszErrBadArgs, "music must be a MIDI file (.mid); use type sfx or speech for WAV/MP3");
    if (sty != styMidi && ftg != kftgWave && ftg != kftgMP3)
        return req.Fail(kszErrBadArgs, "sound effects and speech must be .wav or .mp3 files");

    PFIL pfil = FIL::PfilOpen(&fni);
    if (pfil == pvNil)
        return FFailEngine(req, "could not open '" + strPath + "'");
    if (!strName.empty())
        StnFromStr(strName, &stn);
    bool fOk = pmvie->FCopySndFileToMvie(pfil, sty, &cno, strName.empty() ? pvNil : &stn);
    ReleasePpo(&pfil);
    if (!fOk)
        return FFailEngine(req, "could not import '" + strPath + "'");
    pmvie->SetDirty();

    req.result.Set("sound_id", "user:" + std::to_string(static_cast<uint32_t>(cno)));
    req.result.Set("type", sty == styMidi ? "music" : (sty == stySpeech ? "speech" : "sfx"));
    req.result.Set("note", "attach it with attach_sound; imported sounds that are never attached are dropped when "
                           "the movie is saved and closed");
    return fTrue;
}

static bool _FSoundTag(Request &req, PMVIE pmvie, const std::string &strSound, TAG *ptag)
{
    if (strSound.compare(0, 5, "user:") == 0)
    {
        char *pchEnd = nullptr;
        unsigned long lu = strtoul(strSound.c_str() + 5, &pchEnd, 10);
        if (pchEnd == nullptr || *pchEnd != '\0')
            return req.Fail(kszErrBadArgs, "bad imported sound id '" + strSound + "'");
        PCRF pcrf;
        if (!pmvie->FEnsureAutosave(&pcrf))
            return FFailEngine(req, "the movie file is not available");
        ptag->sid = ksidUseCrf;
        ptag->pcrf = pcrf;
        ptag->ctg = kctgMsnd;
        ptag->cno = static_cast<CNO>(lu);
        if (!pcrf->Pcfl()->FFind(kctgMsnd, ptag->cno))
            return req.Fail(kszErrNotFound, "no imported sound '" + strSound + "' in this movie");
        return fTrue;
    }

    const CatalogEntry *pentry;
    if (!FFindContent(req, strSound, {ContentKind::SoundEffect, ContentKind::SoundSpeech, ContentKind::SoundMusic},
                      &pentry))
    {
        return fFalse;
    }
    *ptag = pentry->tag;
    if (!vptagm->FCacheTagToHD(ptag))
        return FFailEngine(req, "could not load sound '" + pentry->name + "'");
    return fTrue;
}

static bool _FCmdAttachSound(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strSound, strTarget = "scene";
    bool fLoop = fFalse, fQueue = fFalse, fMotionMatch = fFalse;
    double dVolume = -1;
    TAG tag;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("sound", &strSound) || !req.FArgBool("loop", &fLoop, fFalse) ||
        !req.FArgBool("queue", &fQueue, fFalse) || !req.FArgBool("motion_match", &fMotionMatch, fFalse) ||
        !req.FArgNumber("volume", &dVolume, fFalse) || !FApplyFrameArg(req, pscen))
    {
        return fFalse;
    }
    int32_t vlm = vlmNil;
    if (dVolume >= 0)
    {
        if (dVolume > 2)
            return req.Fail(kszErrBadArgs, "'volume' must be between 0 and 2 (1 = normal)");
        vlm = static_cast<int32_t>(dVolume * kvlmFull);
    }
    if (!_FSoundTag(req, pmvie, strSound, &tag))
        return fFalse;

    const Json &jsonTarget = req.args.Get("target");
    bool fScene = jsonTarget.IsNull() || (jsonTarget.IsString() && StrLower(jsonTarget.AsString()) == "scene");
    if (fScene)
    {
        if (!pmvie->FAddBkgdSnd(&tag, fLoop ? tribool::tYes : tribool::tNo, fQueue ? tribool::tYes : tribool::tNo, vlm,
                                styNil))
        {
            return FFailEngine(req, "could not add the sound to the scene");
        }
        req.result.Set("target", "scene");
    }
    else
    {
        PACTR pactr;
        Request reqT;
        if (!FResolveActor(req, "target", &pactr))
            return fFalse;
        pscen->SelectActr(pactr);
        if (!pmvie->FAddActrSnd(&tag, fLoop ? tribool::tYes : tribool::tNo, fQueue ? tribool::tYes : tribool::tNo,
                                fMotionMatch ? tribool::tYes : tribool::tNo, vlm, styNil))
        {
            return FFailEngine(req, "could not attach the sound to the actor");
        }
        req.result.Set("target", pactr->Arid());
    }
    pmvie->SetDirty();
    RefreshViews(pmvie);
    req.result.Set("frame", pscen->Nfrm());
    req.result.Set("sound", strSound);
    return fTrue;
}

static bool _FCmdRemoveSound(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    int32_t sty;
    bool fMotionMatch = fFalse;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!_FSoundTypeArg(req, &sty) || !req.FArgBool("motion_match", &fMotionMatch, fFalse) ||
        !FApplyFrameArg(req, pscen))
    {
        return fFalse;
    }
    const Json &jsonTarget = req.args.Get("target");
    bool fScene = jsonTarget.IsNull() || (jsonTarget.IsString() && StrLower(jsonTarget.AsString()) == "scene");
    if (fScene)
    {
        // Scene sound events at this frame (undoable)
        if (!pscen->FRemSnd(sty))
            return FFailEngine(req, "could not remove the scene sound");
    }
    else
    {
        PACTR pactr, pactrDup;
        if (!FResolveActor(req, "target", &pactr))
            return fFalse;
        if (!pactr->FDup(&pactrDup))
            return FFailEngine(req, "could not remove the sound");
        if (!pactr->FDeleteSndCore(sty, fMotionMatch))
        {
            pactr->Restore(pactrDup);
            ReleasePpo(&pactrDup);
            return FFailEngine(req, "could not remove the actor's sound");
        }
        pactr->FCreateUndo(pactrDup, fTrue);
        ReleasePpo(&pactrDup);
    }
    pmvie->SetDirty();
    RefreshViews(pmvie);
    req.result.Set("frame", pscen->Nfrm());
    req.result.Set("note", "removes the sound events of that type that start at this frame (see describe_timeline "
                           "for when sounds start)");
    return fTrue;
}

const CommandInfo krgcmdMedia[] = {
    {"add_text", _FCmdAddText, fTrue},
    {"import_sound", _FCmdImportSound, fTrue},
    {"attach_sound", _FCmdAttachSound, fTrue},
    {"edit_text", _FCmdEditText, fTrue},
    {"remove_text", _FCmdRemoveText, fTrue},
    {"remove_sound", _FCmdRemoveSound, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
