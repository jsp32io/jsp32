#include "network_manager_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define NETWORK_OWNER "network"
#define SOFTAP_IPV4 0xc0a80401u

static const char *mode_name(network_mode_t mode)
{
    switch (mode) {
    case NETWORK_MODE_AP: return "ap";
    case NETWORK_MODE_STA: return "sta";
    case NETWORK_MODE_APSTA: return "apsta";
    default: return "unknown";
    }
}

static const char *state_name(network_state_t state)
{
    switch (state) {
    case NETWORK_STATE_INIT: return "initializing";
    case NETWORK_STATE_AP_READY: return "ready";
    case NETWORK_STATE_STA_CONNECTING: return "connecting";
    case NETWORK_STATE_ONLINE: return "online";
    case NETWORK_STATE_APPLYING: return "applying";
    case NETWORK_STATE_FALLBACK: return "fallback";
    case NETWORK_STATE_ERROR: return "error";
    case NETWORK_STATE_STOPPING: return "stopping";
    default: return "unknown";
    }
}

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
    char *body = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, body);
    cJSON_free(body);
    return err;
}

static esp_err_t busy_response(httpd_req_t *request, const char *message)
{
    httpd_resp_set_status(request, "409 Conflict");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static cJSON *receive_json(httpd_req_t *request)
{
    if (!request->content_len || request->content_len > CONFIG_NETWORK_HTTP_MAX_BODY) return NULL;
    char body[CONFIG_NETWORK_HTTP_MAX_BODY + 1];
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (count <= 0) { network_secure_clear(body, sizeof(body)); return NULL; }
        timeouts = 0;
        received += count;
    }
    body[received] = '\0';
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(body, received + 1, &end, true);
    network_secure_clear(body, sizeof(body));
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return NULL; }
    return json;
}

static void clear_json_passwords(cJSON *json)
{
    const char *names[] = {"ap", "station"};
    for (size_t i = 0; i < 2; ++i) {
        cJSON *password = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, names[i]), "password");
        if (cJSON_IsString(password)) network_secure_clear(password->valuestring, strlen(password->valuestring));
    }
}

static esp_err_t config_handler(httpd_req_t *request)
{
    network_settings_t settings;
    network_manager_get_settings(request->user_ctx, &settings);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "mode", mode_name(settings.mode));
        cJSON *ap = cJSON_AddObjectToObject(root, "ap");
        cJSON_AddStringToObject(ap, "ssid", settings.ap_ssid);
        cJSON_AddBoolToObject(ap, "open", !settings.ap_password[0]);
        cJSON_AddBoolToObject(ap, "passwordSet", settings.ap_password[0] != 0);
        cJSON *station = cJSON_AddObjectToObject(root, "station");
        cJSON_AddStringToObject(station, "ssid", settings.sta_ssid);
        cJSON_AddBoolToObject(station, "passwordSet", settings.sta_password[0] != 0);
    }
    network_secure_clear(&settings, sizeof(settings));
    return send_json(request, root);
}

/* A missing password retains it; an explicit empty STA password selects open Wi-Fi. */
static bool parse_settings(cJSON *json, network_settings_t *settings)
{
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
    if (!cJSON_IsString(mode)) return false;
    if (strcmp(mode->valuestring, "ap") == 0) settings->mode = NETWORK_MODE_AP;
    else if (strcmp(mode->valuestring, "sta") == 0) settings->mode = NETWORK_MODE_STA;
    else if (strcmp(mode->valuestring, "apsta") == 0) settings->mode = NETWORK_MODE_APSTA;
    else return false;
    cJSON *ap = cJSON_GetObjectItemCaseSensitive(json, "ap");
    if (ap) {
        if (!cJSON_IsObject(ap)) return false;
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(ap, "ssid");
        cJSON *open = cJSON_GetObjectItemCaseSensitive(ap, "open");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(ap, "password");
        if (ssid) {
            if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32) return false;
            strlcpy(settings->ap_ssid, ssid->valuestring, sizeof(settings->ap_ssid));
        }
        if (open && !cJSON_IsBool(open)) return false;
        if (password) {
            if (!cJSON_IsString(password) || strlen(password->valuestring) > 63) return false;
            strlcpy(settings->ap_password, password->valuestring, sizeof(settings->ap_password));
        }
        if (cJSON_IsTrue(open)) network_secure_clear(settings->ap_password, sizeof(settings->ap_password));
        if (cJSON_IsFalse(open) && !settings->ap_password[0]) return false;
    }
    cJSON *station = cJSON_GetObjectItemCaseSensitive(json, "station");
    if (station) {
        if (!cJSON_IsObject(station)) return false;
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(station, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(station, "password");
        if (ssid) {
            if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) > 32) return false;
            if (strcmp(settings->sta_ssid, ssid->valuestring) != 0 && !password) return false;
            strlcpy(settings->sta_ssid, ssid->valuestring, sizeof(settings->sta_ssid));
        }
        if (password) {
            if (!cJSON_IsString(password) || strlen(password->valuestring) > 64) return false;
            strlcpy(settings->sta_password, password->valuestring, sizeof(settings->sta_password));
        }
    }
    return network_settings_valid(settings);
}

