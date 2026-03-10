#include "savanxp/libc.h"

static int fetch_local_ip(uint32_t* address) {
    long fd;
    long status;
    struct savanxp_net_info info;

    fd = open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return 0;
    }

    memset(&info, 0, sizeof(info));
    status = ioctl((int)fd, NET_IOC_UP, 0);
    if (status >= 0) {
        status = ioctl((int)fd, NET_IOC_GET_INFO, (unsigned long)&info);
    }
    close((int)fd);
    if (status < 0 || !info.present) {
        return 0;
    }

    *address = info.ipv4;
    return 1;
}

int main(void) {
    long rx_fd;
    long tx_fd;
    long status;
    uint32_t local_ip = 0;
    struct savanxp_sockaddr_in address;
    struct savanxp_sockaddr_in remote;
    char buffer[64];
    const char* message = "sdk udp ok";

    if (!fetch_local_ip(&local_ip)) {
        puts_err("sdk udptest: unable to query local ip\n");
        return 1;
    }

    rx_fd = socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    tx_fd = socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (rx_fd < 0 || tx_fd < 0) {
        eprintf("sdk udptest: socket failed (%s)\n", result_error_string(rx_fd < 0 ? rx_fd : tx_fd));
        if (rx_fd >= 0) {
            close((int)rx_fd);
        }
        if (tx_fd >= 0) {
            close((int)tx_fd);
        }
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.port = 7100;
    status = bind((int)rx_fd, &address);
    if (status < 0) {
        eprintf("sdk udptest: bind failed (%s)\n", result_error_string(status));
        close((int)rx_fd);
        close((int)tx_fd);
        return 1;
    }

    address.ipv4 = local_ip;
    status = sendto((int)tx_fd, message, strlen(message), &address);
    if (status < 0) {
        eprintf("sdk udptest: sendto failed (%s)\n", result_error_string(status));
        close((int)rx_fd);
        close((int)tx_fd);
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    memset(&remote, 0, sizeof(remote));
    status = recvfrom((int)rx_fd, buffer, sizeof(buffer) - 1, &remote, 1000);
    if (status < 0) {
        eprintf("sdk udptest: recvfrom failed (%s)\n", result_error_string(status));
        close((int)rx_fd);
        close((int)tx_fd);
        return 1;
    }

    buffer[status < (long)(sizeof(buffer) - 1) ? status : (long)(sizeof(buffer) - 1)] = '\0';
    printf("sdk udp ok: %s\n", buffer);

    close((int)rx_fd);
    close((int)tx_fd);
    return 0;
}
