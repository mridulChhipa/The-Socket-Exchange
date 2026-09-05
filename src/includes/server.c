#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "server.h"
#include "config.h"
#include "io.h"

void setNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1)
  {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

/*
Grows the table until client_fd is a valid index. The caller owns client_fd and
closes it when this fails, so nothing is closed here.
*/
bool growConns(struct ClientConnection ***conns, int *curr_cap, int client_fd)
{
  int new_cap = *curr_cap;
  while (client_fd >= new_cap)
    new_cap *= 2;

  printf("Growing connection table from %d to %d\n", *curr_cap, new_cap);

  struct ClientConnection **new_conns = realloc(*conns, new_cap * sizeof(struct ClientConnection *));
  if (new_conns == NULL)
  {
    printf("Failed to grow connection table\n");
    return false;
  }

  for (int j = *curr_cap; j < new_cap; j++)
    new_conns[j] = NULL;

  *conns = new_conns;
  *curr_cap = new_cap;

  return true;
}

/*
Allocates the connection state for an accepted socket and arms it on epoll.
On failure the slot is left NULL and client_fd is closed here, since the
half-registered fd is not the caller's to clean up.
*/
bool registerClient(int client_fd, int epoll_fd, struct ClientConnection **conns, const struct sockaddr_in *caddr, socklen_t caddr_len)
{
  conns[client_fd] = calloc(1, sizeof(struct ClientConnection));

  if (conns[client_fd] == NULL)
  {
    printf("Failed to allocate client connection\n");
    close(client_fd);
    return false;
  }

  conns[client_fd]->client_fd = client_fd;
  conns[client_fd]->logged_in = false;
  conns[client_fd]->caddr = *caddr;
  conns[client_fd]->caddr_len = caddr_len;

  struct epoll_event kev;
  kev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  kev.data.fd = client_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &kev) == -1)
  {
    printf("Failed to add client socket to epoll\n");

    free(conns[client_fd]);
    conns[client_fd] = NULL;
    close(client_fd);

    return false;
  }

  printf("Client connected: %s:%d\n", inet_ntoa(caddr->sin_addr), ntohs(caddr->sin_port));

  return true;
}

void acceptClients(int server_fd, int epoll_fd, struct ClientConnection ***conns, int *curr_cap)
{
  while (true)
  {
    struct sockaddr_in caddr;
    socklen_t caddr_len = sizeof(caddr);
    int client_fd = accept(server_fd, (struct sockaddr *)&caddr, &caddr_len);

    if (client_fd == -1)
    {
      /*
      An interrupted or aborted attempt is worth retrying; EAGAIN just means the backlog is drained, which is how this loop ends.
     */
      if (errno == EINTR || errno == ECONNABORTED)
        continue;

      if (errno != EAGAIN && errno != EWOULDBLOCK)
        printf("Failed to accept client connection: %s\n", strerror(errno));

      break;
    }

    setNonBlocking(client_fd);

    if (client_fd >= *curr_cap && !growConns(conns, curr_cap, client_fd))
    {
      close(client_fd);
      continue;
    }

    registerClient(client_fd, epoll_fd, *conns, &caddr, caddr_len);
  }
}

void handleClientEvent(int client_fd, uint32_t revents, int epoll_fd, struct ClientConnection **conns, int curr_cap)
{
  if (client_fd >= curr_cap || conns[client_fd] == NULL)
    return;

  struct ClientConnection *conn = conns[client_fd];
  bool done = (revents & (EPOLLERR | EPOLLHUP)) != 0;

  if (!done && (revents & EPOLLOUT))
    done = flushToClient(conn);

  if (!done && (revents & EPOLLIN))
    done = communicate(client_fd, conns, curr_cap);

  if (done)
  {
    printf("Removing client %d from epoll and closing connection\n", client_fd);
    removeClient(client_fd, epoll_fd, conns, curr_cap);
  }
}