static esp_err_t apply_handler(httpd_req_t *request)
{
    cJSON *json = receive_json(request);
    network_settings_t settings;
    network_manager_get_settings(request->user_ctx, &settings);
    bool valid = json && parse_settings(json, &settings);
    clear_json_passwords(json);
    cJSON_Delete(json);
    if (!valid) {
        network_secure_clear(&settings, sizeof(settings));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid mode, SSID or password");
    }
    uint32_t attempt = 0;
    esp_err_t err = network_manager_apply_deferred(request->user_ctx, &settings, &attempt);
    network_secure_clear(&settings, sizeof(settings));
    if (err != ESP_OK) return busy_response(request, "Network change or scan already in progress");
    httpd_resp_set_status(request, "202 Accepted");
    cJSON *response = cJSON_CreateObject();
    if (response) cJSON_AddNumberToObject(response, "attemptId", attempt);
    esp_err_t send_err = send_json(request, response);
    /* The old interface stays up until the HTTP response has been sent. */
    network_manager_release_apply(request->user_ctx);
    return send_err;
}
static void add_ip_info(cJSON *network, const char *name, const char *if_key)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return;
    }
    cJSON *item = cJSON_AddObjectToObject(network, name);
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        char address[16];
        snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
        cJSON_AddStringToObject(item, "ip", address);
        snprintf(address, sizeof(address), IPSTR, IP2STR(&info.netmask));
        cJSON_AddStringToObject(item, "netmask", address);
        snprintf(address, sizeof(address), IPSTR, IP2STR(&info.gw));
        cJSON_AddStringToObject(item, "gateway", address);
    }
    const char *hostname = NULL;
    if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname) {
        cJSON_AddStringToObject(item, "hostname", hostname);
    }
}

static void add_station_signal(cJSON *network)
{
    cJSON *station = cJSON_GetObjectItemCaseSensitive(network, "station");
    if (!station) return;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        cJSON_AddNullToObject(station, "ssid");
        cJSON_AddNullToObject(station, "rssi");
        return;
    }
    char ssid[sizeof(ap.ssid) + 1];
    memcpy(ssid, ap.ssid, sizeof(ap.ssid));
    ssid[sizeof(ap.ssid)] = '\0';
    cJSON_AddStringToObject(station, "ssid", ssid);
    cJSON_AddNumberToObject(station, "rssi", ap.rssi);
}

static void add_wifi_tx_power(cJSON *network)
{
    int8_t power;
    if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
        cJSON_AddNumberToObject(network, "wifiMaxTxPowerQdbm", power);
        cJSON_AddNumberToObject(network, "wifiMaxTxPowerDbm", power / 4.0);
    } else {
        cJSON_AddNullToObject(network, "wifiMaxTxPowerQdbm");
        cJSON_AddNullToObject(network, "wifiMaxTxPowerDbm");
    }
}


static void add_network_info(cJSON *network)
{
    add_ip_info(network, "station", "WIFI_STA_DEF");
    add_station_signal(network);
    add_ip_info(network, "softAp", "WIFI_AP_DEF");
    add_wifi_tx_power(network);
}

static void add_status(cJSON *network, network_manager_t *manager)
{
    network_status_t status;
    network_manager_get_status(manager, &status);
    cJSON_AddStringToObject(network, "mode", mode_name(status.active_mode));
    cJSON_AddStringToObject(network, "savedMode", mode_name(status.saved_mode));
    cJSON_AddStringToObject(network, "state", state_name(status.state));
    cJSON_AddBoolToObject(network, "modeSaved", status.mode_saved);
    cJSON_AddBoolToObject(network, "pending", status.pending);
    cJSON_AddBoolToObject(network, "scanning", status.scanning);
    cJSON_AddBoolToObject(network, "fallback", status.fallback);
    cJSON_AddNumberToObject(network, "attemptId", status.attempt_id);
    cJSON_AddNumberToObject(network, "disconnectReason", status.disconnect_reason);
    cJSON_AddNumberToObject(network, "errorCode", status.last_error);
    if (status.last_error != ESP_OK) cJSON_AddStringToObject(network, "error", esp_err_to_name(status.last_error));
    add_network_info(network);
    cJSON *station = cJSON_GetObjectItemCaseSensitive(network, "station");
    cJSON *ap = cJSON_GetObjectItemCaseSensitive(network, "softAp");
    cJSON_AddBoolToObject(station, "enabled", true);
    cJSON_AddBoolToObject(station, "connectionEnabled", status.active_mode != NETWORK_MODE_AP);
    cJSON_AddBoolToObject(station, "connected", status.has_ipv4);
    cJSON_AddBoolToObject(ap, "enabled", status.active_mode != NETWORK_MODE_STA);
    wifi_config_t config = {0};
    if (status.active_mode != NETWORK_MODE_STA && esp_wifi_get_config(WIFI_IF_AP, &config) == ESP_OK) {
        char ssid[33] = {0};
        memcpy(ssid, config.ap.ssid, 32);
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddBoolToObject(ap, "open", config.ap.authmode == WIFI_AUTH_OPEN);
    }
    network_secure_clear(&config, sizeof(config));
    wifi_sta_list_t clients = {0};
    if (status.active_mode != NETWORK_MODE_STA && esp_wifi_ap_get_sta_list(&clients) == ESP_OK)
        cJSON_AddNumberToObject(ap, "clients", clients.num);
}

