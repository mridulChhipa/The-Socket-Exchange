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

#define PORT 8080
#define USERNAME_LENGTH 128
#define MIN_CAPACITY 10
#define MAX_EVENTS 64

#define MAX_CMD_LEN 11
#define MAX_INSTRUMENT_LEN 4
#define MAX_INT_DIGITS 10

#define MAX_LINE_LEN (MAX_CMD_LEN + 1 + (USERNAME_LENGTH - 1) + 1)

#define MAX_TOKEN_LEN 159
#define INBUF_SIZE (MAX_TOKEN_LEN + 1)

#define OUTBUF_SIZE 4096

struct ClientConnection
{
  socklen_t caddr_len;
  int client_fd;
  struct sockaddr_in caddr;
  bool logged_in;
  char username[USERNAME_LENGTH];
  char inbuf[INBUF_SIZE];
  size_t inlen;
  size_t scanned;
  bool discarding;
  char *outbuf;
  size_t outlen;
  bool closing;
};

void setNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1)
  {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

bool flushToClient(struct ClientConnection *conn)
{
  while (conn->outlen > 0)
  {
    ssize_t written = write(conn->client_fd, conn->outbuf, conn->outlen);

    if (written == -1)
    {
      if (errno == EINTR)
        continue;

      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return false;

      return true;
    }

    conn->outlen -= written;
    memmove(conn->outbuf, conn->outbuf + written, conn->outlen);
  }

  return conn->closing;
}

bool sendToClient(struct ClientConnection *conn, const char *data, size_t len)
{
  if (conn->outlen == 0)
  {
    ssize_t written;

    do
    {
      written = write(conn->client_fd, data, len);
    } while (written == -1 && errno == EINTR);

    if (written == -1)
    {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        return true;

      written = 0;
    }

    data += written;
    len -= written;
  }

  if (len == 0)
    return false;

  if (conn->outbuf == NULL && (conn->outbuf = malloc(OUTBUF_SIZE)) == NULL)
    return true;

  if (conn->outlen + len > OUTBUF_SIZE)
    return true;

  memcpy(conn->outbuf + conn->outlen, data, len);
  conn->outlen += len;

  return false;
}

bool sendLine(struct ClientConnection *conn, const char *line)
{
  return sendToClient(conn, line, strlen(line));
}

bool replyError(struct ClientConnection *conn, const char *reason)
{
  char response[64];

  snprintf(response, sizeof(response), "ERROR %s\n", reason);

  return sendLine(conn, response);
}

void removeClient(int client_fd, int epoll_fd, struct ClientConnection **conns, int curr_cap)
{
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL) == -1)
  {
    printf("Failed to remove client socket from epoll\n");
  }

  if (client_fd < curr_cap && conns[client_fd] != NULL)
  {
    struct ClientConnection *conn = conns[client_fd];

    // A client that asked to leave gets an explicit FIN now that its final
    // reply has gone out. An abrupt teardown just closes.
    if (conn->closing)
      shutdown(client_fd, SHUT_WR);

    if (conn->logged_in)
      printf("Client %s:%d (user %s) disconnected\n", inet_ntoa(conn->caddr.sin_addr), ntohs(conn->caddr.sin_port), conn->username);
    else
      printf("Client %s:%d disconnected\n", inet_ntoa(conn->caddr.sin_addr), ntohs(conn->caddr.sin_port));

    free(conn->outbuf);
    free(conn);
    conns[client_fd] = NULL;
  }

  // Released last, so the descriptor number cannot be recycled by an accept()
  // while it is still being used to index the connection table.
  close(client_fd);
}

bool loginUser(const char *username, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap)
{
  if (self->logged_in)
    return replyError(self, "Already logged in");

  for (int i = 0; i < curr_cap; i++)
  {
    if (conns[i] != NULL && conns[i]->logged_in && strcmp(conns[i]->username, username) == 0)
      return replyError(self, "Username already in use");
  }

  strncpy(self->username, username, USERNAME_LENGTH - 1);
  self->username[USERNAME_LENGTH - 1] = '\0';
  self->logged_in = true;

  printf("User %s logged in from %s:%d\n", self->username, inet_ntoa(self->caddr.sin_addr), ntohs(self->caddr.sin_port));

  return sendLine(self, "OK\n");
}

bool quitUser(struct ClientConnection *self)
{
  self->closing = true;

  return sendLine(self, "OK\n") || self->outlen == 0;
}

bool processLine(char *line, struct ClientConnection *conn, struct ClientConnection **conns, int curr_cap)
{
  char cmd[INBUF_SIZE], arg[INBUF_SIZE];

  int matched = sscanf(line, "%159s %159s", cmd, arg);

  if (matched < 1)
    return false;

  printf("Received from client %d: %s\n", conn->client_fd, line);

  if (strlen(cmd) > (size_t)MAX_CMD_LEN)
    return replyError(conn, "Unknown command");

  if (strcmp(cmd, "LOGIN") == 0 && matched == 2)
  {
    if (strlen(arg) > (size_t)(USERNAME_LENGTH - 1))
      return replyError(conn, "Username too long");

    return loginUser(arg, conn, conns, curr_cap);
  }

  if (strcmp(cmd, "QUIT") == 0 && matched == 1)
    return quitUser(conn);

  return replyError(conn, "Unknown command");
}

