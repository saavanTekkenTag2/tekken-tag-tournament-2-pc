// Android: replaces community_tls.cpp (TLS over Windows SChannel). The first Android builds
// play offline; the community server's TLS control connection reports itself unavailable, so
// the online menus fail cleanly instead of hanging. SHA-256 is real (used for pinning ids).
#include "community_tls.h"

extern "C" void Ttt2Sha256(const void* data, size_t size, uint8_t out[32]);   // win32_core.cpp

namespace community {

TlsStream::~TlsStream() { Close(); }

bool TlsStream::Connect(const std::string& host, uint16_t port, int, const std::function<bool(const uint8_t (&)[32])>&, std::string& error)
{
    error = "TLS is not available in this Android build (community server " + host + ":" + std::to_string(port) + ")";
    return false;
}
bool TlsStream::Send(const void*, size_t) { return false; }
int TlsStream::Recv(void*, size_t) { return -1; }
void TlsStream::Close()
{
    if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
}
void TlsStream::Shutdown()
{
    if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
}
bool TlsStream::PeerAddress(sockaddr_storage&, int&) const { return false; }
bool TlsStream::Handshake(const std::string&, std::string& error) { error = "unavailable"; return false; }
bool TlsStream::SendRaw(const void*, size_t) { return false; }
bool TlsStream::RecvSome(std::vector<uint8_t>&) { return false; }

bool Sha256(const void* data, size_t size, uint8_t (&out)[32])
{
    Ttt2Sha256(data, size, out);
    return true;
}

} // namespace community
