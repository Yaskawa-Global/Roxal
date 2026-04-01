#pragma once

#ifdef ROXAL_COMPUTE_SERVER

#include <cstdint>

namespace roxal {

class ComputeServer {
public:
    void listen(std::uint16_t port);

private:
    void handleClient(int clientFd);
};

} // namespace roxal

#endif // ROXAL_COMPUTE_SERVER
