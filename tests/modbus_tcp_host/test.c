#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "nvs.h"
#include "modbus_tcp.h"

struct app_registers { uint8_t x[8], y[8], m[8]; uint16_t d[10]; };
static uint8_t saved_enabled = 1;
static uint16_t saved_port = 502;
static size_t d_capacity = 10;

const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "jsp32_config") == 0 && mode == NVS_READONLY);
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle; assert(strcmp(key, "modbus_enabled") == 0);
    *out = saved_enabled; return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out)
{
    (void)handle; assert(strcmp(key, "modbus_port") == 0);
    *out = saved_port; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
int64_t esp_timer_get_time(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
typedef struct { void (*function)(void *); void *argument; } task_t;
static void *task_entry(void *argument)
{
    task_t *task = argument;
    void (*function)(void *) = task->function;
    void *data = task->argument;
    free(task); function(data); return NULL;
}
int xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
                void *argument, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    task_t *task = malloc(sizeof(*task));
    assert(task); *task = (task_t){function, argument};
    pthread_t thread;
    if (pthread_create(&thread, NULL, task_entry, task) != 0) {
        free(task); return 0;
    }
    pthread_detach(thread); return 1;
}
void vTaskDelay(unsigned ms) { usleep(ms * 1000); }

size_t app_registers_area_count(app_register_area_t area)
{
    return area == APP_REGISTER_AREA_D ? d_capacity : 8;
}
esp_err_t app_registers_read_bits(app_registers_t *r, app_register_address_t a,
                                  uint8_t *out, size_t count, uint64_t *revision)
{
    (void)revision;
    uint8_t *source = a.area == APP_REGISTER_AREA_X ? r->x :
                      a.area == APP_REGISTER_AREA_Y ? r->y : r->m;
    memcpy(out, source + a.index, count);
    return ESP_OK;
}
esp_err_t app_registers_write_bits(app_registers_t *r, app_register_address_t a,
                                   const uint8_t *data, size_t count,
                                   app_register_source_t source, uint64_t *revision)
{
    (void)revision; assert(source == APP_REGISTER_SOURCE_MODBUS);
    assert(a.area != APP_REGISTER_AREA_X);
    memcpy((a.area == APP_REGISTER_AREA_Y ? r->y : r->m) + a.index, data, count);
    return ESP_OK;
}
esp_err_t app_registers_read_words(app_registers_t *r, app_register_address_t a,
                                   uint16_t *out, size_t count, uint64_t *revision)
{
    (void)revision; memcpy(out, r->d + a.index, count * sizeof(*out));
    return ESP_OK;
}
esp_err_t app_registers_write_words(app_registers_t *r, app_register_address_t a,
                                    const uint16_t *data, size_t count,
                                    app_register_source_t source, uint64_t *revision)
{
    (void)revision; assert(source == APP_REGISTER_SOURCE_MODBUS);
    assert(a.index >= 5);
    memcpy(r->d + a.index, data, count * sizeof(*data));
    return ESP_OK;
}

static size_t request(uint8_t *frame, uint16_t transaction, uint8_t unit,
                      const uint8_t *pdu, size_t pdu_len)
{
    frame[0] = transaction >> 8; frame[1] = transaction;
    frame[2] = frame[3] = 0;
    frame[4] = (pdu_len + 1) >> 8; frame[5] = pdu_len + 1;
    frame[6] = unit;
    memcpy(frame + 7, pdu, pdu_len);
    return pdu_len + 7;
}

int main(void)
{
    app_registers_t image = {0};
    image.x[0] = 1; image.y[0] = 1; image.d[0] = 0x1234;
    image.d[5] = 0xabcd;
    uint8_t frame[MODBUS_TCP_MAX_ADU], reply[MODBUS_TCP_MAX_ADU];
    size_t size = request(frame, 0x1234, 0xff,
                          (uint8_t[]){4, 0, 0, 0, 1}, 5);
    size_t answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 11 && reply[0] == 0x12 && reply[1] == 0x34);
    assert(reply[5] == 5 && reply[6] == 0xff && reply[7] == 4);
    assert(reply[9] == 0x12 && reply[10] == 0x34);

    size = request(frame, 1, 1, (uint8_t[]){3, 0, 0, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 11 && reply[9] == 0xab && reply[10] == 0xcd);
    size = request(frame, 2, 1, (uint8_t[]){6, 0, 0, 0x22, 0x33}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 12 && image.d[5] == 0x2233);
    size = request(frame, 3, 1, (uint8_t[]){4, 0, 5, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 9 && reply[7] == 0x84 && reply[8] == 2);
    size = request(frame, 4, 1, (uint8_t[]){6, 0, 5, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 9 && reply[7] == 0x86 && reply[8] == 2);

    size = request(frame, 5, 1, (uint8_t[]){2, 0, 0, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 10 && reply[7] == 2 && reply[9] == 1);
    size = request(frame, 6, 1, (uint8_t[]){5, 0, 8, 0xff, 0}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 12 && image.m[0] == 1);
    size = request(frame, 7, 1, (uint8_t[]){15, 0, 0, 0, 3, 1, 5}, 7);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 12 && image.y[0] == 1 && image.y[1] == 0 && image.y[2] == 1);
    size = request(frame, 8, 1, (uint8_t[]){16, 0, 1, 0, 2, 4, 0x12, 0x34, 0x56, 0x78}, 10);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 12 && image.d[6] == 0x1234 && image.d[7] == 0x5678);

    frame[2] = 1;
    assert(modbus_tcp_process(frame, size, reply, &image) == 0);
    frame[2] = 0; frame[5]--;
    assert(modbus_tcp_process(frame, size, reply, &image) == 0);

    d_capacity = 5;
    image.d[2] = 0x789a;
    size = request(frame, 9, 1, (uint8_t[]){3, 0, 0, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 11 && reply[9] == 0x78 && reply[10] == 0x9a);
    size = request(frame, 10, 1, (uint8_t[]){4, 0, 2, 0, 1}, 5);
    answer = modbus_tcp_process(frame, size, reply, &image);
    assert(answer == 9 && reply[8] == 2);
    d_capacity = 10;

    modbus_tcp_t *server = NULL;
    saved_enabled = 0;
    assert(modbus_tcp_start(&image, &server) == ESP_OK && server == NULL);
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe >= 0);
    struct sockaddr_in endpoint = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(probe, (struct sockaddr *)&endpoint, sizeof(endpoint)) == 0);
    socklen_t address_length = sizeof(endpoint);
    assert(getsockname(probe, (struct sockaddr *)&endpoint, &address_length) == 0);
    saved_port = ntohs(endpoint.sin_port);
    close(probe);
    saved_enabled = 1;
    esp_err_t start_result = modbus_tcp_start(&image, &server);
    if (start_result != ESP_OK) fprintf(stderr, "server start failed: %d, errno %d\n", start_result, errno);
    assert(start_result == ESP_OK && server != NULL);
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    endpoint.sin_port = htons(saved_port);
    assert(connect(socket_fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == 0);
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    size = request(frame, 0x789a, 1, (uint8_t[]){4, 0, 0, 0, 1}, 5);
    assert(send(socket_fd, frame, 3, 0) == 3);
    assert(send(socket_fd, frame + 3, size - 3, 0) == (ssize_t)size - 3);
    size_t received = 0;
    while (received < 11) {
        int count = recv(socket_fd, reply + received, 11 - received, 0);
        assert(count > 0); received += (size_t)count;
    }
    assert(reply[0] == 0x78 && reply[1] == 0x9a && reply[9] == 0x12);
    size = request(frame, 11, 1, (uint8_t[]){4, 0, 0, 0, 1}, 5);
    memcpy(frame + size, frame, size);
    assert(send(socket_fd, frame, size * 2, 0) == (ssize_t)size * 2);
    received = 0;
    while (received < 22) {
        int count = recv(socket_fd, reply + received, 22 - received, 0);
        assert(count > 0); received += (size_t)count;
    }
    assert(reply[0] == 0 && reply[1] == 11 && reply[11] == 0 && reply[12] == 11);
    close(socket_fd);
    puts("Modbus TCP protocol tests passed");
}
