#include "socket_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#if CONFIG_JS_NET_ENABLED

#define WORKER_WAIT_MS 20
#define WORKER_STACK_SIZE 8192

static bool is_stopping(socket_engine_t *engine)
{
    portENTER_CRITICAL(&engine->lock);
    bool stopping = engine->stopping;
    portEXIT_CRITICAL(&engine->lock);
    return stopping;
}

int socket_engine_reserve(socket_engine_t *engine, uint32_t *generation)
{
    int result = -1;
    portENTER_CRITICAL(&engine->lock);
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) {
        socket_slot_t *slot = &engine->slots[i];
        if (slot->used) continue;
        uint32_t next = slot->generation + 1;
        /* Never reuse a generation, including after uint32 wrap. */
        if (!next) continue;
        *slot = (socket_slot_t){.used = true, .generation = next, .fd = -1};
        *generation = next;
        result = (int)i;
        break;
    }
    portEXIT_CRITICAL(&engine->lock);
    return result;
}

void socket_engine_release(socket_engine_t *engine, unsigned index)
{
    portENTER_CRITICAL(&engine->lock);
    engine->slots[index].used = false;
    portEXIT_CRITICAL(&engine->lock);
}

void socket_engine_close(socket_engine_t *engine, unsigned index, uint32_t generation)
{
    portENTER_CRITICAL(&engine->lock);
    socket_slot_t *slot = &engine->slots[index];
    if (slot->used && slot->generation == generation) slot->closing = true;
    portEXIT_CRITICAL(&engine->lock);
}

bool socket_engine_closed(socket_engine_t *engine, unsigned index, uint32_t generation)
{
    portENTER_CRITICAL(&engine->lock);
    socket_slot_t *slot = &engine->slots[index];
    bool closed = !slot->used || slot->generation != generation ||
                  slot->closing || slot->closed;
    portEXIT_CRITICAL(&engine->lock);
    return closed;
}

static bool slot_valid(socket_engine_t *engine, socket_request_t *request)
{
    return !socket_engine_closed(engine, request->slot, request->generation);
}

static void close_slot(socket_engine_t *engine, unsigned index)
{
    socket_slot_t *slot = &engine->slots[index];
    if (slot->tls) --engine->tls_count;
    socket_tls_free(slot->tls);
    slot->tls = NULL;
    slot->tls_busy = NULL;
    if (slot->fd >= 0) close(slot->fd);
    slot->fd = -1;
    portENTER_CRITICAL(&engine->lock);
    slot->closed = true;
    portEXIT_CRITICAL(&engine->lock);
}

static void fail(socket_request_t *request, const char *code, int error)
{
    request->error = code;
    request->system_error = error;
}

static const char *io_error(int error)
{
    if (error == ECONNREFUSED) return "CONNECTION_REFUSED";
    if (error == ECONNRESET || error == EPIPE) return "CONNECTION_RESET";
    if (error == ETIMEDOUT) return "TIMEOUT";
    return "IO";
}

static bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static bool dialing(const socket_request_t *request)
{
    return request->operation == SOCKET_DIAL || request->operation == SOCKET_DIAL_TLS;
}

static bool tls_result(socket_engine_t *engine, socket_request_t *request,
                        int result, const socket_tls_error_t *error)
{
    socket_slot_t *slot = &engine->slots[request->slot];
    request->tls_wait = result;
    if (slot->tls_busy == request) slot->tls_busy = NULL;
    if (result == SOCKET_TLS_WANT_READ || result == SOCKET_TLS_WANT_WRITE) {
        /* A read must not flush a pending application write: retrying that write
         * after another SSL call flushed it could transmit the record twice. */
        if (request->operation != SOCKET_READ || result == SOCKET_TLS_WANT_WRITE ||
            socket_tls_control_pending(slot->tls))
            slot->tls_busy = request;
        return false;
    }
    request->tls_wait = 0;
    if (result == SOCKET_TLS_FAILED) {
        fail(request, error->code, error->system_error);
        request->tls_code = error->tls_code;
        request->verify_flags = error->verify_flags;
        close_slot(engine, request->slot);
    }
    return true;
}

static bool tls_ready(socket_request_t *request, int fd, fd_set *readers, fd_set *writers)
{
    if (request->tls_wait == SOCKET_TLS_WANT_READ) return FD_ISSET(fd, readers);
    if (request->tls_wait == SOCKET_TLS_WANT_WRITE) return FD_ISSET(fd, writers);
    return true;
}

