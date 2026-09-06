#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 1024
#define RECVBUF_SIZE 128

char inbuf[BUFFER_SIZE];
size_t inlen = 0;
bool discarding = false;

char recvbuf[RECVBUF_SIZE];
size_t recvlen = 0;

bool writeAll(int fd, const char *data, size_t len)
{
  while (len > 0)
  {
    ssize_t written = write(fd, data, len);

    if (written == -1)
    {
      if (errno == EINTR)
        continue;

      return false;
    }

    data += written;
    len -= written;
  }

  return true;
}

// Prints every complete response currently buffered and keeps the partial tail.
void drainResponses(void)
{
  char *line = recvbuf, *newline;

  while ((newline = memchr(line, '\n', recvbuf + recvlen - line)) != NULL)
  {
    *newline = '\0';

    if (newline > line && newline[-1] == '\r')
      newline[-1] = '\0';

    printf("Received from server: %s\n", line);

    line = newline + 1;
  }

  recvlen -= line - recvbuf;
  memmove(recvbuf, line, recvlen);
}

// Sends every complete line typed so far, newline included, and keeps the rest.
bool sendPendingCommands(int client_fd)
{
  char *line = inbuf, *newline;

  while ((newline = memchr(line, '\n', inbuf + inlen - line)) != NULL)
  {
    size_t len = newline - line + 1;

    if (discarding)
      discarding = false;
    else if (len > 1 && !writeAll(client_fd, line, len))
      return false;

    line = newline + 1;
  }

  inlen -= line - inbuf;
  memmove(inbuf, line, inlen);

  if (inlen == sizeof(inbuf))
  {
    printf("Input line too long, discarded\n");
    discarding = true;
    inlen = 0;
  }

  return true;
}

void communicate(int client_fd)
{
  struct pollfd fds[2];

  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[1].fd = client_fd;
  fds[1].events = POLLIN;

  while (true)
  {
    if (poll(fds, 2, -1) == -1)
    {
      if (errno == EINTR)
        continue;

      printf("Failed to wait for input\n");
      return;
    }

    // The server is serviced first so that anything which arrived while the
    // user was typing is shown before the next command goes out.
    if (fds[1].revents)
    {
      ssize_t bytes_read = read(client_fd, recvbuf + recvlen, sizeof(recvbuf) - recvlen);

      if (bytes_read == -1)
      {
        printf("Failed to read from server\n");
        return;
      }
      else if (bytes_read == 0)
      {
        printf("Server closed the connection\n");
        return;
      }

      recvlen += bytes_read;
      drainResponses();

      if (recvlen == sizeof(recvbuf))
      {
        printf("Server sent an over-long response\n");
        return;
      }
    }

    if (fds[0].revents)
    {
      ssize_t bytes_read = read(STDIN_FILENO, inbuf + inlen, sizeof(inbuf) - inlen);

      if (bytes_read == -1)
      {
        printf("Failed to read input\n");
        return;
      }
      else if (bytes_read == 0)
      {
        // No more commands are coming, so half-close to send FIN and stop
        // polling stdin, but stay to collect whatever the server still sends.
        shutdown(client_fd, SHUT_WR);
        fds[0].fd = -1;
        continue;
      }

      inlen += bytes_read;

      if (!sendPendingCommands(client_fd))
      {
        printf("Failed to write to server\n");
        return;
      }
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

  setvbuf(stdout, NULL, _IOLBF, 0);

  // Usage: trader_client [host] [port] [username]
  const char *host = (argc > 1) ? argv[1] : SERVER_IP;
  const char *username = (argc > 3) ? argv[3] : NULL;
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

  int client_fd;
  struct sockaddr_in server_addr;
  socklen_t server_addr_len = sizeof(server_addr);

  client_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  // inet_pton returns 0 for a malformed address, not -1.
  if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1)
  {
    printf("Invalid server address: %s\n", host);
    close(client_fd);
    return 1;
  }

  if (connect(client_fd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
  {
    printf("Failed to connect to server\n");
    close(client_fd);
    return 1;
  }

  printf("Connected to server successfully\n");

  // A username on the command line means log in straight away, so the caller
  // does not have to feed a LOGIN line in on stdin.
  if (username != NULL)
  {
    char login[BUFFER_SIZE];
    int len = snprintf(login, sizeof(login), "LOGIN %s\n", username);

    if (len < 0 || (size_t)len >= sizeof(login) || !writeAll(client_fd, login, len))
    {
      printf("Failed to send LOGIN\n");
      close(client_fd);
      return 1;
    }
  }

  communicate(client_fd);

  close(client_fd);

  return 0;
}
