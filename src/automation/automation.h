/***************************************************************************
    Automation bridge: hooks called from the studio app (utest.cpp).

    The bridge is a local JSON-lines command server that drives the movie
    engine directly. It is compiled in when the CMake option 3DMM_AUTOMATION
    is ON (which defines AUTOMATION) and is inactive at runtime unless enabled
    with --automation[=PORT] or the THREEDMM_AUTOMATION environment variable.
    See doc/automation/NOTES.md.
***************************************************************************/
#ifndef AUTOMATION_H
#define AUTOMATION_H

// Returns fTrue if psz is an automation command-line option (and consumes it)
bool FAutomationParseArg(PCSZ psz);

// Reads THREEDMM_AUTOMATION from the environment
void AutomationReadEnv(void);

// Whether automation was requested for this session
bool FAutomationEnabled(void);

// Starts listening. Called once the app has initialized.
void AutomationStart(void);

// Services the socket and runs queued commands. Called from APP::FCmdIdle.
void AutomationPoll(void);

// Closes the socket.
void AutomationShutdown(void);

#endif // AUTOMATION_H
