#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "config.h"

enum Type
{
  none,
  trader,
  market
};

struct ClientConnection
{
  char *outbuf;
  size_t inlen;
  size_t scanned;
  size_t outlen;
  struct sockaddr_in caddr;
  socklen_t caddr_len;
  int client_fd;
  enum Type type;
  bool logged_in;
  bool discarding;
  bool closing;
  bool subscribedJNST;
  bool subscribedIMCT;
  char username[USERNAME_LENGTH];
  char inbuf[INBUF_SIZE];
};

void setNonBlocking(int fd);

void acceptClients(int server_fd, int epoll_fd, struct ClientConnection ***conns, int *curr_cap);

bool growConns(struct ClientConnection ***conns, int *curr_cap, int client_fd);

bool registerClient(int client_fd, int epoll_fd, struct ClientConnection **conns, const struct sockaddr_in *caddr, socklen_t caddr_len);

void handleClientEvent(int client_fd, uint32_t revents, int epoll_fd, struct ClientConnection **conns, int curr_cap);

#endif
