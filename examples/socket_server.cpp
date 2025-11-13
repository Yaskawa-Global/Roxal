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

// Simple TCP server that accepts a single connection, reads a length-prefixed
// binary message, and sends back a short acknowledgement string.  Build it with:
//   g++ socket_server.cpp -o socket_server
// and run it before launching the client.  For a real-world program you can
// wrap the accept loop to handle multiple clients.

namespace {

// Helper function that keeps reading until the buffer is filled or an error occurs.
bool recvAll(int socket_fd, void *buffer, std::size_t total_bytes) {
    std::size_t bytes_read = 0;
    auto *buf = static_cast<std::uint8_t *>(buffer);

    while (bytes_read < total_bytes) {
        ssize_t result = ::recv(socket_fd, buf + bytes_read, total_bytes - bytes_read, 0);
        if (result <= 0) {
            return false;  // Connection closed or error.
        }
        bytes_read += static_cast<std::size_t>(result);
    }
    return true;
}

// Helper function that keeps writing until all bytes have been sent.
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

    // Create the listening socket.
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::perror("socket");
        return 1;
    }

    // Allow immediate reuse of the port after restarting the server.
    int enable = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
        std::perror("setsockopt");
        ::close(server_fd);
        return 1;
    }

    // Bind the socket to listen on all interfaces.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kPort);

    if (::bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        std::perror("bind");
        ::close(server_fd);
        return 1;
    }

    if (::listen(server_fd, SOMAXCONN) == -1) {
        std::perror("listen");
        ::close(server_fd);
        return 1;
    }

    std::cout << "Server listening on port " << kPort << "...\n";

    // Accept a single client connection for simplicity. Loop here if multiple clients are needed.
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd == -1) {
        std::perror("accept");
        ::close(server_fd);
        return 1;
    }

    std::cout << "Client connected. Waiting for message...\n";

    // Read the 32-bit length prefix (network byte order).
    std::uint32_t network_size = 0;
    if (!recvAll(client_fd, &network_size, sizeof(network_size))) {
        std::cerr << "Failed to read message length.\n";
        ::close(client_fd);
        ::close(server_fd);
        return 1;
    }

    std::uint32_t payload_size = ntohl(network_size);
    std::vector<std::uint8_t> payload(payload_size);

    if (payload_size > 0) {
        if (!recvAll(client_fd, payload.data(), payload.size())) {
            std::cerr << "Failed to read message payload.\n";
            ::close(client_fd);
            ::close(server_fd);
            return 1;
        }
    }

    std::cout << "Received payload of " << payload_size << " bytes." << std::endl;

    // For demonstration we send back a simple response message.
    std::string response_text = "Message received (" + std::to_string(payload_size) + " bytes)";
    std::uint32_t response_size = htonl(static_cast<std::uint32_t>(response_text.size()));

    if (!sendAll(client_fd, &response_size, sizeof(response_size)) ||
        !sendAll(client_fd, response_text.data(), response_text.size())) {
        std::cerr << "Failed to send response.\n";
    }

    std::cout << "Response sent. Closing connection." << std::endl;

    ::close(client_fd);
    ::close(server_fd);
    return 0;
}
