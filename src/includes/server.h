#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>

#include "config.h"
#include "orderbook.h"

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

void acceptClients(int server_fd, int kq, struct ClientConnection ***conns, int *curr_cap);

bool growConns(struct ClientConnection ***conns, int *curr_cap, int client_fd);

bool registerClient(int client_fd, int kq, struct ClientConnection **conns, const struct sockaddr_in *caddr, socklen_t caddr_len);

/*
Takes the whole kevent rather than a flag word: kqueue splits readability and
writability into separate filters, so which filter fired is as much a part of
the event as the flags are.
*/
void handleClientEvent(const struct kevent *ev, int kq, struct ClientConnection **conns, int curr_cap, struct LimitOrderBook *orderbook);

#endif
