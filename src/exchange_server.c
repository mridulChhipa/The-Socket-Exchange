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
#include <sys/event.h>
#include <sys/time.h>

#include "orderbook.h"

#include "config.h"
#include "server.h"
#include "commands.h"
#include "io.h"

/*
Cleared by SIGINT/SIGTERM. kevent is never restarted after a signal, so the
loop always gets a chance to notice this.
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

  struct LimitOrderBook orderbook;
  initOrderbook(&orderbook);

  // From here we can set up kqueue to handle multiple client connections efficiently.

  int kq = kqueue();
  if (kq == -1)
  {
    printf("Failed to create kqueue instance\n");
    return 1;
  }

  printf("Kqueue instance created successfully\n");

  struct kevent kev;
  EV_SET(&kev, server_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);

  if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
  {
    printf("Failed to add server socket to kqueue\n");
    return 1;
  }

  printf("Server socket added to kqueue successfully\n");

  struct kevent events[MAX_EVENTS];
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
    int nfds = kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);

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
      int curr_fd = (int)events[i].ident;
      if (curr_fd == server_fd)
      {
        acceptClients(server_fd, kq, &conns, &curr_cap);
      }
      else
      {
        handleClientEvent(&events[i], kq, conns, curr_cap, &orderbook);
      }
    }
  }

  printf("Shutting down, closing %d connection slots\n", curr_cap);

  for (int i = 0; i < curr_cap; i++)
  {
    if (conns[i] != NULL)
      removeClient(i, kq, conns, curr_cap, &orderbook);
  }

  free(conns);

  close(server_fd);
  close(kq);

  printf("Exchange server closed successfully\n");

  return 0;
}