bool communicate(int client_fd, struct ClientConnection **conns, int curr_cap)
{
  struct ClientConnection *self = conns[client_fd];

  if (self->closing)
    return false;

  while (true)
  {
    ssize_t bytes_read = read(client_fd, self->inbuf + self->inlen, sizeof(self->inbuf) - self->inlen);
    if (bytes_read == -1)
    {
      if (errno == EINTR)
        continue;

      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return false;

      printf("Failed to read from client\n");
      return true;
    }
    else if (bytes_read == 0)
    {
      printf("Client disconnected\n");
      return true;
    }

    self->inlen += bytes_read;

    size_t consumed = 0, search = self->scanned;
    char *newline;

    while ((newline = memchr(self->inbuf + search, '\n', self->inlen - search)) != NULL)
    {
      char *line = self->inbuf + consumed;
      size_t line_len = newline - line;

      consumed = search = (newline - self->inbuf) + 1;

      if (self->discarding)
      {
        self->discarding = false;
        continue;
      }

      *newline = '\0';

      if (line_len > 0 && line[line_len - 1] == '\r')
        line[line_len - 1] = '\0';

      if (processLine(line, self, conns, curr_cap))
        return true;

      if (self->closing)
        return false;
    }

    self->inlen -= consumed;
    memmove(self->inbuf, self->inbuf + consumed, self->inlen);
    self->scanned = self->inlen;

    if (self->inlen == sizeof(self->inbuf))
    {
      if (!self->discarding)
      {
        if (replyError(self, "Message too long"))
          return true;

        self->discarding = true;
      }

      self->inlen = self->scanned = 0;
    }
  }
}

int main(int argc, char *argv[])
{
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    printf("Failed to ignore SIGPIPE\n");
    return 1;
  }

  // Line-buffer stdout so the log stays readable and ordered when it is piped
  // rather than attached to a terminal.
  setvbuf(stdout, NULL, _IOLBF, 0);

  // Usage: exchange_server [bind_address] [port]
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

    port = (int)value;
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

  // With no address given, listen on every interface.
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

  while (true)
  {
    printf("Waiting for events...\n");
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    printf("Number of events: %d\n", nfds);
    for (int i = 0; i < nfds; i++)
    {
      int curr_fd = events[i].data.fd;
      if (curr_fd == server_fd)
      {
        while (true)
        {
          struct sockaddr_in caddr;
          socklen_t caddr_len = sizeof(caddr);
          int client_fd = accept(server_fd, (struct sockaddr *)&caddr, &caddr_len);
          if (client_fd == -1)
          {
            // An interrupted or aborted attempt is worth retrying; EAGAIN just
            // means the backlog is drained, which is how this loop ends.
            if (errno == EINTR || errno == ECONNABORTED)
              continue;

            if (errno != EAGAIN && errno != EWOULDBLOCK)
              printf("Failed to accept client connection: %s\n", strerror(errno));

            break;
          }

          setNonBlocking(client_fd);

          if (client_fd >= curr_cap)
          {
            int new_cap = curr_cap;
            while (client_fd >= new_cap)
              new_cap *= 2;

            printf("Growing connection table from %d to %d\n", curr_cap, new_cap);

            struct ClientConnection **new_conns = realloc(conns, new_cap * sizeof(struct ClientConnection *));
            if (new_conns == NULL)
            {
              printf("Failed to grow connection table\n");
              close(client_fd);
              continue;
            }

            conns = new_conns;

            for (int j = curr_cap; j < new_cap; j++)
              conns[j] = NULL;
            curr_cap = new_cap;
          }

          conns[client_fd] = calloc(1, sizeof(struct ClientConnection));
          conns[client_fd]->client_fd = client_fd;
          conns[client_fd]->logged_in = false;
          conns[client_fd]->caddr = caddr;
          conns[client_fd]->caddr_len = caddr_len;

          kev.events = EPOLLIN | EPOLLOUT | EPOLLET;
          kev.data.fd = client_fd;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &kev) == -1)
          {
            printf("Failed to add client socket to epoll\n");
            close(client_fd);
            free(conns[client_fd]);
            conns[client_fd] = NULL;
          }
          else
          {
            printf("Client connected: %s:%d\n", inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
          }
        }
      }
      else
      {
        struct ClientConnection *conn = conns[curr_fd];
        bool done = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;

        // Drain pending output first: it frees buffer space and gets replies
        // moving again before this client is allowed to queue any more.
        if (!done && (events[i].events & EPOLLOUT))
          done = flushToClient(conn);

        if (!done && (events[i].events & EPOLLIN))
          done = communicate(curr_fd, conns, curr_cap);

        if (done)
        {
          printf("Removing client %d from epoll and closing connection\n", curr_fd);
          removeClient(curr_fd, epoll_fd, conns, curr_cap);
        }
      }
    }
  }

  close(server_fd);
  close(epoll_fd);

  printf("Exchange server closed successfully\n");

  return 0;
}
