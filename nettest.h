#ifndef NETTEST_H_INCLUDED
#define NETTEST_H_INCLUDED

// Simple printf logging for internal debugging
#ifndef NETTEST_LOG
#define NETTEST_LOG 0
#endif

#include <stdint.h>

typedef uint8_t nettest_param_t;

typedef enum {
    // Send loss chance, in range 0..1
    NETTEST_PARAM_DROP_CHANCE,
    // Random delay in seconds
    NETTEST_PARAM_DELAY_MIN,
    NETTEST_PARAM_DELAY_MAX,
    // Chance for the packet to be sent twice, in range 0..1
    NETTEST_PARAM_DUPLICATE_CHANCE,

    // Internal

    // milliseconds, truncated to integer
    NETTEST_PARAM_THREAD_SLEEP,

    NETTEST_PARAM_COUNT,
} _nettest_param_t;

// Initialize the internal state.
// If sync is false, this spawns a background thread which is
// going to periodically send packets which are ready.
void nettest_init(bool sync);

// Call every few ms to dispatch packets which are ready.
// WARNING: only call if you called `nettest_init` with sync=true!!!
void nettest_update();

// Flush and shutdown all internal state
void nettest_shutdown();

// Set internal parameter
void nettest_set_param(nettest_param_t param, float value);
float nettest_get_param(nettest_param_t param);

int nettest_sendto(
    int sockfd,
    const char* data,
    size_t data_len,
    int flags,
    const void* dest_addr,
    size_t dest_addr_size);

static int nettest_send(
    int sockfd,
    const char* data,
    size_t data_len,
    int flags) {
    return nettest_sendto(sockfd, data, data_len, flags, NULL, 0);
}

#endif // NETTEST_H_INCLUDED



#ifdef NETTEST_IMPLEMENTATION

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#error "Currently only Windows is supported"
#endif

#if NETTEST_LOG
#define _nettest_log(fmt, ...) printf(fmt, __VA_ARGS__)
#else
#define _nettest_log(fmt, ...)
#endif


#ifdef _WIN32
typedef struct {
    HANDLE thread_handle;
    LARGE_INTEGER timer_freq;
    LARGE_INTEGER timer_prev;
} nettest_plat_state_t;
#endif


#define NETTEST_REORDER_SLOTS 512

typedef struct {
    // ID for debugging purposes.
    uint64_t id;
    int sockfd;
    void* data;
    size_t data_len;
    int flags;
    const void* dest_addr;
    size_t dest_addr_size;
} nettest_packet_t;

typedef struct {
    uint32_t rand_seed;
    uint64_t id_counter;
    bool running;

    float params[NETTEST_PARAM_COUNT];

    // Note: slots are accessed from multiple threads, have to use atomics and coherent reads/writes.
    volatile int16_t slot[NETTEST_REORDER_SLOTS];
    float time_left[NETTEST_REORDER_SLOTS];
    nettest_packet_t packets[NETTEST_REORDER_SLOTS];

    nettest_plat_state_t plat;
} nettest_state_t;

nettest_state_t _nettest;



#ifdef _WIN32 // Windows
DWORD WINAPI _nettest_win32_thread_func(LPVOID lp_param) {
    while(_nettest.running) {
        // _nettest_log("NETTEST: thread update\n");
        nettest_update();

        int sleep_ms = _nettest.params[NETTEST_PARAM_THREAD_SLEEP];
        if(sleep_ms < 1) sleep_ms = 1;
        Sleep(sleep_ms);
    }
    return 0;
}
#endif



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


void nettest_init(bool sync) {
    _nettest.rand_seed = 0x012398;
    _nettest.running = true;
    QueryPerformanceFrequency(&_nettest.plat.timer_freq);
    QueryPerformanceCounter(&_nettest.plat.timer_prev);
    if(!sync) {
        _nettest.plat.thread_handle = CreateThread(NULL, 0, _nettest_win32_thread_func, 0, 0, 0);
    }
}

void nettest_shutdown() {
    _nettest.running = false;
    WaitForSingleObject(_nettest.plat.thread_handle, INFINITE);
    CloseHandle(_nettest.plat.thread_handle);
}

void nettest_update() {
    LARGE_INTEGER timer_curr;
    QueryPerformanceCounter(&timer_curr);

    float delta = (double)(timer_curr.QuadPart - _nettest.plat.timer_prev.QuadPart) / (double)_nettest.plat.timer_freq.QuadPart;

    // _nettest_log("NETTEST: update delta: %f\n", delta);

    for(int i = 0; i < NETTEST_REORDER_SLOTS; i++) {
        if(2 != _InterlockedCompareExchange16(&_nettest.slot[i], 2, 2)) {
            continue;
        }

        // _nettest_log("NETTEST: waiting packet %llu\n", _nettest.packets[i].id);

        _nettest.time_left[i] -= delta;
        if(_nettest.time_left[i] <= 0) {
            _nettest_log("NETTEST: sending packet %llu\n", _nettest.packets[i].id);
            _nettest_packet_sendto(_nettest.packets[i]);

            if(_nettest_frand() < _nettest.params[NETTEST_PARAM_DUPLICATE_CHANCE]) {
                _nettest_log("NETTEST: duplicating packet %llu\n", _nettest.packets[i].id);
                _nettest_packet_sendto(_nettest.packets[i]);
            }

            free(_nettest.packets[i].data);

            // Mark this slot as empty again
            _InterlockedExchange16(&_nettest.slot[i], 0);
        }
    }

    _nettest.plat.timer_prev = timer_curr;
}

int nettest_sendto(
    int sockfd,
    const char* data,
    size_t data_len,
    int flags,
    const void* dest_addr,
    size_t dest_addr_size) {

    _nettest.id_counter++;
    int64_t id = _nettest.id_counter;

    if (_nettest_frand() < _nettest.params[NETTEST_PARAM_DROP_CHANCE]) {
        // Return as if nothing happened.
        // _nettest_log("NETTEST: dropped packet %llu\n", id);
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

            // _nettest_log("NETTEST: storing packet %llu\n", id);

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