static void network_system_info(cJSON *root, void *context)
{
    (void)context;
    add_network_info(cJSON_GetObjectItemCaseSensitive(root, "network"));
}

static esp_err_t status_handler(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root) add_status(root, request->user_ctx);
    return send_json(request, root);
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    if (network_manager_scan(request->user_ctx) != ESP_OK)
        return busy_response(request, "Network is busy applying settings, connecting or scanning");
    httpd_resp_set_status(request, "202 Accepted");
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddStringToObject(root, "status", "scanning");
    return send_json(request, root);
}

static esp_err_t networks_handler(httpd_req_t *request)
{
    network_scan_result_t results[CONFIG_NETWORK_SCAN_MAX_RESULTS];
    size_t count = network_manager_copy_scan_results(request->user_ctx, results, CONFIG_NETWORK_SCAN_MAX_RESULTS);
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < count && networks; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(item, "channel", results[i].channel);
        cJSON_AddBoolToObject(item, "open", results[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, item);
    }
    return send_json(request, root);
}

static bool is_ap_request(httpd_req_t *request)
{
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    return getsockname(httpd_req_to_sockfd(request), (struct sockaddr *)&local, &size) == 0 &&
           local.sin_family == AF_INET && ntohl(local.sin_addr.s_addr) == SOFTAP_IPV4;
}

static esp_err_t redirect_handler(httpd_req_t *request)
{
    network_status_t status;
    network_manager_get_status(request->user_ctx, &status);
    if (status.active_mode == NETWORK_MODE_STA || !is_ap_request(request))
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
    httpd_resp_set_status(request, "302 Found");
#if CONFIG_APP_HTTP_SERVER_PORT == 80
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
#else
    char location[64];
    snprintf(location, sizeof(location), "http://192.168.4.1:%d/", CONFIG_APP_HTTP_SERVER_PORT);
    httpd_resp_set_hdr(request, "Location", location);
#endif
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t system_tx_power_handler(httpd_req_t *request)
{
    char body[129];
    if (request->content_len == 0 || request->content_len >= sizeof(body)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid request body");
    }
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (count <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                       "Invalid request body");
        }
        timeouts = 0;
        received += (size_t)count;
    }
    cJSON *json = cJSON_ParseWithLength(body, received);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "maxTxPowerQdbm");
    if (!cJSON_IsObject(json) || !cJSON_IsNumber(value) ||
        !isfinite(value->valuedouble) ||
        value->valuedouble < 8 || value->valuedouble > 84 ||
        floor(value->valuedouble) != value->valuedouble) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "maxTxPowerQdbm must be an integer from 8 to 84");
    }
    int8_t power = (int8_t)value->valuedouble;
    cJSON_Delete(json);
    esp_err_t err = network_manager_set_tx_power(request->user_ctx, &power);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Failed to apply or save Wi-Fi transmit power");
    }
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    cJSON_AddNumberToObject(response, "maxTxPowerQdbm", power);
    cJSON_AddNumberToObject(response, "maxTxPowerDbm", power / 4.0);
    char *result = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!result) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    err = httpd_resp_sendstr(request, result);
    cJSON_free(result);
    return err;
}


esp_err_t network_http_start(network_manager_t *manager)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/api/v1/network/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = manager},
        {.uri = "/api/v1/network/config", .method = HTTP_GET, .handler = config_handler, .user_ctx = manager},
        {.uri = "/api/v1/network/config", .method = HTTP_PUT, .handler = apply_handler, .user_ctx = manager},
        {.uri = "/api/v1/network/scan", .method = HTTP_POST, .handler = scan_handler, .user_ctx = manager},
        {.uri = "/api/v1/network/networks", .method = HTTP_GET, .handler = networks_handler, .user_ctx = manager},
        {.uri = "/api/v1/system/tx-power", .method = HTTP_POST, .handler = system_tx_power_handler, .user_ctx = manager},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = manager},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = manager},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = manager},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = manager},
    };
    esp_err_t err = app_http_server_register(manager->http_server, NETWORK_OWNER, handlers, sizeof(handlers) / sizeof(handlers[0]));
    if (err == ESP_OK) err = app_http_server_register_info_provider(manager->http_server, NETWORK_OWNER, network_system_info, manager);
    if (err != ESP_OK) app_http_server_unregister_owner(manager->http_server, NETWORK_OWNER);
    return err;
}

void network_http_stop(network_manager_t *manager)
{
    app_http_server_unregister_owner(manager->http_server, NETWORK_OWNER);
}
