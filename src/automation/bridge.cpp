/***************************************************************************
    Automation bridge: server state, request dispatch and app hooks.

    Everything here runs on the UI thread from APP::FCmdIdle. Requests are
    processed strictly in order; a request may defer its reply (see
    Request::poll), in which case later requests wait in the queue.
***************************************************************************/
#include "bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "automation.h"
#include "linesrv.h"

ASSERTNAME

namespace automation
{

const char *const kszErrBadArgs = "bad_args";
const char *const kszErrNotFound = "not_found";
const char *const kszErrBadState = "bad_state";
const char *const kszErrEngine = "engine_error";
const char *const kszErrUnknownCmd = "unknown_command";
const char *const kszErrIo = "io_error";

static bool _fEnabled = false;
static uint16_t _port = kportDefault;
static LineServer _server;
static std::deque<std::string> _dqLines;
static Request *_preqPending = nullptr;
static bool _fInPoll = false;

static const CommandInfo *const _rgpcmdTables[] = {krgcmdSession, krgcmdCatalog, krgcmdScene, krgcmdActor,
                                                   krgcmdMedia,   krgcmdObserve, krgcmdExport};

static const CommandInfo *_PcmdFind(const std::string &strCmd)
{
    for (const CommandInfo *pcmdTable : _rgpcmdTables)
    {
        for (const CommandInfo *pcmd = pcmdTable; pcmd->pszName != nullptr; pcmd++)
        {
            if (strCmd == pcmd->pszName)
                return pcmd;
        }
    }
    return nullptr;
}

/***************************************************************************
    Request argument accessors
***************************************************************************/
bool Request::FArgString(const char *pszName, std::string *pstr, bool fRequired)
{
    const Json &json = args.Get(pszName);
    if (json.IsNull())
    {
        if (fRequired)
            return Fail(kszErrBadArgs, std::string("missing required argument '") + pszName + "'");
        return true;
    }
    if (json.IsNumber())
    {
        // Be forgiving: ids are sometimes passed as numbers
        double d = json.AsNumber();
        char rgch[32];
        snprintf(rgch, sizeof(rgch), "%.0f", d);
        *pstr = rgch;
        return true;
    }
    if (!json.IsString())
        return Fail(kszErrBadArgs, std::string("argument '") + pszName + "' must be a string");
    *pstr = json.AsString();
    return true;
}

bool Request::FArgNumber(const char *pszName, double *pd, bool fRequired)
{
    const Json &json = args.Get(pszName);
    if (json.IsNull())
    {
        if (fRequired)
            return Fail(kszErrBadArgs, std::string("missing required argument '") + pszName + "'");
        return true;
    }
    if (json.IsString())
    {
        char *pchEnd = nullptr;
        double d = strtod(json.AsString().c_str(), &pchEnd);
        if (pchEnd != nullptr && *pchEnd == '\0' && !json.AsString().empty())
        {
            *pd = d;
            return true;
        }
    }
    if (!json.IsNumber())
        return Fail(kszErrBadArgs, std::string("argument '") + pszName + "' must be a number");
    *pd = json.AsNumber();
    return true;
}

bool Request::FArgInt(const char *pszName, int32_t *plw, bool fRequired)
{
    double d;
    if (!FHasArg(pszName))
        return FArgNumber(pszName, &d, fRequired);
    if (!FArgNumber(pszName, &d, fRequired))
        return false;
    if (d < -2147483648.0 || d > 2147483647.0)
        return Fail(kszErrBadArgs, std::string("argument '") + pszName + "' is out of range");
    *plw = static_cast<int32_t>(d < 0 ? d - 0.5 : d + 0.5);
    return true;
}

bool Request::FArgBool(const char *pszName, bool *pf, bool fRequired)
{
    const Json &json = args.Get(pszName);
    if (json.IsNull())
    {
        if (fRequired)
            return Fail(kszErrBadArgs, std::string("missing required argument '") + pszName + "'");
        return true;
    }
    if (json.IsNumber())
    {
        *pf = json.AsNumber() != 0;
        return true;
    }
    if (!json.IsBool())
        return Fail(kszErrBadArgs, std::string("argument '") + pszName + "' must be true or false");
    *pf = json.AsBool();
    return true;
}

/***************************************************************************
    Dispatch
***************************************************************************/
static void _SendReply(Request &req)
{
    Json reply = Json::MakeObject();
    reply.Set("id", req.id);
    if (req.Failed())
    {
        Json error = Json::MakeObject();
        error.Set("code", req.errCode);
        error.Set("message", req.errMessage);
        reply.Set("ok", false);
        reply.Set("error", error);
    }
    else
    {
        reply.Set("ok", true);
        reply.Set("result", req.result.IsNull() ? Json::MakeObject() : req.result);
    }
    _server.SendLine(reply.Dump());
}

// Runs the handler. Returns true if the request finished (reply needed).
static bool _FRunRequest(Request &req)
{
    const CommandInfo *pcmd = _PcmdFind(req.cmd);
    if (pcmd == nullptr)
    {
        req.Fail(kszErrUnknownCmd, "unknown command '" + req.cmd + "'");
        return true;
    }

    if (pcmd->fNeedsStudio && PmvieCur() == pvNil)
    {
        req.Fail(kszErrBadState, "the Studio is not open; call goto_studio first");
        return true;
    }

    // Start from a clean error stack so we only report our own errors
    StrDrainEngineErrors();

    bool fOk = pcmd->pfn(req);
    if (!fOk && !req.Failed())
        FFailEngine(req, "'" + req.cmd + "' failed");

    if (fOk && req.poll)
        return false; // deferred

    // Never let our errors reach DisplayErrors(): it would show a modal alert
    std::string strErrors = StrDrainEngineErrors();
    if (fOk && !strErrors.empty() && !req.result.IsNull() && req.result.IsObject())
        req.result.Set("engine_warnings", strErrors);
    return true;
}

static void _ProcessLine(const std::string &strLine)
{
    std::string strError;
    Json jsonReq;
    Request *preq = new Request();
    preq->tsStart = TsCurrent();

    if (strLine.find_first_not_of(" \t\r") == std::string::npos)
    {
        delete preq;
        return;
    }

    if (!Json::FParse(strLine, &jsonReq, &strError))
    {
        preq->Fail(kszErrBadArgs, "invalid JSON: " + strError);
        _SendReply(*preq);
        delete preq;
        return;
    }
    if (!jsonReq.IsObject() || !jsonReq.Get("cmd").IsString())
    {
        preq->id = jsonReq.Get("id");
        preq->Fail(kszErrBadArgs, "request must be an object with a string \"cmd\"");
        _SendReply(*preq);
        delete preq;
        return;
    }

    preq->id = jsonReq.Get("id");
    preq->cmd = jsonReq.Get("cmd").AsString();
    preq->args = jsonReq.Get("args");
    if (preq->args.IsNull())
        preq->args = Json::MakeObject();
    if (!preq->args.IsObject())
    {
        preq->Fail(kszErrBadArgs, "\"args\" must be an object");
        _SendReply(*preq);
        delete preq;
        return;
    }

    if (_FRunRequest(*preq))
    {
        _SendReply(*preq);
        delete preq;
    }
    else
    {
        _preqPending = preq;
    }
}

static void _PollPending(void)
{
    if (_preqPending == nullptr)
        return;

    bool fDone = _preqPending->poll(*_preqPending);
    if (!fDone)
        return;

    StrDrainEngineErrors();
    _SendReply(*_preqPending);
    delete _preqPending;
    _preqPending = nullptr;
}

} // namespace automation