static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool literal_address(const char *host, uint16_t port,
                             struct sockaddr_storage *storage, socklen_t *size)
{
    memset(storage, 0, sizeof(*storage));
    struct sockaddr_in *v4 = (struct sockaddr_in *)storage;
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        *size = sizeof(*v4);
        return true;
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)storage;
    if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        *size = sizeof(*v6);
        return true;
    }
    return false;
}

static void read_address(int fd, bool peer, socket_address_t *out)
{
    struct sockaddr_storage address = {0};
    socklen_t length = sizeof(address);
    int result = peer ? getpeername(fd, (struct sockaddr *)&address, &length)
                      : getsockname(fd, (struct sockaddr *)&address, &length);
    if (result != 0) return;
    if (address.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
        inet_ntop(AF_INET, &v4->sin_addr, out->address, sizeof(out->address));
        out->port = ntohs(v4->sin_port);
        out->family = 4;
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
        inet_ntop(AF_INET6, &v6->sin6_addr, out->address, sizeof(out->address));
        out->port = ntohs(v6->sin6_port);
        out->family = 6;
    }
}

static void resolver_task(void *opaque)
{
    socket_engine_t *engine = opaque;
    socket_dns_t job;
    while (xQueueReceive(engine->dns_commands, &job, portMAX_DELAY) == pdTRUE) {
        if (!job.generation) break;
        struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
        struct addrinfo *addresses = NULL;
        job.error = getaddrinfo(job.host, NULL, &hints, &addresses);
        job.address_size = 0;
        for (struct addrinfo *item = addresses; !job.error && item; item = item->ai_next) {
            if ((item->ai_family != AF_INET && item->ai_family != AF_INET6) ||
                item->ai_addrlen > sizeof(job.address)) continue;
            memcpy(job.address, item->ai_addr, item->ai_addrlen);
            job.address_size = item->ai_addrlen;
            break;
        }
        if (addresses) freeaddrinfo(addresses);
        if (!job.address_size && !job.error) job.error = EAI_NONAME;
        /* At most MAX_CONNECTIONS DNS jobs exist, including unread results. */
        xQueueSend(engine->dns_results, &job, 0);
    }
    xSemaphoreGive(engine->resolver_stopped);
    vTaskDelete(NULL);
}

