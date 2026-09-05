#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <stdlib.h>

#include "server.h"
#include "config.h"

bool flushToClient(struct ClientConnection *conn);
bool sendToClient(struct ClientConnection *conn, const char *data, size_t len);
bool sendLine(struct ClientConnection *conn, const char *line);

bool replyError(struct ClientConnection *conn, const char *reason);
void removeClient(int client_fd, int epoll_fd, struct ClientConnection **conns, int curr_cap);
bool processLine(char *line, struct ClientConnection *conn, struct ClientConnection **conns, int curr_cap);
bool communicate(int client_fd, struct ClientConnection **conns, int curr_cap);

#endif