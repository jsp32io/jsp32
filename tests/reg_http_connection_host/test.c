#include "reg_http_connection.h"
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(reg_http_connection_alive(sockets[0])); // Idle is alive, probe must not block.
    assert(send(sockets[1], "x", 1, 0) == 1);
    assert(reg_http_connection_alive(sockets[0]));
    char byte;
    assert(recv(sockets[0], &byte, 1, 0) == 1 && byte == 'x'); // Probe did not consume data.
    close(sockets[1]);
    assert(!reg_http_connection_alive(sockets[0])); // FIN without a server write.
    close(sockets[0]);
    assert(!reg_http_connection_alive(-1));
    return 0;
}
