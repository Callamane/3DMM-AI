/***************************************************************************
    Automation bridge: a non-blocking, single-client, newline-delimited TCP
    server bound to 127.0.0.1. Designed to be polled from the UI thread, so
    nothing here ever blocks.
***************************************************************************/
#ifndef AUTOMATION_LINESRV_H
#define AUTOMATION_LINESRV_H

#include <cstdint>
#include <string>
#include <vector>

namespace automation
{

class LineServer
{
  public:
    LineServer() = default;
    ~LineServer();
    LineServer(const LineServer &) = delete;
    LineServer &operator=(const LineServer &) = delete;

    // Starts listening on 127.0.0.1:port. port 0 picks a free port.
    bool FStart(uint16_t port, std::string *pstrError);
    void Stop();
    bool FListening() const;
    uint16_t Port() const
    {
        return _port;
    }
    bool FClientConnected() const;

    // Accepts connections and reads available data. Complete lines are
    // appended to *plines (without the newline). Returns true if a new client
    // connected during this call.
    bool Poll(std::vector<std::string> *plines);

    // Queues a line (a newline is appended) and flushes as much as possible.
    void SendLine(const std::string &strLine);
    // Writes pending output; call from Poll as well.
    void Flush();

    // Maximum accepted line length; longer lines drop the connection.
    static const size_t kcbLineMax = 16 * 1024 * 1024;

  private:
    intptr_t _sockListen = -1;
    intptr_t _sockClient = -1;
    uint16_t _port = 0;
    std::string _strIn;
    std::string _strOut;

    void _CloseClient();
};

} // namespace automation

#endif // AUTOMATION_LINESRV_H
