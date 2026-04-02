#include "libc.h"

static void print_ipv4(uint32_t address) {
    printf(
        "%u.%u.%u.%u",
        (unsigned int)((address >> 24) & 0xffu),
        (unsigned int)((address >> 16) & 0xffu),
        (unsigned int)((address >> 8) & 0xffu),
        (unsigned int)(address & 0xffu)
    );
}

int main(void) {
    struct savanxp_net_info info;
    long fd = open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        eprintf("netinfo: /dev/net0 unavailable (%s)\n", result_error_string(fd));
        return 1;
    }

    memset(&info, 0, sizeof(info));
    {
        long status = ioctl((int)fd, NET_IOC_GET_INFO, (unsigned long)&info);
        if (status < 0) {
            eprintf("netinfo: NET_IOC_GET_INFO failed (%s)\n", result_error_string(status));
            close((int)fd);
            return 1;
        }
    }

    if (!info.present) {
        puts("net0: not present\n");
        close((int)fd);
        return 0;
    }

    if (!info.up) {
        long status = ioctl((int)fd, NET_IOC_UP, 0);
        if (status < 0) {
            eprintf("netinfo: NET_IOC_UP failed (%s)\n", result_error_string(status));
            close((int)fd);
            return 1;
        }

        status = ioctl((int)fd, NET_IOC_GET_INFO, (unsigned long)&info);
        if (status < 0) {
            eprintf("netinfo: NET_IOC_GET_INFO failed after NET_IOC_UP (%s)\n", result_error_string(status));
            close((int)fd);
            return 1;
        }
    }

    printf(
        "net0: present=%u up=%u link=%u mac=%x:%x:%x:%x:%x:%x\n",
        (unsigned int)info.present,
        (unsigned int)info.up,
        (unsigned int)info.link,
        (unsigned int)info.mac[0],
        (unsigned int)info.mac[1],
        (unsigned int)info.mac[2],
        (unsigned int)info.mac[3],
        (unsigned int)info.mac[4],
        (unsigned int)info.mac[5]
    );
    puts("ip: ");
    print_ipv4(info.ipv4);
    putchar(1, '\n');
    puts("mask: ");
    print_ipv4(info.netmask);
    putchar(1, '\n');
    puts("gateway: ");
    print_ipv4(info.gateway);
    putchar(1, '\n');
    printf("status: %s\n", net_status_string(info.last_status));
    printf(
        "stats: tx=%u rx=%u txerr=%u rxerr=%u arp=%u arpto=%u ping=%u pingto=%u\n",
        (unsigned int)info.tx_frames,
        (unsigned int)info.rx_frames,
        (unsigned int)info.tx_errors,
        (unsigned int)info.rx_errors,
        (unsigned int)info.arp_requests,
        (unsigned int)info.arp_timeouts,
        (unsigned int)info.ping_requests,
        (unsigned int)info.ping_timeouts
    );

    close((int)fd);
    return 0;
}
