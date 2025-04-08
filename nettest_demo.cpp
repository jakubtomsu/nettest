#include <stdio.h>

#define NETTEST_IMPLEMENTATION
#include "nettest.h"

#define IP_ADDRESS "127.0.0.1"
#define SEND_PORT 5100
#define RECV_PORT 5200
#define SEND_DELAY 100

#define USE_NETTEST_SENDTO 1

#if USE_NETTEST_SENDTO
#define _sendto nettest_sendto
#else
#define _sendto sendto
#endif

typedef struct {
    uint32_t index;
    char msg[256];
} packet_t;

int main(int argc, char** argv) {
    if(argc < 2) {
        printf("Invalid arguments. Please use s/r to send/recieve\n");
        return 1;
    }

    int sender = argv[1][0] == 's';

    nettest_init(0);

    // nettest_set_param(NETTEST_PARAM_DROP_CHANCE, 0.1f);
    // nettest_set_param(NETTEST_PARAM_DELAY_MIN, 0.0f);
    nettest_set_param(NETTEST_PARAM_DELAY_MAX, 1.0f);
    // nettest_set_param(NETTEST_PARAM_DUPLICATE_CHANCE, 0.1f);
    nettest_set_param(NETTEST_PARAM_THREAD_SLEEP, 5.0f);

#if _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(sender ? SEND_PORT : RECV_PORT);
    local.sin_addr.s_addr = INADDR_ANY;

    int sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if(bind(sock, (struct sockaddr*)&local, sizeof(local)) != 0) {
        printf("Failed to bind\n");
        return 1;
    }

    struct sockaddr_in addr_dst;
    addr_dst.sin_family = AF_INET;
    addr_dst.sin_port = htons(sender ? RECV_PORT : SEND_PORT);
    inet_pton(AF_INET, IP_ADDRESS, &addr_dst.sin_addr.s_addr);


    packet_t packet = {0};
    if(sender) {
        for(uint32_t i = 0;; i++) {
            packet.index = i;
            snprintf(packet.msg, sizeof(packet.msg), "Hello world %x", i);

            printf("Sending packet: %u: %s\n", i, packet.msg);
            if(_sendto(sock, (const char*)&packet, sizeof(packet_t), 0, (const struct sockaddr*)&addr_dst, sizeof(struct sockaddr)) == -1) {
                printf("sendto failed");
            }

            Sleep(SEND_DELAY);
        }

    } else {
        uint32_t mod_bits = 0;

        while(1) {
            struct sockaddr_in from;
            int from_size = sizeof(struct sockaddr_in);

            int len = recvfrom(sock, (char*)&packet, sizeof(packet_t), 0, (struct sockaddr*)&from, &from_size);
            if(len > 0) {
                mod_bits ^= (uint32_t)1 << (uint32_t)(packet.index);

                for (uint32_t i = 0; i < 32; i++) {
                    putchar((mod_bits & (uint32_t)(1 << i)) != 0 ? '*' : ' ');
                }

                printf(" Recieved packet: %u: %s, size %i\n", packet.index, packet.msg, len);
            } else {
                printf("Recieved invalid packet length\n");
            }
        }
    }

    nettest_shutdown();

    return 0;
}