static void begin_connect(socket_engine_t *engine, socket_request_t *request,
                           struct sockaddr *address, socklen_t size)
{
    socket_slot_t *slot = &engine->slots[request->slot];
    if (address->sa_family == AF_INET)
        ((struct sockaddr_in *)address)->sin_port = htons(request->port);
    else
        ((struct sockaddr_in6 *)address)->sin6_port = htons(request->port);
    slot->fd = socket(address->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (slot->fd < 0 || !nonblocking(slot->fd)) {
        fail(request, "IO", errno);
        return;
    }
    int no_delay = request->no_delay;
    if (setsockopt(slot->fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) < 0) {
        fail(request, "IO", errno);
        return;
    }
    int result = connect(slot->fd, address, size);
    if (result < 0 && errno != EINPROGRESS && !would_block())
        fail(request, io_error(errno), errno);
}

static void begin_request(socket_engine_t *engine, socket_request_t *request)
{
    request->started = true;
    struct sockaddr_storage address;
    socklen_t size = 0;
    bool literal = literal_address(request->host, request->port, &address, &size);
    if (dialing(request)) {
        if (literal) {
            begin_connect(engine, request, (struct sockaddr *)&address, size);
        } else if (engine->dns_pending < CONFIG_JS_NET_MAX_CONNECTIONS) {
            socket_dns_t job = {.slot = request->slot, .generation = request->generation,
                                .port = request->port};
            memcpy(job.host, request->host, sizeof(job.host));
            ++engine->dns_pending;
            xQueueSend(engine->dns_commands, &job, 0);
        } else {
            fail(request, "QUEUE_FULL", 0);
        }
        return;
    }
    if (request->operation != SOCKET_LISTEN) return;
    if (!literal) {
        fail(request, "INVALID_ADDRESS", 0);
        return;
    }
    socket_slot_t *slot = &engine->slots[request->slot];
    slot->listener = true;
    slot->fd = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (slot->fd < 0 || !nonblocking(slot->fd)) {
        fail(request, "IO", errno);
        return;
    }
    int reuse = 1;
    if (setsockopt(slot->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(slot->fd, (struct sockaddr *)&address, size) < 0 ||
        listen(slot->fd, request->backlog) < 0) {
        fail(request, io_error(errno), errno);
        return;
    }
    read_address(slot->fd, false, &slot->local);
}

static void collect_dns(socket_engine_t *engine)
{
    socket_dns_t job;
    while (xQueueReceive(engine->dns_results, &job, 0) == pdTRUE) {
        --engine->dns_pending;
        for (socket_request_t *request = engine->pending; request; request = request->next) {
            if (!dialing(request) || request->slot != job.slot ||
                request->generation != job.generation || !slot_valid(engine, request)) continue;
            if (job.error) fail(request, "DNS", job.error);
            else {
                struct sockaddr_storage address;
                memcpy(&address, job.address, job.address_size);
                begin_connect(engine, request, (struct sockaddr *)&address, job.address_size);
            }
            break;
        }
    }
}

static bool earlier_write(socket_engine_t *engine, socket_request_t *request)
{
    for (socket_request_t *item = engine->pending; item != request; item = item->next) {
        if (item->slot == request->slot && item->generation == request->generation &&
            (item->operation == SOCKET_WRITE || item->operation == SOCKET_END)) return true;
    }
    return false;
}

static bool advance_request(socket_engine_t *engine, socket_request_t *request,
                             fd_set *readers, fd_set *writers)
{
    socket_slot_t *slot = &engine->slots[request->slot];
    int fd = slot->fd;
    if (slot->tls_busy && slot->tls_busy != request) return false;
    switch (request->operation) {
    case SOCKET_LISTEN:
        return true;
    case SOCKET_DIAL:
    case SOCKET_DIAL_TLS: {
        if (fd < 0) return false;
        if (!slot->connected) {
            if (!FD_ISSET(fd, writers)) return false;
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
            if (error) { fail(request, io_error(error), error); return true; }
            slot->connected = true;
            read_address(fd, false, &slot->local);
            read_address(fd, true, &slot->remote);
        }
        if (request->operation == SOCKET_DIAL) return true;
        socket_tls_error_t error = {0};
        if (!slot->tls) {
            if (engine->tls_count >= CONFIG_JS_NET_MAX_TLS_CONNECTIONS) {
                fail(request, "RESOURCE_LIMIT", 0);
                return true;
            }
            slot->tls = socket_tls_create(fd, request->server_name, request->ca, request->ca_size, &error);
            if (!slot->tls) {
                fail(request, error.code, error.system_error);
                request->tls_code = error.tls_code;
                return true;
            }
            ++engine->tls_count;
        }
        if (!tls_ready(request, fd, readers, writers)) return false;
        return tls_result(engine, request, socket_tls_handshake(slot->tls, &error), &error);
    }
    case SOCKET_ACCEPT: {
        if (!FD_ISSET(fd, readers)) return false;
        uint32_t generation;
        int index = socket_engine_reserve(engine, &generation);
        if (index < 0) { fail(request, "RESOURCE_LIMIT", 0); return true; }
        int accepted = accept(fd, NULL, NULL);
        if (accepted < 0) {
            int error = errno;
            socket_engine_release(engine, index);
            if (would_block()) return false;
            fail(request, io_error(error), error);
            return true;
        }
        if (!nonblocking(accepted)) {
            fail(request, "IO", errno);
            close(accepted);
            socket_engine_release(engine, index);
            return true;
        }
        socket_slot_t *peer = &engine->slots[index];
        peer->fd = accepted;
        read_address(accepted, false, &peer->local);
        read_address(accepted, true, &peer->remote);
        request->accepted_slot = index;
        request->accepted_generation = generation;
        return true;
    }
    case SOCKET_READ: {
        if (slot->eof) { request->eof = true; return true; }
        if (slot->tls) {
            if (!tls_ready(request, fd, readers, writers) &&
                (request->tls_wait == SOCKET_TLS_WANT_WRITE || !socket_tls_available(slot->tls))) return false;
            socket_tls_error_t error = {0};
            int count = socket_tls_read(slot->tls, request->data, request->size, &error);
            if (!tls_result(engine, request, count, &error)) return false;
            if (count == SOCKET_TLS_EOF) { slot->eof = true; request->eof = true; }
            else if (count > 0) request->transferred = count;
            return true;
        }
        if (!FD_ISSET(fd, readers)) return false;
        int count = recv(fd, request->data, request->size, 0);
        if (count < 0 && would_block()) return false;
        if (count < 0) {
            fail(request, io_error(errno), errno);
            close_slot(engine, request->slot);
        } else {
            request->transferred = count;
            request->eof = count == 0;
            slot->eof = request->eof;
        }
        return true;
    }
    case SOCKET_WRITE:
    case SOCKET_END:
        if (earlier_write(engine, request)) return false;
        if (slot->write_closed) { fail(request, "CLOSED", 0); return true; }
        if (request->operation == SOCKET_END) {
            if (slot->tls) {
                if (!tls_ready(request, fd, readers, writers)) return false;
                socket_tls_error_t error = {0};
                int result = socket_tls_end(slot->tls, &error);
                if (!tls_result(engine, request, result, &error)) return false;
                if (!request->error) slot->write_closed = true;
                return true;
            }
            if (shutdown(fd, SHUT_WR) < 0) {
                fail(request, io_error(errno), errno);
                close_slot(engine, request->slot);
            } else slot->write_closed = true;
            return true;
        }
        if (request->transferred == request->size) return true;
        if (slot->tls) {
            if (!tls_ready(request, fd, readers, writers)) return false;
            socket_tls_error_t error = {0};
            int count = socket_tls_write(slot->tls, request->data + request->transferred,
                                         request->size - request->transferred, &error);
            if (!tls_result(engine, request, count, &error)) return false;
            if (request->error) return true;
            request->transferred += count;
            return request->transferred == request->size;
        }
        if (!FD_ISSET(fd, writers)) return false;
        /* One send per turn keeps competing connections moving. */
        int count = send(fd, request->data + request->transferred,
                         request->size - request->transferred, 0);
        if (count < 0 && would_block()) return false;
        if (count <= 0) {
            fail(request, io_error(errno), errno);
            close_slot(engine, request->slot);
            return true;
        }
        request->transferred += count;
        return request->transferred == request->size;
    }
    return false;
}

static void worker_task(void *opaque)
{
    socket_engine_t *engine = opaque;
    while (!is_stopping(engine)) {
        socket_request_t *incoming;
        socket_request_t **tail = &engine->pending;
        while (*tail) tail = &(*tail)->next;
        while (xQueueReceive(engine->commands, &incoming, 0) == pdTRUE) {
            *tail = incoming;
            tail = &incoming->next;
        }
        collect_dns(engine);
        bool notify = uxQueueMessagesWaiting(engine->completions) != 0;
        for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) {
            portENTER_CRITICAL(&engine->lock);
            bool closing = engine->slots[i].used && engine->slots[i].closing &&
                           !engine->slots[i].closed;
            bool closed = engine->slots[i].used && engine->slots[i].closed;
            portEXIT_CRITICAL(&engine->lock);
            if (closing) close_slot(engine, i);
            notify |= closed || closing;
        }
        fd_set readers, writers;
        FD_ZERO(&readers);
        FD_ZERO(&writers);
        int maximum = -1;
        bool immediate = false;
        for (socket_request_t *request = engine->pending; request; request = request->next) {
            if (js_cancelled(request->cancel)) { immediate = true; continue; }
            if (!slot_valid(engine, request)) continue;
            if (!request->started) begin_request(engine, request);
            int fd = engine->slots[request->slot].fd;
            if (fd < 0 || request->error) continue;
            socket_slot_t *slot = &engine->slots[request->slot];
            if (slot->tls_busy && slot->tls_busy != request) continue;
            bool writing = request->operation == SOCKET_WRITE || request->operation == SOCKET_END;
            if (writing && earlier_write(engine, request)) continue;
            if (slot->tls) {
                if (request->tls_wait == SOCKET_TLS_WANT_READ) FD_SET(fd, &readers);
                else if (request->tls_wait == SOCKET_TLS_WANT_WRITE) FD_SET(fd, &writers);
                else immediate = true;
                if (request->operation == SOCKET_READ && request->tls_wait != SOCKET_TLS_WANT_WRITE &&
                    socket_tls_available(slot->tls)) immediate = true;
            } else {
                if (request->operation == SOCKET_READ || request->operation == SOCKET_ACCEPT)
                    FD_SET(fd, &readers);
                if (dialing(request) ||
                (request->operation == SOCKET_WRITE && !earlier_write(engine, request)))
                    FD_SET(fd, &writers);
            }
            if (fd > maximum) maximum = fd;
        }
        struct timeval wait = {.tv_usec = immediate ? 0 : WORKER_WAIT_MS * 1000};
        int ready = select(maximum + 1, &readers, &writers, NULL, &wait);
        int select_error = ready < 0 ? errno : 0;
        if (ready < 0) { FD_ZERO(&readers); FD_ZERO(&writers); }
        socket_request_t **link = &engine->pending;
        while (*link) {
            socket_request_t *request = *link;
            bool done = request->error != NULL;
            if (js_cancelled(request->cancel)) {
                fail(request, "ABORTED", 0);
                done = true;
            } else if (!slot_valid(engine, request)) {
                fail(request, "CLOSED", 0);
                done = true;
            } else if (!done && request->deadline_us &&
                       esp_timer_get_time() >= request->deadline_us) {
                fail(request, "TIMEOUT", ETIMEDOUT);
                done = true;
            } else if (!done && select_error && select_error != EINTR) {
                fail(request, "IO", select_error);
                done = true;
            } else if (!done) {
                done = advance_request(engine, request, &readers, &writers);
                if (done && !request->error && dialing(request) && request->deadline_us &&
                    esp_timer_get_time() >= request->deadline_us)
                    fail(request, "TIMEOUT", ETIMEDOUT);
            }
            if (!done) { link = &request->next; continue; }
            if (engine->slots[request->slot].tls_busy == request)
                engine->slots[request->slot].tls_busy = NULL;
            if (request->error && (dialing(request) ||
                request->operation == SOCKET_LISTEN || request->operation == SOCKET_WRITE ||
                request->operation == SOCKET_END)) close_slot(engine, request->slot);
            *link = request->next;
            request->next = NULL;
            /* Admission reserves completion capacity; never wait on the JS task. */
            xQueueSend(engine->completions, &request, 0);
            notify = true;
        }
        if (notify) engine->notify(engine->notify_opaque);
    }
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) {
        if (engine->slots[i].used) close_slot(engine, i);
    }
    xSemaphoreGive(engine->worker_stopped);
    vTaskDelete(NULL);
}

