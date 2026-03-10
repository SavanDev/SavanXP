#pragma once

#include <stddef.h>
#include <stdint.h>

namespace process {
struct Process;
}

namespace net {

struct Socket;

void initialize();
bool ready();
bool present();
void poll();
int create_socket(uint32_t domain, uint32_t type, uint32_t protocol, Socket*& out_socket);
void close_socket(Socket* socket);
int bind_socket(Socket* socket, uint64_t user_address);
int connect_socket(Socket* socket, uint64_t user_address, uint32_t timeout_ms);
int sendto_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address);
int recvfrom_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms);
int read_socket(Socket* socket, uint64_t user_buffer, size_t count);
int write_socket(Socket* socket, uint64_t user_buffer, size_t count);

} // namespace net
