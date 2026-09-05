#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "orderbook.h"

#include "config.h"
#include "server.h"
#include "commands.h"
#include "io.h"

/*
Cleared by SIGINT/SIGTERM. epoll_wait is never restarted after a signal, so
the loop always gets a chance to notice this.
*/
static volatile sig_atomic_t running = 1;

void requestShutdown(int signum)
{
  (void)signum;
  running = 0;
}

int main(int argc, char *argv[])
{
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    printf("Failed to ignore SIGPIPE\n");
    return 1;
  }

  if (signal(SIGINT, requestShutdown) == SIG_ERR || signal(SIGTERM, requestShutdown) == SIG_ERR)
  {
    printf("Failed to install shutdown handler\n");
    return 1;
  }

  setvbuf(stdout, NULL, _IOLBF, 0);

  const char *host = (argc > 1) ? argv[1] : NULL;
  int port = PORT;

  if (argc > 2)
  {
    char *end;
    long value = strtol(argv[2], &end, 10);

    if (*end != '\0' || value <= 0 || value > 65535)
    {
      printf("Invalid port: %s\n", argv[2]);
      return 1;
    }

    port = value;
  }

  int server_fd;
  struct sockaddr_in server_addr;
  socklen_t server_addr_len = sizeof(server_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (host == NULL)
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1)
  {
    printf("Invalid bind address: %s\n", host);
    return 1;
  }

  int opt = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
  {
    printf("Failed to set socket options\n");
    return 1;
  }

  setNonBlocking(server_fd);

  printf("Socket options set successfully\n");

  if (bind(server_fd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
  {
    printf("Failed to bind socket\n");
    return 1;
  }

  printf("Socket bound successfully\n");

  if (listen(server_fd, SOMAXCONN) == -1)
  {
    printf("Failed to listen on socket\n");
    return 1;
  }

  printf("Listening on %s:%d\n", host ? host : "0.0.0.0", port);

  // From here we can set up epoll to handle multiple client connections efficiently.

  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1)
  {
    printf("Failed to create epoll instance\n");
    return 1;
  }

  printf("Epoll instance created successfully\n");

  struct epoll_event kev;
  kev.events = EPOLLIN;
  kev.data.fd = server_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &kev) == -1)
  {
    printf("Failed to add server socket to epoll\n");
    return 1;
  }

  printf("Server socket added to epoll successfully\n");

  struct epoll_event events[MAX_EVENTS];
  int curr_cap = MIN_CAPACITY;

  struct ClientConnection **conns = calloc(curr_cap, sizeof(struct ClientConnection *));

  if (conns == NULL)
  {
    printf("Failed to allocate connection table\n");
    return 1;
  }

  while (running)
  {
    printf("Waiting for events...\n");
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    if (nfds == -1)
    {
      if (errno == EINTR)
        continue;

      printf("Failed to wait for events: %s\n", strerror(errno));
      break;
    }

    printf("Number of events: %d\n", nfds);
    for (int i = 0; i < nfds; i++)
    {
      int curr_fd = events[i].data.fd;
      if (curr_fd == server_fd)
      {
        acceptClients(server_fd, epoll_fd, &conns, &curr_cap);
      }
      else
      {
        handleClientEvent(curr_fd, events[i].events, epoll_fd, conns, curr_cap);
      }
    }
  }

  printf("Shutting down, closing %d connection slots\n", curr_cap);

  for (int i = 0; i < curr_cap; i++)
  {
    if (conns[i] != NULL)
      removeClient(i, epoll_fd, conns, curr_cap);
  }

  free(conns);

  close(server_fd);
  close(epoll_fd);

  printf("Exchange server closed successfully\n");

  return 0;
}