bool socket_engine_start(socket_engine_t *engine, void (*notify)(void *), void *opaque)
{
    *engine = (socket_engine_t){.lock = portMUX_INITIALIZER_UNLOCKED,
                                .notify = notify, .notify_opaque = opaque};
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) engine->slots[i].fd = -1;
    engine->commands = xQueueCreate(CONFIG_JS_NET_MAX_PENDING, sizeof(socket_request_t *));
    engine->completions = xQueueCreate(CONFIG_JS_NET_MAX_PENDING, sizeof(socket_request_t *));
    engine->dns_commands = xQueueCreate(CONFIG_JS_NET_MAX_CONNECTIONS + 1, sizeof(socket_dns_t));
    engine->dns_results = xQueueCreate(CONFIG_JS_NET_MAX_CONNECTIONS, sizeof(socket_dns_t));
    engine->worker_stopped = xSemaphoreCreateBinary();
    engine->resolver_stopped = xSemaphoreCreateBinary();
    if (!engine->commands || !engine->completions || !engine->dns_commands ||
        !engine->dns_results || !engine->worker_stopped || !engine->resolver_stopped) goto fail;
    if (xTaskCreate(resolver_task, "js_net_dns", WORKER_STACK_SIZE, engine, 5, NULL) != pdPASS)
        goto fail;
    if (xTaskCreate(worker_task, "js_net", WORKER_STACK_SIZE, engine, 5, NULL) != pdPASS) {
        socket_dns_t stop = {0};
        xQueueSend(engine->dns_commands, &stop, 0);
        xSemaphoreTake(engine->resolver_stopped, portMAX_DELAY);
        goto fail;
    }
    return true;
