/***************************************************************************
    Automation bridge: content catalog (backgrounds, cameras, templates,
    actions, costumes, sounds, 3D word styles).

    Content is enumerated exactly like the Studio browsers do it: from the
    thumbnail descriptor files (.3th), via BCLS. See NOTES.md section 4.
***************************************************************************/
#include "bridge.h"

#include <cmath>
#include <map>

ASSERTNAME

namespace automation
{

struct KindInfo
{
    ContentKind kind;
    CTG ctgThumb;
    const char *pszPrefix;
    const char *pszName;
};

static const KindInfo _rgkindinfo[] = {
    {ContentKind::Background, kctgBkth, "bkgd", "background"},
    {ContentKind::Actor, kctgTmth, "actor", "actor"},
    {ContentKind::Prop, kctgPrth, "prop", "prop"},
    {ContentKind::SoundEffect, kctgSfth, "sfx", "sound effect"},
    {ContentKind::SoundSpeech, kctgSvth, "speech", "speech"},
    {ContentKind::SoundMusic, kctgSmth, "music", "music"},
    {ContentKind::Font3d, kctgTfth, "font", "3D font"},
    {ContentKind::Material, kctgMtth, "mtrl", "material"},
};

static const KindInfo *_PkindinfoFor(ContentKind kind)
{
    for (const KindInfo &kindinfo : _rgkindinfo)
    {
        if (kindinfo.kind == kind)
            return &kindinfo;
    }
    Bug("unknown content kind");
    return &_rgkindinfo[0];
}

const char *PszKindName(ContentKind kind)
{
    return _PkindinfoFor(kind)->pszName;
}

static std::map<ContentKind, std::vector<CatalogEntry>> _mapCatalog;

// Names of content chunks, keyed by (ctg, cno), read from the product's .3cn
// content files. Thumbnail files only name some kinds of content (sounds);
// backgrounds and templates are named by their content chunks.
static std::map<std::pair<CTG, CNO>, std::string> _mapContentName;
static bool _fContentNamesLoaded = fFalse;

static void _LoadContentNames(void)
{
    FNI fniDir, fni;
    FNE fne;
    FTG ftg = kftgContent;
    CKI cki;
    STN stn;

    _fContentNamesLoaded = fTrue;
    vapp.GetFniProduct(&fniDir);
    if (!fne.FInit(&fniDir, &ftg, 1))
        return;
    while (fne.FNextFni(&fni))
    {
        PCFL pcfl = CFL::PcflOpen(&fni, fcflNil);
        if (pcfl == pvNil)
            continue;
        for (CTG ctg : {kctgBkgd, kctgTmpl, kctgTdf, kctgMsnd, kctgMtrl})
        {
            for (int32_t icki = 0; icki < pcfl->CckiCtg(ctg); icki++)
            {
                if (pcfl->FGetCkiCtg(ctg, icki, &cki) && pcfl->FGetName(cki.ctg, cki.cno, &stn) && stn.Cch() > 0)
                    _mapContentName[std::make_pair(cki.ctg, cki.cno)] = StrFromStn(&stn);
            }
        }
        ReleasePpo(&pcfl);
    }
}

static std::string _StrContentName(PTAG ptag)
{
    if (!_fContentNamesLoaded)
        _LoadContentNames();
    auto it = _mapContentName.find(std::make_pair(ptag->ctg, ptag->cno));
    if (it != _mapContentName.end())
        return it->second;
    return std::string();
}

static bool _FLoadCatalog(ContentKind kind, std::vector<CatalogEntry> *pvec)
{
    const KindInfo *pkindinfo = _PkindinfoFor(kind);
    CKI cki;
    THD thd;
    STN stn;
    PGL pglthd;
    PGST pgst = pvNil;

    cki.ctg = pkindinfo->ctgThumb;
    cki.cno = cnoNil;

    // BCLS also collects the thumbnail names, but fails outright if any
    // thumbnail chunk is unnamed; fall back to BCL + content chunk names.
    PBCLS pbcls = BCLS::PbclsNew(pvNil, &cki, ctgNil, pvNil, pvNil, fTrue);
    PBCL pbcl = pvNil;
    if (pbcls != pvNil)
    {
        pglthd = pbcls->Pglthd();
        pgst = pbcls->Pgst();
    }
    else
    {
        pbcl = BCL::PbclNew(pvNil, &cki, ctgNil, pvNil, fTrue);
        if (pbcl == pvNil)
            return fFalse;
        pglthd = pbcl->Pglthd();
    }

    for (int32_t ithd = 0; ithd < pglthd->IvMac(); ithd++)
    {
        CatalogEntry entry;
        pglthd->Get(ithd, &thd);
        entry.kind = kind;
        entry.tag = thd.tag;
        entry.tag.pcrf = pvNil;
        if (pgst != pvNil && ithd < pgst->IvMac())
        {
            pgst->GetStn(ithd, &stn);
            entry.name = StrFromStn(&stn);
        }
        if (entry.name.empty())
            entry.name = _StrContentName(&entry.tag);
        entry.id = std::string(pkindinfo->pszPrefix) + ":" + std::to_string(thd.tag.sid) + ":" +
                   std::to_string(static_cast<uint32_t>(thd.tag.cno));
        if (entry.name.empty())
            entry.name = entry.id;
        pvec->push_back(entry);
    }
    ReleasePpo(&pbcls);
    ReleasePpo(&pbcl);
    return fTrue;
}

const std::vector<CatalogEntry> *PvecCatalog(ContentKind kind)
{
    auto it = _mapCatalog.find(kind);
    if (it != _mapCatalog.end())
        return &it->second;

    std::vector<CatalogEntry> vec;
    if (!_FLoadCatalog(kind, &vec))
        return nullptr;
    return &(_mapCatalog[kind] = std::move(vec));
}

bool FFindContent(Request &req, const std::string &strRef, const std::vector<ContentKind> &kinds,
                  const CatalogEntry **ppentry)
{
    std::string strWant = StrLower(strRef);
    std::vector<const CatalogEntry *> vecPrefix;
    std::vector<const CatalogEntry *> vecSubstr;
    std::string strKinds;

    *ppentry = nullptr;
    for (ContentKind kind : kinds)
    {
        const std::vector<CatalogEntry> *pvec = PvecCatalog(kind);
        if (!strKinds.empty())
            strKinds += "/";
        strKinds += PszKindName(kind);
        if (pvec == nullptr)
            continue;
        for (const CatalogEntry &entry : *pvec)
        {
            if (entry.id == strRef)
            {
                *ppentry = &entry;
                return fTrue;
            }
        }
        for (const CatalogEntry &entry : *pvec)
        {
            std::string strName = StrLower(entry.name);
            if (strName == strWant)
            {
                *ppentry = &entry;
                return fTrue;
            }
            if (strName.compare(0, strWant.size(), strWant) == 0)
                vecPrefix.push_back(&entry);
            else if (strName.find(strWant) != std::string::npos)
                vecSubstr.push_back(&entry);
        }
    }

    // A unique partial match is good enough
    if (vecPrefix.size() == 1)
    {
        *ppentry = vecPrefix[0];
        return fTrue;
    }
    if (vecPrefix.empty() && vecSubstr.size() == 1)
    {
        *ppentry = vecSubstr[0];
        return fTrue;
    }

    std::vector<const CatalogEntry *> &vecSuggest = vecPrefix.empty() ? vecSubstr : vecPrefix;
    std::string strMessage = "no " + strKinds + " named '" + strRef + "'";
    if (!vecSuggest.empty())
    {
        strMessage += "; it matches several: ";
        for (size_t i = 0; i < vecSuggest.size() && i < 12; i++)
        {
            if (i > 0)
                strMessage += ", ";
            strMessage += "'" + vecSuggest[i]->name + "'";
        }
    }
    else
    {
        strMessage += "; use the matching list_* command to see what exists";
    }
    return req.Fail(kszErrNotFound, strMessage);
}

/***************************************************************************
    Templates and actions
***************************************************************************/
PTMPL PtmplFetch(PTAG ptag)
{
    if (!vptagm->FCacheTagToHD(ptag))
        return pvNil;
    return (PTMPL)vptagm->PbacoFetch(ptag, TMPL::FReadTmpl);
}

std::string StrActionName(PTMPL ptmpl, int32_t anid)
{
    STN stn;
    if (anid < 0 || anid >= ptmpl->Cactn() || !ptmpl->FGetActnName(anid, &stn))
        return std::string();
    return StrFromStn(&stn);
}

static double _DwrAverageStep(PTMPL ptmpl, int32_t anid, int32_t ccel)
{
    double dwrTotal = 0;
    for (int32_t celn = 0; celn < ccel; celn++)
    {
        BRS dwr;
        if (ptmpl->FGetDwrActnCel(anid, celn, &dwr))
            dwrTotal += DFromBrs(dwr);
    }
    return ccel > 0 ? dwrTotal / ccel : 0;
}

Json JsonActionList(PTMPL ptmpl)
{
    Json jsonList = Json::MakeArray();
    for (int32_t anid = 0; anid < ptmpl->Cactn(); anid++)
    {
        int32_t ccel = 0;
        Json json = Json::MakeObject();
        ptmpl->FGetCcelActn(anid, &ccel);
        double dwrStep = _DwrAverageStep(ptmpl, anid, ccel);
        json.Set("index", anid);
        json.Set("name", StrActionName(ptmpl, anid));
        json.Set("cels", ccel);
        // Metres travelled per frame when this action is used for a path
        json.Set("metres_per_frame", std::floor(dwrStep / kdwrPerMetre * 1000 + 0.5) / 1000);
        json.Set("moves", dwrStep > 0);
        jsonList.Push(json);
    }
    return jsonList;
}

bool FResolveAction(Request &req, PTMPL ptmpl, const Json &jsonAction, int32_t *panid)
{
    if (jsonAction.IsNumber())
    {
        int32_t anid = static_cast<int32_t>(jsonAction.AsNumber());
        if (anid < 0 || anid >= ptmpl->Cactn())
        {
            return req.Fail(kszErrNotFound, "action index " + std::to_string(anid) + " is out of range (0-" +
                                                std::to_string(ptmpl->Cactn() - 1) + ")");
        }
        *panid = anid;
        return fTrue;
    }
    if (!jsonAction.IsString())
        return req.Fail(kszErrBadArgs, "'action' must be an action name or index (see list_actions)");

    // Names may end in " *" (the Studio's marker for the walking family);
    // ignore it. Exact match first, then a unique prefix, then a unique
    // substring.
    auto strip = [](std::string str) {
        str = StrLower(str);
        while (!str.empty() && (str.back() == '*' || str.back() == ' '))
            str.pop_back();
        return str;
    };
    std::string strWant = strip(jsonAction.AsString());
    std::vector<int32_t> rgvecMatch[2];
    std::string strNames;
    for (int32_t anid = 0; anid < ptmpl->Cactn(); anid++)
    {
        std::string strName = StrActionName(ptmpl, anid);
        std::string strLower = strip(strName);
        if (strLower == strWant)
        {
            *panid = anid;
            return fTrue;
        }
        if (strLower.compare(0, strWant.size(), strWant) == 0)
            rgvecMatch[0].push_back(anid);
        else if (strLower.find(strWant) != std::string::npos)
            rgvecMatch[1].push_back(anid);
        if (!strNames.empty())
            strNames += ", ";
        strNames += "'" + strName + "'";
    }
    for (const std::vector<int32_t> &vecMatch : rgvecMatch)
    {
        if (vecMatch.size() == 1)
        {
            *panid = vecMatch[0];
            return fTrue;
        }
        if (vecMatch.size() > 1)
        {
            std::string strMatches;
            for (int32_t anid : vecMatch)
                strMatches += (strMatches.empty() ? "'" : ", '") + StrActionName(ptmpl, anid) + "'";
            return req.Fail(kszErrNotFound, "action '" + jsonAction.AsString() + "' is ambiguous; it matches " +
                                                strMatches + ". Pass one of those names.");
        }
    }

    STN stn;
    ptmpl->GetName(&stn);
    return req.Fail(kszErrNotFound, "'" + StrFromStn(&stn) + "' has no action named '" + jsonAction.AsString() +
                                        "'. Its actions are: " + strNames);
}

/***************************************************************************
    Commands
***************************************************************************/
static Json _JsonEntry(const CatalogEntry &entry)
{
    Json json = Json::MakeObject();
    json.Set("id", entry.id);
    json.Set("name", entry.name);
    return json;
}

static bool _FListKinds(Request &req, const std::vector<ContentKind> &kinds, const char *pszType)
{
    std::string strFilter;
    if (!req.FArgString("filter", &strFilter, fFalse))
        return fFalse;
    strFilter = StrLower(strFilter);

    Json jsonItems = Json::MakeArray();
    for (ContentKind kind : kinds)
    {
        const std::vector<CatalogEntry> *pvec = PvecCatalog(kind);
        if (pvec == nullptr)
            return FFailEngine(req, std::string("could not read the ") + PszKindName(kind) + " catalog");
        for (const CatalogEntry &entry : *pvec)
        {
            if (!strFilter.empty() && StrLower(entry.name).find(strFilter) == std::string::npos)
                continue;
            Json json = _JsonEntry(entry);
            if (pszType != nullptr)
                json.Set("type", PszKindName(kind));
            jsonItems.Push(json);
        }
    }
    req.result.Set("count", static_cast<int>(jsonItems.Size()));
    req.result.Set("items", jsonItems);
    return fTrue;
}

static Json _JsonCameras(const CatalogEntry &entry)
{
    Json jsonCams = Json::MakeArray();
    CKI cki;
    THD thd;
    STN stn;

    cki.ctg = kctgBkth;
    cki.cno = entry.tag.cno;
    PBCL pbcl = BCL::PbclNew(pvNil, &cki, kctgCath, pvNil, fTrue);
    if (pbcl == pvNil)
        return jsonCams;

    // Sort by camera index (chid) so index == the number set_camera takes
    std::vector<std::pair<int32_t, std::string>> vecCams;
    for (int32_t ithd = 0; ithd < pbcl->IthdMac(); ithd++)
    {
        pbcl->GetThd(ithd, &thd);
        vecCams.emplace_back(static_cast<int32_t>(thd.chid), std::string());
    }
    ReleasePpo(&pbcl);
    std::sort(vecCams.begin(), vecCams.end());
    for (auto &cam : vecCams)
    {
        Json json = Json::MakeObject();
        json.Set("index", cam.first);
        json.Set("name", cam.second.empty() ? "Camera " + std::to_string(cam.first + 1) : cam.second);
        jsonCams.Push(json);
    }
    return jsonCams;
}

static bool _FCmdListBackgrounds(Request &req)
{
    bool fCameras = fTrue;
    if (!req.FArgBool("include_cameras", &fCameras, fFalse))
        return fFalse;
    if (!_FListKinds(req, {ContentKind::Background}, nullptr))
        return fFalse;

    if (fCameras)
    {
        // Annotate each background with its cameras
        Json jsonItems = Json::MakeArray();
        for (const Json &jsonItem : req.result.Get("items").AsArray())
        {
            const CatalogEntry *pentry;
            Json json = jsonItem;
            Request reqT;
            if (FFindContent(reqT, jsonItem.Get("id").AsString(), {ContentKind::Background}, &pentry))
            {
                Json jsonCams = _JsonCameras(*pentry);
                json.Set("camera_count", static_cast<int>(jsonCams.Size()));
                json.Set("cameras", jsonCams);
            }
            jsonItems.Push(json);
        }
        req.result.Set("items", jsonItems);
    }
    return fTrue;
}

static bool _FCmdListCameras(Request &req)
{
    std::string strBkgd;
    const CatalogEntry *pentry;
    PMVIE pmvie = PmvieCur();

    if (!req.FArgString("background", &strBkgd, fFalse))
        return fFalse;
    if (strBkgd.empty())
    {
        TAG tag;
        if (pmvie == pvNil || pmvie->Pscen() == pvNil || !pmvie->Pscen()->FGetTagBkgd(&tag))
            return req.Fail(kszErrBadArgs, "pass 'background' (no scene is open to default to)");
        strBkgd = "bkgd:" + std::to_string(tag.sid) + ":" + std::to_string(static_cast<uint32_t>(tag.cno));
    }
    if (!FFindContent(req, strBkgd, {ContentKind::Background}, &pentry))
        return fFalse;
    req.result.Set("background", _JsonEntry(*pentry));
    req.result.Set("cameras", _JsonCameras(*pentry));
    return fTrue;
}

static bool _FParseTemplateKinds(Request &req, std::vector<ContentKind> *pkinds)
{
    std::string strType = "all";
    if (!req.FArgString("type", &strType, fFalse))
        return fFalse;
    strType = StrLower(strType);
    if (strType == "all" || strType.empty())
        *pkinds = {ContentKind::Actor, ContentKind::Prop};
    else if (strType == "actor" || strType == "actors")
        *pkinds = {ContentKind::Actor};
    else if (strType == "prop" || strType == "props")
        *pkinds = {ContentKind::Prop};
    else
        return req.Fail(kszErrBadArgs, "'type' must be \"actor\", \"prop\" or \"all\"");
    return fTrue;
}

static bool _FCmdListTemplates(Request &req)
{
    std::vector<ContentKind> kinds;
    if (!_FParseTemplateKinds(req, &kinds))
        return fFalse;
    return _FListKinds(req, kinds, "type");
}

static bool _FFetchTemplateArg(Request &req, const CatalogEntry **ppentry, PTMPL *pptmpl)
{
    std::string strTmpl;
    if (!req.FArgString("template", &strTmpl))
        return fFalse;
    if (!FFindContent(req, strTmpl, {ContentKind::Actor, ContentKind::Prop}, ppentry))
        return fFalse;
    TAG tag = (*ppentry)->tag;
    *pptmpl = PtmplFetch(&tag);
    if (*pptmpl == pvNil)
        return FFailEngine(req, "could not load template '" + (*ppentry)->name + "'");
    return fTrue;
}

static bool _FCmdListActions(Request &req)
{
    const CatalogEntry *pentry;
    PTMPL ptmpl;

    if (!_FFetchTemplateArg(req, &pentry, &ptmpl))
        return fFalse;
    req.result.Set("template", _JsonEntry(*pentry));
    req.result.Set("actions", JsonActionList(ptmpl));
    ReleasePpo(&ptmpl);
    return fTrue;
}

static bool _FCmdListCostumes(Request &req)
{
    const CatalogEntry *pentry;
    PTMPL ptmpl;

    if (!_FFetchTemplateArg(req, &pentry, &ptmpl))
        return fFalse;

    // A body is made of "body part sets" (e.g. head, torso, legs); each set
    // may have several built-in custom costumes (cmids).
    PBODY pbody = ptmpl->PbodyCreate();
    int32_t cbset = pbody == pvNil ? 0 : pbody->Cbset();
    ReleasePpo(&pbody);

    Json jsonSets = Json::MakeArray();
    for (int32_t ibset = 0; ibset < cbset; ibset++)
    {
        Json json = Json::MakeObject();
        int32_t ibsetAcc;
        json.Set("body_part", ibset);
        json.Set("costume_count", ptmpl->CcmidOfBset(ibset));
        bool fAcc = ptmpl->FIbsetAccOfIbset(ibset, &ibsetAcc);
        json.Set("is_accessory", fAcc && ibsetAcc == ibset);
        jsonSets.Push(json);
    }
    req.result.Set("template", _JsonEntry(*pentry));
    req.result.Set("body_parts", jsonSets);
    req.result.Set("note", "set_costume(actor, body_part, costume) picks one of costume_count built-in costumes for "
                           "that body part (0-based). set_costume(actor, body_part, material) instead paints the "
                           "part with a material from list_materials.");
    ReleasePpo(&ptmpl);
    return fTrue;
}

static bool _FCmdListMaterials(Request &req)
{
    return _FListKinds(req, {ContentKind::Material}, nullptr);
}

static bool _FCmdListSounds(Request &req)
{
    std::string strType = "all";
    std::vector<ContentKind> kinds;

    if (!req.FArgString("type", &strType, fFalse))
        return fFalse;
    strType = StrLower(strType);
    if (strType == "all" || strType.empty())
        kinds = {ContentKind::SoundEffect, ContentKind::SoundSpeech, ContentKind::SoundMusic};
    else if (strType == "sfx" || strType == "effect" || strType == "effects" || strType == "sound effect")
        kinds = {ContentKind::SoundEffect};
    else if (strType == "speech" || strType == "voice" || strType == "dialogue")
        kinds = {ContentKind::SoundSpeech};
    else if (strType == "music" || strType == "midi")
        kinds = {ContentKind::SoundMusic};
    else
        return req.Fail(kszErrBadArgs, "'type' must be \"sfx\", \"speech\", \"music\" or \"all\"");
    return _FListKinds(req, kinds, "type");
}

static const char *const _rgpszTdtShape[] = {
    "normal", "arch", "circle", "large middle", "arch down", "arch z", "circle z", "vertical", "grow right",
    "grow left",
};

static bool _FCmdList3dWordStyles(Request &req)
{
    Json jsonShapes = Json::MakeArray();
    for (int32_t tdts = 0; tdts < tdtsLim; tdts++)
    {
        Json json = Json::MakeObject();
        json.Set("index", tdts);
        json.Set("name", _rgpszTdtShape[tdts]);
        jsonShapes.Push(json);
    }
    if (!_FListKinds(req, {ContentKind::Font3d}, nullptr))
        return fFalse;
    Json jsonFonts = req.result.Get("items");
    req.result = Json::MakeObject();
    req.result.Set("shapes", jsonShapes);
    req.result.Set("fonts", jsonFonts);
    return fTrue;
}

const CommandInfo krgcmdCatalog[] = {
    {"list_backgrounds", _FCmdListBackgrounds, fFalse},
    {"list_cameras", _FCmdListCameras, fFalse},
    {"list_templates", _FCmdListTemplates, fFalse},
    {"list_actions", _FCmdListActions, fFalse},
    {"list_costumes", _FCmdListCostumes, fFalse},
    {"list_materials", _FCmdListMaterials, fFalse},
    {"list_sounds", _FCmdListSounds, fFalse},
    {"list_3d_word_styles", _FCmdList3dWordStyles, fFalse},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
