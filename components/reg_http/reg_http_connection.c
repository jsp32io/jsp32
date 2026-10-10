#include "reg_http_connection.h"
#include <errno.h>
#include <sys/socket.h>

bool reg_http_connection_alive(int socket)
{
    char byte;
    int result = recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result > 0) return true;
    if (result == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
