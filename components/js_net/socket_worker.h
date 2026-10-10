#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "socket_tls.h"

typedef enum { SOCKET_DIAL, SOCKET_LISTEN, SOCKET_ACCEPT, SOCKET_READ,
               SOCKET_WRITE, SOCKET_END, SOCKET_DIAL_TLS } socket_operation_t;

typedef struct socket_request socket_request_t;
struct socket_request {
    socket_operation_t operation;
    unsigned slot;
    uint32_t generation;
    int64_t deadline_us;
    uint8_t *data;
    size_t size;
    size_t transferred;
    char host[256];
    char server_name[256];
    char *ca;
    size_t ca_size;
    uint16_t port;
    unsigned backlog;
    bool no_delay;
    bool started;
    bool eof;
    const char *error;
    int system_error;
    int tls_code;
    uint32_t verify_flags;
    int tls_wait;
    unsigned accepted_slot;
    uint32_t accepted_generation;
    socket_request_t *next;
};

typedef struct {
    char address[48];
    uint16_t port;
    unsigned family;
} socket_address_t;

typedef struct {
    /* used, generation, closing and closed are protected by the engine lock. */
    bool used;
    bool closing;
    bool closed;
    uint32_t generation;
    /* Remaining fields belong to the network worker. */
    int fd;
    bool listener;
    bool eof;
    bool write_closed;
    bool connected;
    socket_tls_t *tls;
    socket_request_t *tls_busy;
    socket_address_t local;
    socket_address_t remote;
} socket_slot_t;

typedef struct {
    unsigned slot;
    uint32_t generation;
    char host[256];
    uint16_t port;
    int error;
    /* IPv4/IPv6 sockaddr storage, copied by the resolver. */
    uint8_t address[128];
    unsigned address_size;
} socket_dns_t;

typedef struct {
    QueueHandle_t commands;
    QueueHandle_t completions;
    QueueHandle_t dns_commands;
    QueueHandle_t dns_results;
    SemaphoreHandle_t worker_stopped;
    SemaphoreHandle_t resolver_stopped;
    portMUX_TYPE lock;
    bool stopping;
    void (*notify)(void *opaque);
    void *notify_opaque;
    unsigned dns_pending;
    unsigned tls_count;
    socket_request_t *pending;
    socket_slot_t slots[CONFIG_JS_NET_MAX_CONNECTIONS];
} socket_engine_t;

bool socket_engine_start(socket_engine_t *engine, void (*notify)(void *), void *opaque);
void socket_engine_stop(socket_engine_t *engine);
int socket_engine_reserve(socket_engine_t *engine, uint32_t *generation);
void socket_engine_release(socket_engine_t *engine, unsigned slot);
void socket_engine_close(socket_engine_t *engine, unsigned slot, uint32_t generation);
bool socket_engine_closed(socket_engine_t *engine, unsigned slot, uint32_t generation);
