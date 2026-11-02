/***************************************************************************
    Automation bridge: internal interfaces shared by the command modules.
***************************************************************************/
#ifndef AUTOMATION_BRIDGE_H
#define AUTOMATION_BRIDGE_H

#include "studio.h"

#include <functional>
#include <string>
#include <vector>

#include "json.h"
#include "stage.h"

namespace automation
{

const int32_t kProtocolVersion = 1;
const uint16_t kportDefault = 7341;

// Error codes returned in {"error": {"code": ...}}
extern const char *const kszErrBadArgs;    // missing/invalid argument
extern const char *const kszErrNotFound;   // unknown id/name
extern const char *const kszErrBadState;   // e.g. no movie, no scene, playing
extern const char *const kszErrEngine;     // the engine refused or failed
extern const char *const kszErrUnknownCmd; // no such command
extern const char *const kszErrIo;         // file system error

/***************************************************************************
    A request being processed. Handlers fill result (on success) or call
    Fail(). A handler may set poll to defer its reply: poll is called on
    every idle until it returns true, then the reply is sent.
***************************************************************************/
struct Request
{
    Json id;
    std::string cmd;
    Json args;
    Json result;
    std::string errCode;
    std::string errMessage;
    std::function<bool(Request &)> poll;
    uint32_t tsStart = 0;

    bool Fail(const char *pszCode, const std::string &strMessage)
    {
        errCode = pszCode;
        errMessage = strMessage;
        return false;
    }
    bool Failed() const
    {
        return !errCode.empty();
    }

    // Argument accessors. Each returns false and fills the error when a
    // required argument is missing or has the wrong type.
    bool FArgString(const char *pszName, std::string *pstr, bool fRequired = true);
    bool FArgInt(const char *pszName, int32_t *plw, bool fRequired = true);
    bool FArgNumber(const char *pszName, double *pd, bool fRequired = true);
    bool FArgBool(const char *pszName, bool *pf, bool fRequired = true);
    bool FHasArg(const char *pszName) const
    {
        return args.Has(pszName) && !args.Get(pszName).IsNull();
    }
};

typedef bool (*PFNCMD)(Request &req);

struct CommandInfo
{
    const char *pszName;
    PFNCMD pfn;
    bool fNeedsStudio; // fails with bad_state unless the Studio is up
};

// Command tables, one per module
extern const CommandInfo krgcmdSession[];
extern const CommandInfo krgcmdCatalog[];
extern const CommandInfo krgcmdScene[];
extern const CommandInfo krgcmdActor[];
extern const CommandInfo krgcmdMedia[];
extern const CommandInfo krgcmdObserve[];
extern const CommandInfo krgcmdExport[];

/***************************************************************************
    BRender number conversions. This build of BRender is fixed point
    (BASED_FIXED): BRS is 16.16 fixed, BRA is a 16-bit fraction of a turn.
***************************************************************************/
inline double DFromBrs(BRS brs)
{
    return BrScalarToFloat(brs);
}
inline BRS BrsFromD(double d)
{
    return (BRS)BrFloatToScalar(d);
}
inline double DegFromBra(BRA bra)
{
    return DFromBrs(BrAngleToDegree(bra));
}
inline BRA BraFromDeg(double deg)
{
    deg = deg - 360.0 * static_cast<int64_t>(deg / 360.0);
    if (deg < 0)
        deg += 360.0;
    return BrDegreeToAngle(BrsFromD(deg));
}

/***************************************************************************
    Engine helpers (engine.cpp)
***************************************************************************/
std::string StrFromStn(PSTN pstn);
void StnFromStr(const std::string &str, PSTN pstn);
std::string StrLower(const std::string &str);

PSTDIO PstdioCur(void);
PMVIE PmvieCur(void);
PMVU PmvuCur(void);

bool FRequireMovie(Request &req, PMVIE *ppmvie);
bool FRequireScene(Request &req, PMVIE *ppmvie, PSCEN *ppscen);
bool FRequireNotPlaying(Request &req, PMVIE pmvie);

// Resolves args[pszName] (an arid number or actor name) to an actor in the
// current scene. If the arg is absent, uses the selected actor.
bool FResolveActor(Request &req, const char *pszName, PACTR *ppactr);

// Builds the camera model and stage frame for the current scene/camera.
bool FGetCamera(PMVIE pmvie, CameraModel *pcamera);
bool FGetStageFrame(PMVIE pmvie, StageFrame *pstage, CameraModel *pcamera = pvNil);

// Parses a position argument: {"x","z"[,"y"]} in stage metres or
// {"world":[x,y,z]}. Returns world coordinates.
bool FParsePosition(Request &req, const Json &json, const StageFrame &stage, Vec3 *pworld);

// JSON for a world point: {"stage":{x,y,z},"world":[..],"screen":{..}}
Json JsonFromPoint(const Vec3 &world, const StageFrame &stage, const CameraModel &camera);

// Moves the scene to frame nfrm, failing with a useful message.
bool FGotoFrame(Request &req, PSCEN pscen, int32_t nfrm);
// Applies an optional "frame" argument (goes there before editing)
bool FApplyFrameArg(Request &req, PSCEN pscen);

// Drains the engine's error stack; returns a description ("" if none)
std::string StrDrainEngineErrors(void);
// Fails req with kszErrEngine, appending any engine error codes
bool FFailEngine(Request &req, const std::string &strWhat);

void RefreshViews(PMVIE pmvie);
// Renders the 3D world for the current frame so actor screen bounds are current
void EnsureRendered(PMVIE pmvie);

/***************************************************************************
    Catalog helpers (catalog.cpp)
***************************************************************************/
enum class ContentKind
{
    Background,
    Actor,
    Prop,
    SoundEffect,
    SoundSpeech,
    SoundMusic,
    Font3d,
    Material,
};

struct CatalogEntry
{
    std::string id;   // "<prefix>:<sid>:<cno>"
    std::string name; // display name from the thumbnail file
    TAG tag;          // content tag (sid, ctg, cno)
    ContentKind kind;
};

const std::vector<CatalogEntry> *PvecCatalog(ContentKind kind);
// Finds an entry by id or (case-insensitive) name across the given kinds.
// On failure fills req's error with suggestions.
bool FFindContent(Request &req, const std::string &strRef, const std::vector<ContentKind> &kinds,
                  const CatalogEntry **ppentry);
const char *PszKindName(ContentKind kind);

// Loads a template (actor/prop) by tag. Caller releases.
PTMPL PtmplFetch(PTAG ptag);
// Resolves an action reference (name or index) for a template
bool FResolveAction(Request &req, PTMPL ptmpl, const Json &jsonAction, int32_t *panid);
Json JsonActionList(PTMPL ptmpl);
std::string StrActionName(PTMPL ptmpl, int32_t anid);

/***************************************************************************
    Frame capture (observe.cpp)
***************************************************************************/
// Renders the current frame (3D + text boxes) to a PNG. fOverlay draws the
// stage-coordinate grid and actor labels on top.
bool FRenderToPng(Request &req, const std::string &strPath, int32_t *pdxp, int32_t *pdyp, bool fOverlay);

} // namespace automation

#endif // AUTOMATION_BRIDGE_H
