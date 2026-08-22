#include "kernel/net.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/nic.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "savanxp/syscall.h"

namespace net {

enum class TcpState : uint8_t {
    closed = 0,
    syn_sent = 1,
    established = 2,
    close_wait = 3,
};

struct UdpPacket {
    bool in_use;
    uint32_t source_ip;
    uint16_t source_port;
    uint16_t length;
    uint8_t data[1024];
};

struct Socket {
    bool in_use;
    bool bound;
    uint32_t domain;
    uint32_t type;
    uint32_t protocol;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;
    TcpState tcp_state;
    bool connected;
    bool fin_received;
    uint32_t send_unacked;
    uint32_t send_next;
    uint32_t recv_next;
    uint32_t initial_sequence;
    size_t packet_queue_head;
    size_t packet_queue_size;
    UdpPacket queue[16];
    size_t rx_head;
    size_t rx_size;
    uint8_t rx_buffer[8192];
};

} // namespace net

namespace {

constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kArpHardwareEthernet = 1;
constexpr uint16_t kArpOpRequest = 1;
constexpr uint16_t kArpOpReply = 2;
constexpr uint8_t kIpProtocolIcmp = 1;
constexpr uint8_t kIpProtocolUdp = 17;
constexpr uint8_t kIpProtocolTcp = 6;
constexpr uint16_t kPingIdentifier = 0x5358;
constexpr size_t kMaxSockets = 32;
constexpr uint16_t kEphemeralPortStart = 49152;
constexpr uint16_t kEphemeralPortEnd = 65535;
constexpr uint16_t kTcpWindowSize = 4096;
constexpr uint8_t kTcpFlagFin = 0x01;
constexpr uint8_t kTcpFlagSyn = 0x02;
constexpr uint8_t kTcpFlagRst = 0x04;
constexpr uint8_t kTcpFlagPsh = 0x08;
constexpr uint8_t kTcpFlagAck = 0x10;

struct [[gnu::packed]] EthernetHeader {
    uint8_t destination[6];
    uint8_t source[6];
    uint16_t type;
};

struct [[gnu::packed]] ArpPacket {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_size;
    uint8_t protocol_size;
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
};

struct [[gnu::packed]] Ipv4Header {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t source_ip;
    uint32_t destination_ip;
};

struct [[gnu::packed]] IcmpEchoHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
};

struct [[gnu::packed]] UdpHeader {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
    uint16_t checksum;
};

struct [[gnu::packed]] TcpHeader {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence_number;
    uint32_t acknowledgement_number;
    uint8_t data_offset_reserved;
    uint8_t flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_pointer;
};

