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
int sendto_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address, bool nonblocking);
int recvfrom_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms, bool nonblocking);
int read_socket(Socket* socket, uint64_t user_buffer, size_t count, bool nonblocking);
int read_socket_bytes(Socket* socket, uint8_t* output, size_t count, bool nonblocking);
int write_socket(Socket* socket, uint64_t user_buffer, size_t count, bool nonblocking);
int set_socket_option(Socket* socket, uint32_t option, uint64_t value);
uint32_t socket_recv_timeout_ms(const Socket* socket);
bool socket_can_read(const Socket* socket);
bool socket_can_write(const Socket* socket);
bool socket_has_hangup(const Socket* socket);

} // namespace net
