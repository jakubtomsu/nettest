#ifndef NETTEST_H_INCLUDED
#define NETTEST_H_INCLUDED

#include <stdint.h>

typedef uint8_t nettest_param_t;

typedef enum {
    // Send loss chance, in range 0..1
    NETTEST_PARAM_DROP_CHANCE,
    // Random delay in seconds
    NETTEST_PARAM_DELAY_MIN,
    NETTEST_PARAM_DELAY_MAX,
    NETTEST_PARAM_DUPLICATE_CHANCE,

    // Internal

    // milliseconds, truncated to integer
    NETTEST_PARAM_THREAD_SLEEP,

    NETTEST_PARAM_COUNT,
} _nettest_param_t;

// Initialize the internal state
void nettest_init(bool sync);

// Call every few ms, only if working in sync mode!
void nettest_update();

// Flush and shutdown all internal state
void nettest_shutdown();

// Set internal parameter
void nettest_set_param(nettest_param_t param, float value);
float nettest_get_param(nettest_param_t param);

// BSD sockets' `sendto` alternative
int nettest_sendto(
    int sockfd,
    const void* data,
    size_t data_len,
    int flags,
    const void* dest_addr,
    size_t dest_addr_size);

#endif // NETTEST_H_INCLUDED



#ifdef NETTEST_IMPLEMENTATION

#ifdef _WIN32
#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define NETTEST_REORDER_SLOTS 512

typedef struct {
    uint64_t id;
    int sockfd;
    void* data;
    size_t data_len;
    int flags;
    const void* dest_addr;
    size_t dest_addr_size;
} nettest_packet_t;

typedef struct {
    // Note: slots are accessed from multiple threads, have to use atomics and coherent reads/writes.
    volatile int16_t slot[NETTEST_REORDER_SLOTS];
    float time_left[NETTEST_REORDER_SLOTS];
    nettest_packet_t packets[NETTEST_REORDER_SLOTS];
    float params[NETTEST_PARAM_COUNT];
    uint32_t rand_seed;
    uint64_t id_counter;
    bool running;

#if _WIN32
    HANDLE thread_handle;
    LARGE_INTEGER timer_freq;
    LARGE_INTEGER timer_prev;
#endif
} nettest_state_t;

nettest_state_t _nettest;

static uint32_t _nettest_rand() {
    _nettest.rand_seed = _nettest.rand_seed * 0x343fd + 0x269ec3;
    return (_nettest.rand_seed >> 16) & 32767;
}

static float _nettest_frand() {
    return (float)_nettest_rand() / 32767.0f;
}

void nettest_set_param(nettest_param_t param, float value) {
    _nettest.params[param] = value;
}

float nettest_get_param(nettest_param_t param) {
    return _nettest.params[param];
}

static void _nettest_packet_sendto(nettest_packet_t packet) {
    sendto(
        packet.sockfd,
        (const char*)packet.data,
        packet.data_len,
        packet.flags,
        (const sockaddr*)packet.dest_addr,
        packet.dest_addr_size);
}

#if _WIN32
DWORD WINAPI _nettest_win32_thread_func(LPVOID lp_param) {
    while(_nettest.running) {
        // printf("NETTEST: thread update\n");
        nettest_update();

        int sleep_ms = _nettest.params[NETTEST_PARAM_THREAD_SLEEP];
        if(sleep_ms < 1) sleep_ms = 1;
        Sleep(sleep_ms);
    }
    return 0;
}
#endif

void nettest_init(bool sync) {
    QueryPerformanceFrequency(&_nettest.timer_freq);
    QueryPerformanceCounter(&_nettest.timer_prev);

    _nettest.rand_seed = 0x012398;
    _nettest.running = true;

    if(!sync) {
        _nettest.thread_handle = CreateThread(NULL, 0, _nettest_win32_thread_func, 0, 0, 0);
    }
}

void nettest_shutdown() {
    WaitForSingleObject(_nettest.thread_handle, INFINITE);
}

void nettest_update() {
    LARGE_INTEGER timer_curr;
    QueryPerformanceCounter(&timer_curr);

    float delta = (double)(timer_curr.QuadPart - _nettest.timer_prev.QuadPart) / (double)_nettest.timer_freq.QuadPart;

    // printf("NETTEST: update delta: %f\n", delta);

    for(int i = 0; i < NETTEST_REORDER_SLOTS; i++) {
        if(2 != _InterlockedCompareExchange16(&_nettest.slot[i], 2, 2)) {
            continue;
        }

        // printf("NETTEST: waiting packet %llu\n", _nettest.packets[i].id);

        _nettest.time_left[i] -= delta;
        if(_nettest.time_left[i] <= 0) {
            printf("NETTEST: sending packet %llu\n", _nettest.packets[i].id);
            _nettest_packet_sendto(_nettest.packets[i]);

            if(_nettest_frand() < _nettest.params[NETTEST_PARAM_DUPLICATE_CHANCE]) {
                printf("NETTEST: duplicating packet %llu\n", _nettest.packets[i].id);
                _nettest_packet_sendto(_nettest.packets[i]);
            }

            free(_nettest.packets[i].data);

            // Mark this slot as empty again
            _InterlockedExchange16(&_nettest.slot[i], 0);
        }
    }

    _nettest.timer_prev = timer_curr;
}

int nettest_sendto(
    int sockfd,
    const void* data,
    size_t data_len,
    int flags,
    const void* dest_addr,
    size_t dest_addr_size) {

    _nettest.id_counter++;
    int64_t id = _nettest.id_counter;

    if (_nettest_frand() < _nettest.params[NETTEST_PARAM_DROP_CHANCE]) {
        // Return as if nothing happened.
        printf("NETTEST: dropped packet %llu\n", id);
        return data_len;
    }

    bool done = false;
    while(!done) {
        for(int i = 0; i < NETTEST_REORDER_SLOTS; i++) {
            if(0 != _InterlockedCompareExchange16(&_nettest.slot[i], 1, 0)) {
                continue;
            }

            float time_left = _nettest.params[NETTEST_PARAM_DELAY_MIN] +
                              _nettest_frand() *
                                  (_nettest.params[NETTEST_PARAM_DELAY_MAX] - _nettest.params[NETTEST_PARAM_DELAY_MIN]);

            // printf("NETTEST: storing packet %llu\n", id);

            void* buf = malloc(data_len);
            memcpy(buf, data, data_len);

            _nettest.time_left[i] = time_left;

            _nettest.packets[i].id = id;
            _nettest.packets[i].sockfd = sockfd;
            _nettest.packets[i].data = buf;
            _nettest.packets[i].data_len = data_len;
            _nettest.packets[i].flags = flags;
            _nettest.packets[i].dest_addr = dest_addr;
            _nettest.packets[i].dest_addr_size = dest_addr_size;

            _InterlockedExchange16(&_nettest.slot[i], 2);

            done = true;
            break;
        }

        if(!done) {
            Sleep(1);
        }
    }

    return data_len;
}


#endif // NETTEST_IMPLEMENTATION