device::Device g_device = {
    .name = "net0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

uint8_t g_arp_mac[6] = {};
uint32_t g_arp_ip = 0;
uint32_t g_pending_ping_ip = 0;
uint16_t g_pending_ping_sequence = 0;
savanxp_net_ping_result g_pending_ping_result = {};
uint32_t g_last_status = SAVANXP_NET_STATUS_UNKNOWN;
uint32_t g_arp_requests = 0;
uint32_t g_arp_timeouts = 0;
uint32_t g_ping_requests = 0;
uint32_t g_ping_timeouts = 0;
bool g_up = false;
bool g_ping_complete = false;
bool g_ready = false;
net::Socket g_sockets[kMaxSockets] = {};
uint16_t g_next_ephemeral_port = kEphemeralPortStart;
constexpr bool kLogNet = false;

constexpr uint32_t kConfiguredIpv4 = (10u << 24) | (0u << 16) | (2u << 8) | 15u;
constexpr uint32_t kConfiguredMask = (255u << 24) | (255u << 16) | (255u << 8) | 0u;
constexpr uint32_t kConfiguredGateway = (10u << 24) | (0u << 16) | (2u << 8) | 2u;


int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

void set_status(uint32_t status) {
    g_last_status = status;
}

bool resolve_arp(uint32_t target_ip, uint32_t timeout_ms);

void net_log(const char* format, ...) {
    if (!kLogNet) {
        return;
    }

    va_list args;
    va_start(args, format);
    console::vprintf(format, args);
    va_end(args);
}

uint16_t byteswap16(uint16_t value) {
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

uint32_t byteswap32(uint32_t value) {
    return ((value & 0x000000ffu) << 24) |
        ((value & 0x0000ff00u) << 8) |
        ((value & 0x00ff0000u) >> 8) |
        ((value & 0xff000000u) >> 24);
}

uint16_t host_to_be16(uint16_t value) {
    return byteswap16(value);
}

uint32_t host_to_be32(uint32_t value) {
    return byteswap32(value);
}

uint16_t be16_to_host(uint16_t value) {
    return byteswap16(value);
}

uint32_t be32_to_host(uint32_t value) {
    return byteswap32(value);
}

bool same_subnet(uint32_t left, uint32_t right) {
    return (left & kConfiguredMask) == (right & kConfiguredMask);
}

uint16_t compute_checksum(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;

    for (size_t index = 0; index + 1 < length; index += 2) {
        sum += (static_cast<uint32_t>(bytes[index]) << 8) | bytes[index + 1];
    }
    if ((length & 1u) != 0) {
        sum += static_cast<uint32_t>(bytes[length - 1]) << 8;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

uint16_t compute_transport_checksum(uint32_t source_ip, uint32_t destination_ip, uint8_t protocol, const void* data, size_t length) {
    uint32_t sum = 0;
    const uint8_t pseudo_header[12] = {
        static_cast<uint8_t>((source_ip >> 24) & 0xffu),
        static_cast<uint8_t>((source_ip >> 16) & 0xffu),
        static_cast<uint8_t>((source_ip >> 8) & 0xffu),
        static_cast<uint8_t>(source_ip & 0xffu),
        static_cast<uint8_t>((destination_ip >> 24) & 0xffu),
        static_cast<uint8_t>((destination_ip >> 16) & 0xffu),
        static_cast<uint8_t>((destination_ip >> 8) & 0xffu),
        static_cast<uint8_t>(destination_ip & 0xffu),
        0,
        protocol,
        static_cast<uint8_t>((length >> 8) & 0xffu),
        static_cast<uint8_t>(length & 0xffu),
    };

    for (size_t index = 0; index < sizeof(pseudo_header); index += 2) {
        sum += (static_cast<uint32_t>(pseudo_header[index]) << 8) | pseudo_header[index + 1];
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index + 1 < length; index += 2) {
        sum += (static_cast<uint32_t>(bytes[index]) << 8) | bytes[index + 1];
    }
    if ((length & 1u) != 0) {
        sum += static_cast<uint32_t>(bytes[length - 1]) << 8;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    {
        const uint16_t checksum = static_cast<uint16_t>(~sum);
        return checksum != 0 ? checksum : 0xffffu;
    }
}

void wait_for_tick() {
    arch::x86_64::enable_interrupts();
    arch::x86_64::halt_once();
    arch::x86_64::disable_interrupts();
}


bool send_arp(uint16_t operation, const uint8_t* target_mac, uint32_t sender_ip, uint32_t target_ip) {
    uint8_t frame[64] = {};
    auto* ethernet = reinterpret_cast<EthernetHeader*>(frame);
    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthernetHeader));

    if (operation == kArpOpReply && target_mac != nullptr) {
        memcpy(ethernet->destination, target_mac, 6);
        memcpy(arp->target_mac, target_mac, 6);
    } else {
        memset(ethernet->destination, 0xff, 6);
        memset(arp->target_mac, 0x00, 6);
    }

    memcpy(ethernet->source, nic::mac_address(), 6);
    ethernet->type = host_to_be16(kEtherTypeArp);

    arp->hardware_type = host_to_be16(kArpHardwareEthernet);
    arp->protocol_type = host_to_be16(kEtherTypeIpv4);
    arp->hardware_size = 6;
    arp->protocol_size = 4;
    arp->operation = host_to_be16(operation);
    memcpy(arp->sender_mac, nic::mac_address(), 6);
    arp->sender_ip = host_to_be32(sender_ip);
    arp->target_ip = host_to_be32(target_ip);

    if (operation == kArpOpRequest) {
        ++g_arp_requests;
        set_status(SAVANXP_NET_STATUS_ARP_RESOLVING);
    }

    return nic::transmit(frame, sizeof(EthernetHeader) + sizeof(ArpPacket));
}

bool send_icmp_echo(uint32_t destination_ip, const uint8_t* destination_mac, uint16_t sequence, uint16_t payload_size) {
    if (payload_size == 0 || payload_size > 512) {
        payload_size = 32;
    }

    uint8_t frame[14 + 20 + 8 + 512] = {};
    auto* ethernet = reinterpret_cast<EthernetHeader*>(frame);
    auto* ipv4 = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* icmp = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    uint8_t* payload = frame + sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader);

    memcpy(ethernet->destination, destination_mac, 6);
    memcpy(ethernet->source, nic::mac_address(), 6);
    ethernet->type = host_to_be16(kEtherTypeIpv4);

    ipv4->version_ihl = 0x45;
    ipv4->dscp_ecn = 0;
    ipv4->total_length = host_to_be16(static_cast<uint16_t>(sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payload_size));
    ipv4->identification = host_to_be16(sequence);
    ipv4->flags_fragment = host_to_be16(0x4000);
    ipv4->ttl = 64;
    ipv4->protocol = kIpProtocolIcmp;
    ipv4->checksum = 0;
    ipv4->source_ip = host_to_be32(kConfiguredIpv4);
    ipv4->destination_ip = host_to_be32(destination_ip);

    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = host_to_be16(kPingIdentifier);
    icmp->sequence = host_to_be16(sequence);

    for (uint16_t index = 0; index < payload_size; ++index) {
        payload[index] = static_cast<uint8_t>('A' + (index % 26));
    }

    icmp->checksum = host_to_be16(compute_checksum(icmp, sizeof(IcmpEchoHeader) + payload_size));
    ipv4->checksum = host_to_be16(compute_checksum(ipv4, sizeof(Ipv4Header)));
    set_status(SAVANXP_NET_STATUS_ICMP_SENT);

    return nic::transmit(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payload_size);
}

bool send_icmp_reply(
    const EthernetHeader& ethernet,
    const Ipv4Header& ipv4,
    const IcmpEchoHeader& icmp,
    const uint8_t* payload,
    size_t payload_size
) {
    uint8_t frame[14 + 20 + 8 + 512] = {};
    if (payload_size > 512) {
        return false;
    }

    auto* out_eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* out_ipv4 = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* out_icmp = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    uint8_t* out_payload = frame + sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader);

    memcpy(out_eth->destination, ethernet.source, 6);
    memcpy(out_eth->source, nic::mac_address(), 6);
    out_eth->type = host_to_be16(kEtherTypeIpv4);

    *out_ipv4 = ipv4;
    out_ipv4->source_ip = host_to_be32(kConfiguredIpv4);
    out_ipv4->destination_ip = ipv4.source_ip;
    out_ipv4->ttl = 64;
    out_ipv4->checksum = 0;

    *out_icmp = icmp;
    out_icmp->type = 0;
    out_icmp->checksum = 0;
    memcpy(out_payload, payload, payload_size);

    out_icmp->checksum = host_to_be16(compute_checksum(out_icmp, sizeof(IcmpEchoHeader) + payload_size));
    out_ipv4->checksum = host_to_be16(compute_checksum(out_ipv4, sizeof(Ipv4Header)));

    return nic::transmit(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payload_size);
}

void remember_arp(uint32_t ip, const uint8_t* mac) {
    g_arp_ip = ip;
    memcpy(g_arp_mac, mac, 6);
    set_status(SAVANXP_NET_STATUS_ARP_RESOLVED);
}

net::Socket* find_bound_socket(uint16_t port) {
    for (auto& socket : g_sockets) {
        if (socket.in_use && socket.bound && socket.local_port == port) {
            return &socket;
        }
    }
    return nullptr;
}

uint16_t allocate_ephemeral_port() {
    for (uint32_t attempt = 0; attempt <= (uint32_t)(kEphemeralPortEnd - kEphemeralPortStart); ++attempt) {
        const uint16_t port = g_next_ephemeral_port;
        g_next_ephemeral_port = static_cast<uint16_t>(port >= kEphemeralPortEnd ? kEphemeralPortStart : (port + 1));
        if (find_bound_socket(port) == nullptr) {
            return port;
        }
    }
    return 0;
}

int bind_socket_port(net::Socket* socket, uint16_t port) {
    if (socket == nullptr || !socket->in_use) {
        return negative_error(SAVANXP_EBADF);
    }
    if (!((socket->type == SAVANXP_SOCK_DGRAM && socket->protocol == SAVANXP_IPPROTO_UDP) ||
        (socket->type == SAVANXP_SOCK_STREAM && socket->protocol == SAVANXP_IPPROTO_TCP))) {
        return negative_error(SAVANXP_EBADF);
    }

    if (socket->bound) {
        return negative_error(SAVANXP_EBUSY);
    }

    if (port == 0) {
        port = allocate_ephemeral_port();
        if (port == 0) {
            return negative_error(SAVANXP_ENOSPC);
        }
    } else if (find_bound_socket(port) != nullptr) {
        return negative_error(SAVANXP_EBUSY);
    }

    socket->bound = true;
    socket->local_port = port;
    return 0;
}

int ensure_socket_bound(net::Socket* socket) {
    if (socket == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    if (socket->bound) {
        return 0;
    }
    return bind_socket_port(socket, 0);
}

bool enqueue_udp_packet(net::Socket& socket, uint32_t source_ip, uint16_t source_port, const uint8_t* payload, size_t length) {
    if (socket.packet_queue_size >= (sizeof(socket.queue) / sizeof(socket.queue[0]))) {
        return false;
    }

    if (length > sizeof(socket.queue[0].data)) {
        length = sizeof(socket.queue[0].data);
    }

    net::UdpPacket& packet = socket.queue[(socket.packet_queue_head + socket.packet_queue_size) % (sizeof(socket.queue) / sizeof(socket.queue[0]))];
    memset(&packet, 0, sizeof(packet));
    packet.in_use = true;
    packet.source_ip = source_ip;
    packet.source_port = source_port;
    packet.length = static_cast<uint16_t>(length);
    memcpy(packet.data, payload, length);
    socket.packet_queue_size += 1;
    return true;
}

bool dequeue_udp_packet(net::Socket& socket, net::UdpPacket& packet) {
    if (socket.packet_queue_size == 0) {
        return false;
    }

    packet = socket.queue[socket.packet_queue_head];
    memset(&socket.queue[socket.packet_queue_head], 0, sizeof(socket.queue[socket.packet_queue_head]));
    socket.packet_queue_head = (socket.packet_queue_head + 1) % (sizeof(socket.queue) / sizeof(socket.queue[0]));
    socket.packet_queue_size -= 1;
    return true;
}

bool deliver_udp_payload(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip, uint16_t destination_port, const uint8_t* payload, size_t length) {
    if (destination_ip != kConfiguredIpv4) {
        return false;
    }

    net::Socket* socket = find_bound_socket(destination_port);
    if (socket == nullptr) {
        return false;
    }

    return enqueue_udp_packet(*socket, source_ip, source_port, payload, length);
}

size_t tcp_rx_free(const net::Socket& socket) {
    return sizeof(socket.rx_buffer) - socket.rx_size;
}

bool enqueue_tcp_bytes(net::Socket& socket, const uint8_t* payload, size_t length) {
    if (payload == nullptr || length > tcp_rx_free(socket)) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        socket.rx_buffer[(socket.rx_head + socket.rx_size + index) % sizeof(socket.rx_buffer)] = payload[index];
    }
    socket.rx_size += length;
    return true;
}

size_t dequeue_tcp_bytes(net::Socket& socket, uint8_t* output, size_t length) {
    size_t count = length < socket.rx_size ? length : socket.rx_size;
    if (output == nullptr) {
        return 0;
    }
    for (size_t index = 0; index < count; ++index) {
        output[index] = socket.rx_buffer[(socket.rx_head + index) % sizeof(socket.rx_buffer)];
    }
    socket.rx_head = (socket.rx_head + count) % sizeof(socket.rx_buffer);
    socket.rx_size -= count;
    return count;
}

net::Socket* find_tcp_socket(uint16_t local_port, uint16_t remote_port, uint32_t remote_ip) {
    for (auto& socket : g_sockets) {
        if (!socket.in_use || socket.type != SAVANXP_SOCK_STREAM || socket.protocol != SAVANXP_IPPROTO_TCP) {
            continue;
        }
        if (!socket.bound || socket.local_port != local_port) {
            continue;
        }
        if (socket.connected) {
            if (socket.remote_port == remote_port && socket.remote_ip == remote_ip) {
                return &socket;
            }
        } else if (socket.tcp_state == net::TcpState::syn_sent && socket.remote_port == remote_port && socket.remote_ip == remote_ip) {
            return &socket;
        }
    }
    return nullptr;
}

net::Socket* find_pending_tcp_socket(uint16_t local_port, uint16_t remote_port) {
    for (auto& socket : g_sockets) {
        if (!socket.in_use || socket.type != SAVANXP_SOCK_STREAM || socket.protocol != SAVANXP_IPPROTO_TCP) {
            continue;
        }
        if (!socket.bound || socket.local_port != local_port) {
            continue;
        }
        if (socket.tcp_state == net::TcpState::syn_sent && socket.remote_port == remote_port) {
            return &socket;
        }
    }
    return nullptr;
}

bool send_tcp_segment(
    net::Socket& socket,
    uint8_t flags,
    const uint8_t* payload,
    size_t payload_length,
    uint32_t sequence_number,
    uint32_t acknowledgement_number
) {
    if (payload_length > 1024) {
        return false;
    }

    const uint32_t destination_ip = socket.remote_ip;
    const uint32_t next_hop = same_subnet(kConfiguredIpv4, destination_ip) ? destination_ip : kConfiguredGateway;
    if (!resolve_arp(next_hop, 1000u)) {
        return false;
    }

    uint8_t frame[14 + 20 + 20 + 1024] = {};
    auto* ethernet = reinterpret_cast<EthernetHeader*>(frame);
    auto* ipv4 = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* tcp = reinterpret_cast<TcpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    uint8_t* out_payload = frame + sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(TcpHeader);

    memcpy(ethernet->destination, g_arp_mac, 6);
    memcpy(ethernet->source, nic::mac_address(), 6);
    ethernet->type = host_to_be16(kEtherTypeIpv4);

    ipv4->version_ihl = 0x45;
    ipv4->dscp_ecn = 0;
    ipv4->total_length = host_to_be16(static_cast<uint16_t>(sizeof(Ipv4Header) + sizeof(TcpHeader) + payload_length));
    ipv4->identification = 0;
    ipv4->flags_fragment = host_to_be16(0x4000);
    ipv4->ttl = 64;
    ipv4->protocol = kIpProtocolTcp;
    ipv4->checksum = 0;
    ipv4->source_ip = host_to_be32(kConfiguredIpv4);
    ipv4->destination_ip = host_to_be32(destination_ip);

    tcp->source_port = host_to_be16(socket.local_port);
    tcp->destination_port = host_to_be16(socket.remote_port);
    tcp->sequence_number = host_to_be32(sequence_number);
    tcp->acknowledgement_number = host_to_be32(acknowledgement_number);
    tcp->data_offset_reserved = static_cast<uint8_t>(5u << 4);
    tcp->flags = flags;
    tcp->window_size = host_to_be16(kTcpWindowSize);
    tcp->checksum = 0;
    tcp->urgent_pointer = 0;

    if (payload_length != 0) {
        memcpy(out_payload, payload, payload_length);
    }

    tcp->checksum = host_to_be16(compute_transport_checksum(kConfiguredIpv4, destination_ip, kIpProtocolTcp, tcp, sizeof(TcpHeader) + payload_length));
    ipv4->checksum = host_to_be16(compute_checksum(ipv4, sizeof(Ipv4Header)));
    return nic::transmit(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(TcpHeader) + payload_length);
}

bool send_udp_datagram(
    uint32_t destination_ip,
    const uint8_t* destination_mac,
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* payload,
    size_t payload_length
) {
    if (payload == nullptr || payload_length > 1024 || destination_mac == nullptr) {
        return false;
    }

    uint8_t frame[14 + 20 + 8 + 1024] = {};
    auto* ethernet = reinterpret_cast<EthernetHeader*>(frame);
    auto* ipv4 = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* udp = reinterpret_cast<UdpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    uint8_t* out_payload = frame + sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(UdpHeader);

    memcpy(ethernet->destination, destination_mac, 6);
    memcpy(ethernet->source, nic::mac_address(), 6);
    ethernet->type = host_to_be16(kEtherTypeIpv4);

    ipv4->version_ihl = 0x45;
    ipv4->dscp_ecn = 0;
    ipv4->total_length = host_to_be16(static_cast<uint16_t>(sizeof(Ipv4Header) + sizeof(UdpHeader) + payload_length));
    ipv4->identification = 0;
    ipv4->flags_fragment = host_to_be16(0x4000);
    ipv4->ttl = 64;
    ipv4->protocol = kIpProtocolUdp;
    ipv4->checksum = 0;
    ipv4->source_ip = host_to_be32(kConfiguredIpv4);
    ipv4->destination_ip = host_to_be32(destination_ip);

    udp->source_port = host_to_be16(source_port);
    udp->destination_port = host_to_be16(destination_port);
    udp->length = host_to_be16(static_cast<uint16_t>(sizeof(UdpHeader) + payload_length));
    udp->checksum = 0;

    memcpy(out_payload, payload, payload_length);
    ipv4->checksum = host_to_be16(compute_checksum(ipv4, sizeof(Ipv4Header)));
    return nic::transmit(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(UdpHeader) + payload_length);
}

void handle_arp(const EthernetHeader&, const uint8_t* payload, size_t length) {
    if (length < sizeof(ArpPacket)) {
        return;
    }

    const auto* arp = reinterpret_cast<const ArpPacket*>(payload);
    if (be16_to_host(arp->hardware_type) != kArpHardwareEthernet ||
        be16_to_host(arp->protocol_type) != kEtherTypeIpv4 ||
        arp->hardware_size != 6 ||
        arp->protocol_size != 4) {
        return;
    }

    const uint32_t sender_ip = be32_to_host(arp->sender_ip);
    const uint32_t target_ip = be32_to_host(arp->target_ip);
    net_log(
        "net: arp op=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u\n",
        (unsigned)be16_to_host(arp->operation),
        (unsigned)((sender_ip >> 24) & 0xffu),
        (unsigned)((sender_ip >> 16) & 0xffu),
        (unsigned)((sender_ip >> 8) & 0xffu),
        (unsigned)(sender_ip & 0xffu),
        (unsigned)((target_ip >> 24) & 0xffu),
        (unsigned)((target_ip >> 16) & 0xffu),
        (unsigned)((target_ip >> 8) & 0xffu),
        (unsigned)(target_ip & 0xffu)
    );
    remember_arp(sender_ip, arp->sender_mac);

    if (be16_to_host(arp->operation) == kArpOpRequest && target_ip == kConfiguredIpv4) {
        (void)send_arp(kArpOpReply, arp->sender_mac, kConfiguredIpv4, sender_ip);
    }
}

void handle_icmp(const EthernetHeader& ethernet, const Ipv4Header& ipv4, const uint8_t* payload, size_t length) {
    if (length < sizeof(IcmpEchoHeader)) {
        return;
    }

    const auto* icmp = reinterpret_cast<const IcmpEchoHeader*>(payload);
    const uint16_t identifier = be16_to_host(icmp->identifier);
    const uint16_t sequence = be16_to_host(icmp->sequence);
    const size_t payload_size = length - sizeof(IcmpEchoHeader);
    const uint32_t source_ip = be32_to_host(ipv4.source_ip);

    if (icmp->type == 8 && icmp->code == 0) {
        net_log("net: icmp echo request seq=%u\n", (unsigned)sequence);
        (void)send_icmp_reply(ethernet, ipv4, *icmp, payload + sizeof(IcmpEchoHeader), payload_size);
        return;
    }

    if (icmp->type == 0 &&
        icmp->code == 0 &&
        identifier == kPingIdentifier &&
        source_ip == g_pending_ping_ip &&
        sequence == g_pending_ping_sequence) {
        net_log("net: icmp echo reply seq=%u ttl=%u\n", (unsigned)sequence, (unsigned)ipv4.ttl);
        g_pending_ping_result.ipv4 = g_pending_ping_ip;
        g_pending_ping_result.reply_ipv4 = source_ip;
        g_pending_ping_result.ttl = ipv4.ttl;
        g_ping_complete = true;
        set_status(SAVANXP_NET_STATUS_ICMP_REPLY);
    }
}

void handle_udp(const Ipv4Header& ipv4, const uint8_t* payload, size_t length) {
    if (length < sizeof(UdpHeader)) {
        return;
    }

    const auto* udp = reinterpret_cast<const UdpHeader*>(payload);
    const uint16_t udp_length = be16_to_host(udp->length);
    if (udp_length < sizeof(UdpHeader) || udp_length > length) {
        return;
    }

    const uint16_t destination_port = be16_to_host(udp->destination_port);
    const uint16_t source_port = be16_to_host(udp->source_port);
    const uint8_t* udp_payload = payload + sizeof(UdpHeader);
    const size_t udp_payload_length = udp_length - sizeof(UdpHeader);
    const uint32_t source_ip = be32_to_host(ipv4.source_ip);

    (void)deliver_udp_payload(source_ip, source_port, kConfiguredIpv4, destination_port, udp_payload, udp_payload_length);
}

void handle_tcp(const Ipv4Header& ipv4, const uint8_t* payload, size_t length) {
    if (length < sizeof(TcpHeader)) {
        return;
    }

    const auto* tcp = reinterpret_cast<const TcpHeader*>(payload);
    const size_t header_length = static_cast<size_t>((tcp->data_offset_reserved >> 4) * 4u);
    if (header_length < sizeof(TcpHeader) || header_length > length) {
        return;
    }

    const uint16_t destination_port = be16_to_host(tcp->destination_port);
    const uint16_t source_port = be16_to_host(tcp->source_port);
    const uint32_t source_ip = be32_to_host(ipv4.source_ip);
    const uint32_t sequence_number = be32_to_host(tcp->sequence_number);
    const uint32_t acknowledgement_number = be32_to_host(tcp->acknowledgement_number);
    const uint8_t flags = tcp->flags;
    const uint8_t* tcp_payload = payload + header_length;
    const size_t tcp_payload_length = length - header_length;

    net::Socket* socket = find_tcp_socket(destination_port, source_port, source_ip);
    if (socket == nullptr) {
        socket = find_pending_tcp_socket(destination_port, source_port);
    }
    if (socket == nullptr) {
        return;
    }

    if ((flags & kTcpFlagAck) != 0 && acknowledgement_number > socket->send_unacked) {
        socket->send_unacked = acknowledgement_number;
    }

    if ((flags & kTcpFlagRst) != 0) {
        socket->connected = false;
        socket->tcp_state = net::TcpState::closed;
        socket->fin_received = true;
        return;
    }

    if (socket->tcp_state == net::TcpState::syn_sent) {
        if ((flags & (kTcpFlagSyn | kTcpFlagAck)) == (kTcpFlagSyn | kTcpFlagAck) &&
            acknowledgement_number == socket->send_next) {
            socket->remote_ip = source_ip;
            socket->recv_next = sequence_number + 1;
            socket->send_unacked = acknowledgement_number;
            socket->connected = true;
            socket->tcp_state = net::TcpState::established;
            set_status(SAVANXP_NET_STATUS_TCP_ESTABLISHED);
            (void)send_tcp_segment(*socket, kTcpFlagAck, nullptr, 0, socket->send_next, socket->recv_next);
        }
        return;
    }

    if (!socket->connected) {
        return;
    }

    uint32_t consumed_sequence = sequence_number;
    if (tcp_payload_length != 0) {
        if (sequence_number == socket->recv_next && enqueue_tcp_bytes(*socket, tcp_payload, tcp_payload_length)) {
            socket->recv_next += static_cast<uint32_t>(tcp_payload_length);
            consumed_sequence = socket->recv_next;
            (void)send_tcp_segment(*socket, kTcpFlagAck, nullptr, 0, socket->send_next, socket->recv_next);
        }
    }

    if ((flags & kTcpFlagFin) != 0) {
        if (consumed_sequence == socket->recv_next) {
            socket->recv_next += 1;
            socket->fin_received = true;
            socket->tcp_state = net::TcpState::close_wait;
            set_status(SAVANXP_NET_STATUS_TCP_FIN);
            (void)send_tcp_segment(*socket, kTcpFlagAck, nullptr, 0, socket->send_next, socket->recv_next);
        }
    }
}

void handle_ipv4(const EthernetHeader& ethernet, const uint8_t* payload, size_t length) {
    if (length < sizeof(Ipv4Header)) {
        return;
    }

    const auto* ipv4 = reinterpret_cast<const Ipv4Header*>(payload);
    const size_t header_length = (ipv4->version_ihl & 0x0fu) * 4u;
    const uint16_t total_length = be16_to_host(ipv4->total_length);
    const uint32_t destination_ip = be32_to_host(ipv4->destination_ip);
    if ((ipv4->version_ihl >> 4) != 4 || header_length < sizeof(Ipv4Header) || total_length < header_length || total_length > length) {
        return;
    }
    if (destination_ip != kConfiguredIpv4) {
        return;
    }
    if (ipv4->protocol == kIpProtocolIcmp) {
        handle_icmp(
            ethernet,
            *ipv4,
            payload + header_length,
            total_length - header_length
        );
    } else if (ipv4->protocol == kIpProtocolUdp) {
        handle_udp(*ipv4, payload + header_length, total_length - header_length);
    } else if (ipv4->protocol == kIpProtocolTcp) {
        handle_tcp(*ipv4, payload + header_length, total_length - header_length);
    }
}

void process_frame(const uint8_t* frame, size_t length) {
    if (length < sizeof(EthernetHeader)) {
        return;
    }

    const auto* ethernet = reinterpret_cast<const EthernetHeader*>(frame);
    const uint16_t ether_type = be16_to_host(ethernet->type);
    const uint8_t* payload = frame + sizeof(EthernetHeader);
    const size_t payload_length = length - sizeof(EthernetHeader);

    if (ether_type == kEtherTypeArp) {
        handle_arp(*ethernet, payload, payload_length);
    } else if (ether_type == kEtherTypeIpv4) {
        handle_ipv4(*ethernet, payload, payload_length);
    }
}

// Callbacks con los que el driver del NIC habla hacia arriba: un frame
// Ethernet crudo entra por aca, y los estados que solo el driver puede
// diagnosticar (TX_FAILED, RX_INVALID, ...) llegan por set_status.
void on_frame(const uint8_t* data, size_t length) {
    process_frame(data, length);
}

const nic::Events kNicEvents = {
    &on_frame,
    &set_status,
};


bool resolve_arp(uint32_t target_ip, uint32_t timeout_ms) {
    if (g_arp_ip == target_ip) {
        net_log("net: arp cache hit\n");
        set_status(SAVANXP_NET_STATUS_ARP_RESOLVED);
        return true;
    }

    net_log(
        "net: arp resolve %u.%u.%u.%u timeout=%u\n",
        (unsigned)((target_ip >> 24) & 0xffu),
        (unsigned)((target_ip >> 16) & 0xffu),
        (unsigned)((target_ip >> 8) & 0xffu),
        (unsigned)(target_ip & 0xffu),
        (unsigned)timeout_ms
    );
    if (!send_arp(kArpOpRequest, nullptr, kConfiguredIpv4, target_ip)) {
        return false;
    }

    const uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
    while (true) {
        nic::poll_receive();
        if (g_arp_ip == target_ip) {
            return true;
        }

        const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
        if (now_ms - start_ms >= timeout_ms) {
            net_log("net: arp timeout\n");
            ++g_arp_timeouts;
            set_status(SAVANXP_NET_STATUS_ARP_TIMEOUT);
            return false;
        }
        wait_for_tick();
    }
}

// Sube la interfaz. El driver hace lo suyo (reset, buffers, RX/TX enable)
// y aca queda solo el estado de red: cache ARP limpia y el status que ve
// userland.
bool bring_up() {
    if (!nic::present()) {
        set_status(SAVANXP_NET_STATUS_NO_DEVICE);
        return false;
    }
    if (g_up) {
        set_status(SAVANXP_NET_STATUS_READY);
        return true;
    }

    if (!nic::bring_up()) {
        // El driver ya reporto el motivo puntual por Events::status.
        return false;
    }

    g_arp_ip = 0;
    memset(g_arp_mac, 0, sizeof(g_arp_mac));
    g_up = true;
    set_status(SAVANXP_NET_STATUS_READY);
    return true;
}


int net_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case NET_IOC_GET_INFO: {
            if (!process::validate_user_range(argument, sizeof(savanxp_net_info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }

            nic::Stats stats = {};
            nic::get_stats(stats);
            savanxp_net_info info = {};
            info.present = nic::present() ? 1u : 0u;
            info.up = g_up ? 1u : 0u;
            info.link = (nic::present() && g_up) ? 1u : 0u;
            memcpy(info.mac, nic::mac_address(), sizeof(info.mac));
            info.ipv4 = kConfiguredIpv4;
            info.netmask = kConfiguredMask;
            info.gateway = kConfiguredGateway;
            info.last_status = g_last_status;
            info.tx_frames = stats.tx_frames;
            info.rx_frames = stats.rx_frames;
            info.tx_errors = stats.tx_errors;
            info.rx_errors = stats.rx_errors;
            info.arp_requests = g_arp_requests;
            info.arp_timeouts = g_arp_timeouts;
            info.ping_requests = g_ping_requests;
            info.ping_timeouts = g_ping_timeouts;
            return process::copy_to_user(argument, &info, sizeof(info)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case NET_IOC_UP:
            return bring_up() ? 0 : negative_error(nic::present() ? SAVANXP_EIO : SAVANXP_ENODEV);
        case NET_IOC_PING: {
            if (!process::validate_user_range(argument, sizeof(savanxp_net_ping_request), false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            savanxp_net_ping_request request_data = {};
            if (!process::copy_from_user(&request_data, argument, sizeof(request_data))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (request_data.result_ptr == 0 || !process::validate_user_range(request_data.result_ptr, sizeof(savanxp_net_ping_result), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!bring_up()) {
                return negative_error(nic::present() ? SAVANXP_EIO : SAVANXP_ENODEV);
            }

            const uint32_t next_hop = same_subnet(kConfiguredIpv4, request_data.ipv4) ? request_data.ipv4 : kConfiguredGateway;
            if (!resolve_arp(next_hop, request_data.timeout_ms != 0 ? request_data.timeout_ms : 1000u)) {
                return negative_error(SAVANXP_ETIMEDOUT);
            }

            memset(&g_pending_ping_result, 0, sizeof(g_pending_ping_result));
            g_pending_ping_ip = request_data.ipv4;
            g_pending_ping_sequence = request_data.sequence;
            g_ping_complete = false;
            ++g_ping_requests;

            if (!send_icmp_echo(request_data.ipv4, g_arp_mac, request_data.sequence, request_data.payload_size)) {
                return negative_error(SAVANXP_EIO);
            }

            const uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
            const uint32_t timeout_ms = request_data.timeout_ms != 0 ? request_data.timeout_ms : 1000u;
            while (true) {
                nic::poll_receive();
                if (g_ping_complete) {
                    const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
                    g_pending_ping_result.elapsed_ms = static_cast<uint32_t>(now_ms - start_ms);
                    return process::copy_to_user(request_data.result_ptr, &g_pending_ping_result, sizeof(g_pending_ping_result))
                        ? 0
                        : negative_error(SAVANXP_EINVAL);
                }

                const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
                if (now_ms - start_ms >= timeout_ms) {
                    ++g_ping_timeouts;
                    set_status(SAVANXP_NET_STATUS_ICMP_TIMEOUT);
                    return negative_error(SAVANXP_ETIMEDOUT);
                }
                wait_for_tick();
            }
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

} // namespace

namespace net {

int create_socket(uint32_t domain, uint32_t type, uint32_t protocol, Socket*& out_socket) {
    out_socket = nullptr;
    if (domain != SAVANXP_AF_INET) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (type != SAVANXP_SOCK_DGRAM && type != SAVANXP_SOCK_STREAM) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (type == SAVANXP_SOCK_DGRAM && protocol != 0 && protocol != SAVANXP_IPPROTO_UDP) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (type == SAVANXP_SOCK_STREAM && protocol != 0 && protocol != SAVANXP_IPPROTO_TCP) {
        return negative_error(SAVANXP_EINVAL);
    }

    for (auto& socket : g_sockets) {
        if (!socket.in_use) {
            memset(&socket, 0, sizeof(socket));
            socket.in_use = true;
            socket.domain = domain;
            socket.type = type;
            socket.protocol = type == SAVANXP_SOCK_STREAM ? SAVANXP_IPPROTO_TCP : SAVANXP_IPPROTO_UDP;
            socket.tcp_state = net::TcpState::closed;
            out_socket = &socket;
            return 0;
        }
    }
    return negative_error(SAVANXP_ENOMEM);
}

void close_socket(Socket* socket) {
    if (socket == nullptr || !socket->in_use) {
        return;
    }
    if (socket->type == SAVANXP_SOCK_STREAM && socket->connected) {
        (void)send_tcp_segment(*socket, kTcpFlagFin | kTcpFlagAck, nullptr, 0, socket->send_next, socket->recv_next);
        socket->send_next += 1;
    }
    memset(socket, 0, sizeof(*socket));
}

int bind_socket(Socket* socket, uint64_t user_address) {
    if (socket == nullptr || !socket->in_use) {
        return negative_error(SAVANXP_EBADF);
    }
    if (user_address == 0 || !process::validate_user_range(user_address, sizeof(savanxp_sockaddr_in), false)) {
        return negative_error(SAVANXP_EINVAL);
    }

    savanxp_sockaddr_in address = {};
    if (!process::copy_from_user(&address, user_address, sizeof(address))) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (address.ipv4 != 0 && address.ipv4 != kConfiguredIpv4) {
        return negative_error(SAVANXP_EINVAL);
    }
    return bind_socket_port(socket, address.port);
}

int connect_socket(Socket* socket, uint64_t user_address, uint32_t timeout_ms) {
    if (socket == nullptr || !socket->in_use || socket->type != SAVANXP_SOCK_STREAM || socket->protocol != SAVANXP_IPPROTO_TCP) {
        return negative_error(SAVANXP_EBADF);
    }
    if (user_address == 0 || !process::validate_user_range(user_address, sizeof(savanxp_sockaddr_in), false)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (!bring_up()) {
        return negative_error(nic::present() ? SAVANXP_EIO : SAVANXP_ENODEV);
    }

    savanxp_sockaddr_in address = {};
    if (!process::copy_from_user(&address, user_address, sizeof(address))) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (address.ipv4 == 0 || address.port == 0) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (socket->connected || socket->tcp_state == net::TcpState::syn_sent) {
        return negative_error(SAVANXP_EBUSY);
    }

    const int bind_result = ensure_socket_bound(socket);
    if (bind_result < 0) {
        return bind_result;
    }

    socket->remote_ip = address.ipv4;
    socket->remote_port = address.port;
    socket->initial_sequence = 0x53580000u + socket->local_port;
    socket->send_unacked = socket->initial_sequence;
    socket->send_next = socket->initial_sequence + 1;
    socket->recv_next = 0;
    socket->connected = false;
    socket->fin_received = false;
    socket->rx_head = 0;
    socket->rx_size = 0;
    socket->tcp_state = net::TcpState::syn_sent;
    set_status(SAVANXP_NET_STATUS_TCP_SYN_SENT);

    if (!send_tcp_segment(*socket, kTcpFlagSyn, nullptr, 0, socket->initial_sequence, 0)) {
        socket->tcp_state = net::TcpState::closed;
        return negative_error(SAVANXP_EIO);
    }

    const uint32_t effective_timeout = timeout_ms != 0 ? timeout_ms : 3000u;
    const uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
    while (!socket->connected) {
        nic::poll_receive();
        if (socket->tcp_state == net::TcpState::closed && socket->fin_received) {
            return negative_error(SAVANXP_EIO);
        }
        const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
        if (now_ms - start_ms >= effective_timeout) {
            socket->tcp_state = net::TcpState::closed;
            set_status(SAVANXP_NET_STATUS_TCP_TIMEOUT);
            return negative_error(SAVANXP_ETIMEDOUT);
        }
        wait_for_tick();
    }
    return 0;
}

int sendto_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address, bool nonblocking) {
    (void)nonblocking;
    if (socket == nullptr || !socket->in_use || socket->type != SAVANXP_SOCK_DGRAM || socket->protocol != SAVANXP_IPPROTO_UDP || count == 0 || count > 1024) {
        return count == 0 ? 0 : negative_error(SAVANXP_EINVAL);
    }
    if (user_address == 0 || !process::validate_user_range(user_address, sizeof(savanxp_sockaddr_in), false) ||
        !process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (!bring_up()) {
        return negative_error(nic::present() ? SAVANXP_EIO : SAVANXP_ENODEV);
    }

    savanxp_sockaddr_in address = {};
    uint8_t payload[1024] = {};
    if (!process::copy_from_user(&address, user_address, sizeof(address)) ||
        !process::copy_from_user(payload, user_buffer, count)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (address.port == 0 || address.ipv4 == 0) {
        return negative_error(SAVANXP_EINVAL);
    }

    const int bind_result = ensure_socket_bound(socket);
    if (bind_result < 0) {
        return bind_result;
    }

    if (address.ipv4 == kConfiguredIpv4) {
        if (!deliver_udp_payload(kConfiguredIpv4, socket->local_port, address.ipv4, address.port, payload, count)) {
            return negative_error(SAVANXP_EIO);
        }
        return static_cast<int>(count);
    }

    const uint32_t next_hop = same_subnet(kConfiguredIpv4, address.ipv4) ? address.ipv4 : kConfiguredGateway;
    if (!resolve_arp(next_hop, 1000u)) {
        return negative_error(SAVANXP_ETIMEDOUT);
    }
    if (!send_udp_datagram(address.ipv4, g_arp_mac, socket->local_port, address.port, payload, count)) {
        return negative_error(SAVANXP_EIO);
    }
    return static_cast<int>(count);
}

int recvfrom_socket(Socket* socket, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms, bool nonblocking) {
    if (socket == nullptr || !socket->in_use || socket->type != SAVANXP_SOCK_DGRAM || socket->protocol != SAVANXP_IPPROTO_UDP || count == 0 || !process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (user_address != 0 && !process::validate_user_range(user_address, sizeof(savanxp_sockaddr_in), true)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (!bring_up()) {
        return negative_error(nic::present() ? SAVANXP_EIO : SAVANXP_ENODEV);
    }

    const int bind_result = ensure_socket_bound(socket);
    if (bind_result < 0) {
        return bind_result;
    }

    const uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
    while (true) {
        nic::poll_receive();

        UdpPacket packet = {};
        if (dequeue_udp_packet(*socket, packet)) {
            const size_t to_copy = packet.length < count ? packet.length : count;
            if (!process::copy_to_user(user_buffer, packet.data, to_copy)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (user_address != 0) {
                savanxp_sockaddr_in address = {};
                address.ipv4 = packet.source_ip;
                address.port = packet.source_port;
                if (!process::copy_to_user(user_address, &address, sizeof(address))) {
                    return negative_error(SAVANXP_EINVAL);
                }
            }
            return static_cast<int>(to_copy);
        }

        if (nonblocking) {
            return negative_error(SAVANXP_EAGAIN);
        }

        if (timeout_ms != 0) {
            const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
            if (now_ms - start_ms >= timeout_ms) {
                return negative_error(SAVANXP_ETIMEDOUT);
            }
        }
        wait_for_tick();
    }
}

int read_socket(Socket* socket, uint64_t user_buffer, size_t count, bool nonblocking) {
    if (socket == nullptr || !socket->in_use || socket->type != SAVANXP_SOCK_STREAM || !socket->connected || count == 0) {
        return count == 0 ? 0 : negative_error(SAVANXP_EBADF);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
    while (socket->rx_size == 0) {
        nic::poll_receive();
        if (socket->rx_size != 0) {
            break;
        }
        if (socket->fin_received) {
            return 0;
        }
        if (nonblocking) {
            return negative_error(SAVANXP_EAGAIN);
        }
        const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
        if (now_ms - start_ms >= 5000u) {
            return negative_error(SAVANXP_ETIMEDOUT);
        }
        wait_for_tick();
    }

    uint8_t scratch[1024] = {};
    const size_t to_copy = count < sizeof(scratch) ? count : sizeof(scratch);
    const size_t copied = dequeue_tcp_bytes(*socket, scratch, to_copy);
    if (!process::copy_to_user(user_buffer, scratch, copied)) {
        return negative_error(SAVANXP_EINVAL);
    }
    return static_cast<int>(copied);
}

int write_socket(Socket* socket, uint64_t user_buffer, size_t count, bool nonblocking) {
    if (socket == nullptr || !socket->in_use || socket->type != SAVANXP_SOCK_STREAM || !socket->connected) {
        return negative_error(SAVANXP_EBADF);
    }
    if (count == 0) {
        return 0;
    }
    if (count > 1024 || !process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SAVANXP_EINVAL);
    }

    uint8_t payload[1024] = {};
    if (!process::copy_from_user(payload, user_buffer, count)) {
        return negative_error(SAVANXP_EINVAL);
    }

    const uint32_t sequence_number = socket->send_next;
    if (!send_tcp_segment(*socket, kTcpFlagAck | kTcpFlagPsh, payload, count, sequence_number, socket->recv_next)) {
        return negative_error(SAVANXP_EIO);
    }
    socket->send_next += static_cast<uint32_t>(count);

    if (nonblocking) {
        return static_cast<int>(count);
    }

    const uint64_t start_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
    while (socket->send_unacked < socket->send_next) {
        nic::poll_receive();
        if (socket->send_unacked >= socket->send_next) {
            break;
        }
        const uint64_t now_ms = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
        if (now_ms - start_ms >= 3000u) {
            return negative_error(SAVANXP_ETIMEDOUT);
        }
        wait_for_tick();
    }
    return static_cast<int>(count);
}

bool socket_can_read(const Socket* socket) {
    if (socket == nullptr || !socket->in_use) {
        return false;
    }
    if (socket->type == SAVANXP_SOCK_DGRAM) {
        return socket->packet_queue_size != 0;
    }
    if (socket->type == SAVANXP_SOCK_STREAM) {
        return socket->rx_size != 0 || socket->fin_received;
    }
    return false;
}

bool socket_can_write(const Socket* socket) {
    if (socket == nullptr || !socket->in_use) {
        return false;
    }
    if (socket->type == SAVANXP_SOCK_DGRAM) {
        return true;
    }
    if (socket->type == SAVANXP_SOCK_STREAM) {
        return socket->connected && !socket->fin_received;
    }
    return false;
}

bool socket_has_hangup(const Socket* socket) {
    if (socket == nullptr || !socket->in_use) {
        return false;
    }
    return socket->type == SAVANXP_SOCK_STREAM && socket->fin_received;
}

void initialize() {
    memset(g_sockets, 0, sizeof(g_sockets));
    g_device.ioctl = net_ioctl;
    g_ready = device::register_node("/dev/net0", &g_device, true);

    // El NIC ya lo ato nic::bind_best() desde kernel_main; aca solo se
    // enganchan los callbacks por los que el driver entrega frames y reporta
    // sus propias fallas.
    nic::attach(kNicEvents);
    set_status(nic::present() ? SAVANXP_NET_STATUS_IDLE : SAVANXP_NET_STATUS_NO_DEVICE);
}

bool ready() {
    return g_up;
}

bool present() {
    return nic::present();
}

void poll() {
    nic::poll_receive();
}

} // namespace net
