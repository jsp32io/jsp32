#pragma once
#include <stdbool.h>

// Probe without consuming bytes or generating network traffic.
bool reg_http_connection_alive(int socket);