fail:
    if (engine->commands) vQueueDelete(engine->commands);
    if (engine->completions) vQueueDelete(engine->completions);
    if (engine->dns_commands) vQueueDelete(engine->dns_commands);
    if (engine->dns_results) vQueueDelete(engine->dns_results);
    if (engine->worker_stopped) vSemaphoreDelete(engine->worker_stopped);
    if (engine->resolver_stopped) vSemaphoreDelete(engine->resolver_stopped);
    memset(engine, 0, sizeof(*engine));
    return false;
}

void socket_engine_stop(socket_engine_t *engine)
{
    portENTER_CRITICAL(&engine->lock);
    engine->stopping = true;
    portEXIT_CRITICAL(&engine->lock);
    xSemaphoreTake(engine->worker_stopped, portMAX_DELAY);
    /* Discard queued DNS work; an in-flight libc resolver finishes independently. */
    socket_dns_t job;
    while (xQueueReceive(engine->dns_commands, &job, 0) == pdTRUE) {}
    job = (socket_dns_t){0};
    xQueueSend(engine->dns_commands, &job, 0);
    xSemaphoreTake(engine->resolver_stopped, portMAX_DELAY);
    vQueueDelete(engine->commands);
    vQueueDelete(engine->completions);
    vQueueDelete(engine->dns_commands);
    vQueueDelete(engine->dns_results);
    vSemaphoreDelete(engine->worker_stopped);
    vSemaphoreDelete(engine->resolver_stopped);
}
#endif
