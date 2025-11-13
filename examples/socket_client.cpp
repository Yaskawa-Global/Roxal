#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Companion TCP client for socket_server.cpp.  It connects to the server,
// sends a length-prefixed payload, and prints the server's response.
// Build with:
//   g++ socket_client.cpp -o socket_client

namespace {

bool recvAll(int socket_fd, void *buffer, std::size_t total_bytes) {
    std::size_t bytes_read = 0;
    auto *buf = static_cast<std::uint8_t *>(buffer);

    while (bytes_read < total_bytes) {
        ssize_t result = ::recv(socket_fd, buf + bytes_read, total_bytes - bytes_read, 0);
        if (result <= 0) {
            return false;
        }
        bytes_read += static_cast<std::size_t>(result);
    }
    return true;
}

bool sendAll(int socket_fd, const void *buffer, std::size_t total_bytes) {
    std::size_t bytes_sent = 0;
    auto *buf = static_cast<const std::uint8_t *>(buffer);

    while (bytes_sent < total_bytes) {
        ssize_t result = ::send(socket_fd, buf + bytes_sent, total_bytes - bytes_sent, 0);
        if (result < 0) {
            return false;
        }
        bytes_sent += static_cast<std::size_t>(result);
    }
    return true;
}

}  // namespace

int main() {
    constexpr std::uint16_t kPort = 54000;
    const char *kServerAddress = "127.0.0.1";  // Replace with server IP if needed.

    // Example payload. Replace with the actual binary blob when integrating.
    const std::string payload = "Example opaque payload";

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(kPort);
    if (::inet_pton(AF_INET, kServerAddress, &server_addr.sin_addr) != 1) {
        std::perror("inet_pton");
        ::close(client_fd);
        return 1;
    }

    if (::connect(client_fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) == -1) {
        std::perror("connect");
        ::close(client_fd);
        return 1;
    }

    std::cout << "Connected to server. Sending payload...\n";

    // Send length-prefixed payload.
    std::uint32_t network_size = htonl(static_cast<std::uint32_t>(payload.size()));
    if (!sendAll(client_fd, &network_size, sizeof(network_size)) ||
        !sendAll(client_fd, payload.data(), payload.size())) {
        std::cerr << "Failed to send payload.\n";
        ::close(client_fd);
        return 1;
    }

    // Wait for a response from the server.
    std::uint32_t response_size_network = 0;
    if (!recvAll(client_fd, &response_size_network, sizeof(response_size_network))) {
        std::cerr << "Failed to read response length.\n";
        ::close(client_fd);
        return 1;
    }

    std::uint32_t response_size = ntohl(response_size_network);
    std::vector<char> response(response_size);
    if (response_size > 0) {
        if (!recvAll(client_fd, response.data(), response.size())) {
            std::cerr << "Failed to read response body.\n";
            ::close(client_fd);
            return 1;
        }
    }

    std::cout << "Received response: " << std::string(response.begin(), response.end()) << std::endl;

    ::close(client_fd);
    return 0;
}