using namespace automation;

/***************************************************************************
    App hooks
***************************************************************************/
static bool _FParsePort(const char *psz, uint16_t *pport)
{
    char *pchEnd = nullptr;
    long lw = strtol(psz, &pchEnd, 10);
    if (pchEnd == psz || *pchEnd != '\0' || lw < 0 || lw > 65535)
        return fFalse;
    *pport = static_cast<uint16_t>(lw);
    return fTrue;
}

bool FAutomationParseArg(PCSZ psz)
{
    static const char kszOpt[] = "--automation";
    U8SZ u8sz;
    STN stn;
    size_t cchOpt = sizeof(kszOpt) - 1;

    stn.SetSz(psz);
    stn.GetUtf8Sz(u8sz);

    if (strncmp(u8sz, kszOpt, cchOpt) != 0)
        return fFalse;
    if (u8sz[cchOpt] == '\0')
    {
        _fEnabled = fTrue;
        return fTrue;
    }
    if (u8sz[cchOpt] == '=' && _FParsePort(u8sz + cchOpt + 1, &_port))
    {
        _fEnabled = fTrue;
        return fTrue;
    }
    fprintf(stderr, "automation: ignoring malformed option '%s' (expected --automation or --automation=PORT)\n", u8sz);
    return fTrue;
}

void AutomationReadEnv(void)
{
    const char *psz = getenv("THREEDMM_AUTOMATION");
    if (psz == nullptr || *psz == '\0' || strcmp(psz, "0") == 0)
        return;
    if (strcmp(psz, "1") == 0)
    {
        _fEnabled = fTrue;
        return;
    }
    if (_FParsePort(psz, &_port))
        _fEnabled = fTrue;
    else
        fprintf(stderr, "automation: ignoring THREEDMM_AUTOMATION='%s' (expected 1 or a port number)\n", psz);
}

bool FAutomationEnabled(void)
{
    return _fEnabled;
}

void AutomationStart(void)
{
    std::string strError;

    if (!_fEnabled || _server.FListening())
        return;
    if (!_server.FStart(_port, &strError))
    {
        fprintf(stderr, "automation: %s\n", strError.c_str());
        return;
    }
    fprintf(stderr, "automation: listening on 127.0.0.1:%u (protocol %d)\n", _server.Port(), kProtocolVersion);
    fflush(stderr);
}

void AutomationPoll(void)
{
    std::vector<std::string> vecLines;

    if (!_fEnabled || _fInPoll)
        return;
    _fInPoll = fTrue;

    if (_server.Poll(&vecLines))
    {
        fprintf(stderr, "automation: client connected\n");
        _dqLines.clear();
    }
    for (std::string &strLine : vecLines)
        _dqLines.push_back(std::move(strLine));

    _PollPending();
    while (_preqPending == nullptr && !_dqLines.empty())
    {
        std::string strLine = std::move(_dqLines.front());
        _dqLines.pop_front();
        _ProcessLine(strLine);
    }
    _server.Flush();

    _fInPoll = fFalse;
}

void AutomationShutdown(void)
{
    if (_preqPending != nullptr)
    {
        delete _preqPending;
        _preqPending = nullptr;
    }
    _server.Stop();
}
