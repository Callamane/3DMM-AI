/***************************************************************************
    Automation bridge: non-blocking line server (BSD sockets / Winsock).
***************************************************************************/
#include "linesrv.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int socklen_t;
#define kSockInvalid INVALID_SOCKET
static int _LastSockError()
{
    return WSAGetLastError();
}
static bool _FWouldBlock(int err)
{
    return err == WSAEWOULDBLOCK;
}
static void _CloseSock(socket_t sock)
{
    closesocket(sock);
}
static bool _FSetNonBlocking(socket_t sock)
{
    u_long fNonBlocking = 1;
    return ioctlsocket(sock, FIONBIO, &fNonBlocking) == 0;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define kSockInvalid (-1)
static int _LastSockError()
{
    return errno;
}
static bool _FWouldBlock(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}
static void _CloseSock(socket_t sock)
{
    close(sock);
}
static bool _FSetNonBlocking(socket_t sock)
{
    int grf = fcntl(sock, F_GETFL, 0);
    return grf >= 0 && fcntl(sock, F_SETFL, grf | O_NONBLOCK) == 0;
}
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace automation
{

static socket_t _Sock(intptr_t sock)
{
    return static_cast<socket_t>(sock);
}

LineServer::~LineServer()
{
    Stop();
}

bool LineServer::FStart(uint16_t port, std::string *pstrError)
{
    Stop();

#ifdef _WIN32
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
    {
        *pstrError = "WSAStartup failed";
        return false;
    }
#endif

    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kSockInvalid)
    {
        *pstrError = "socket() failed: " + std::to_string(_LastSockError());
        return false;
    }

    int fReuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&fReuse), sizeof(fReuse));
#ifdef SO_NOSIGPIPE
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &fReuse, sizeof(fReuse));
#endif

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Loopback only: the bridge must never be reachable from the network
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        *pstrError = "cannot bind 127.0.0.1:" + std::to_string(port) + " (error " +
                     std::to_string(_LastSockError()) + "; is another 3DMMEx already listening?)";
        _CloseSock(sock);
        return false;
    }
    if (listen(sock, 1) != 0 || !_FSetNonBlocking(sock))
    {
        *pstrError = "listen() failed: " + std::to_string(_LastSockError());
        _CloseSock(sock);
        return false;
    }

    socklen_t cbAddr = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &cbAddr) == 0)
        _port = ntohs(addr.sin_port);
    else
        _port = port;

    _sockListen = static_cast<intptr_t>(sock);
    return true;
}

void LineServer::Stop()
{
    _CloseClient();
    if (_sockListen != -1)
    {
        _CloseSock(_Sock(_sockListen));
        _sockListen = -1;
    }
}

bool LineServer::FListening() const
{
    return _sockListen != -1;
}

bool LineServer::FClientConnected() const
{
    return _sockClient != -1;
}

void LineServer::_CloseClient()
{
    if (_sockClient != -1)
    {
        _CloseSock(_Sock(_sockClient));
        _sockClient = -1;
    }
    _strIn.clear();
    _strOut.clear();
}

bool LineServer::Poll(std::vector<std::string> *plines)
{
    bool fNewClient = false;

    if (_sockListen == -1)
        return false;

    // Accept new connections. Only one client at a time: extras get an
    // error line and are closed.
    while (true)
    {
        socket_t sock = accept(_Sock(_sockListen), nullptr, nullptr);
        if (sock == kSockInvalid)
            break;
        if (_sockClient != -1)
        {
            static const char kszBusy[] =
                "{\"id\":null,\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"another automation client is "
                "already connected\"}}\n";
            send(sock, kszBusy, sizeof(kszBusy) - 1, MSG_NOSIGNAL);
            _CloseSock(sock);
            continue;
        }
        _FSetNonBlocking(sock);
        int fNoDelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&fNoDelay), sizeof(fNoDelay));
#ifdef SO_NOSIGPIPE
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &fNoDelay, sizeof(fNoDelay));
#endif
        _sockClient = static_cast<intptr_t>(sock);
        _strIn.clear();
        _strOut.clear();
        fNewClient = true;
    }

    if (_sockClient == -1)
        return fNewClient;

    Flush();
    if (_sockClient == -1)
        return fNewClient;

    char rgb[8192];
    while (true)
    {
        int cb = static_cast<int>(recv(_Sock(_sockClient), rgb, sizeof(rgb), 0));
        if (cb > 0)
        {
            _strIn.append(rgb, cb);
            if (_strIn.size() > kcbLineMax)
            {
                _CloseClient();
                return fNewClient;
            }
            continue;
        }
        if (cb == 0 || !_FWouldBlock(_LastSockError()))
        {
            // Peer closed or hard error. Deliver complete lines first.
            size_t ich;
            while ((ich = _strIn.find('\n')) != std::string::npos)
            {
                plines->push_back(_strIn.substr(0, ich));
                _strIn.erase(0, ich + 1);
            }
            _CloseClient();
            return fNewClient;
        }
        break;
    }

    size_t ich;
    while ((ich = _strIn.find('\n')) != std::string::npos)
    {
        std::string strLine = _strIn.substr(0, ich);
        if (!strLine.empty() && strLine.back() == '\r')
            strLine.pop_back();
        plines->push_back(std::move(strLine));
        _strIn.erase(0, ich + 1);
    }
    return fNewClient;
}

void LineServer::SendLine(const std::string &strLine)
{
    if (_sockClient == -1)
        return;
    _strOut.append(strLine);
    _strOut.push_back('\n');
    Flush();
}

void LineServer::Flush()
{
    while (_sockClient != -1 && !_strOut.empty())
    {
        int cb = static_cast<int>(send(_Sock(_sockClient), _strOut.data(), static_cast<int>(_strOut.size()),
                                       MSG_NOSIGNAL));
        if (cb > 0)
        {
            _strOut.erase(0, cb);
            continue;
        }
        if (cb < 0 && _FWouldBlock(_LastSockError()))
            break;
        _CloseClient();
    }
}

} // namespace automation
