#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>

#include "io.h"
#include "commands.h"

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

void removeClient(int client_fd, int kq, struct ClientConnection **conns, int curr_cap)
{
  /*
  Closing the fd would drop both filters on its own, but they are removed first
  so a failure to deregister is still visible. ENOENT is expected and ignored:
  registerClient may have failed partway, and either filter can already be gone.
  */
  struct kevent kev[2];
  EV_SET(&kev[0], client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&kev[1], client_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

  if (kevent(kq, kev, 2, NULL, 0, NULL) == -1 && errno != ENOENT)
  {
    printf("Failed to remove client socket from kqueue: %s\n", strerror(errno));
  }

  if (client_fd < curr_cap && conns[client_fd] != NULL)
  {
    struct ClientConnection *conn = conns[client_fd];

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

  close(client_fd);
}

bool processLine(char *line, struct ClientConnection *conn, struct ClientConnection **conns, int curr_cap, struct LimitOrderBook *orderbook)
{
  char cmd[INBUF_SIZE], arg[INBUF_SIZE];

  int matched = sscanf(line, "%159s %159s", cmd, arg);

  if (matched < 1)
    return false;

  printf("Received from client %d: %s\n", conn->client_fd, line);

  if (strlen(cmd) > MAX_CMD_LEN)
    return replyError(conn, "Unknown command");

  if (strcmp(cmd, "LOGIN") == 0 && matched == 2)
  {
    if (strlen(arg) > USERNAME_LENGTH - 1)
      return replyError(conn, "Username too long");

    return loginUser(arg, conn, conns, curr_cap);
  }

  if (strcmp(cmd, "SUBSCRIBE") == 0 && matched == 2)
    return subscribeClient(arg, conn);

  if (strcmp(cmd, "QUIT") == 0 && matched == 1)
    return quitUser(conn);

  if (strcmp(cmd, "UNSUBSCRIBE") == 0 && matched == 2)
    return unsubscribeClient(arg, conn);

  return replyError(conn, "Unknown command");
}

bool communicate(int client_fd, struct ClientConnection **conns, int curr_cap, struct LimitOrderBook *orderbook)
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

      if (processLine(line, self, conns, curr_cap, orderbook))
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
