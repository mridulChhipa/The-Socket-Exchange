#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 1024

#define RECVBUF_SIZE 128

char buffer[BUFFER_SIZE];

char recvbuf[RECVBUF_SIZE];
size_t recvlen = 0;

int drainLines()
{
  int lines = 0;
  char *line = recvbuf, *newline;

  while ((newline = memchr(line, '\n', recvbuf + recvlen - line)) != NULL)
  {
    *newline = '\0';

    if (newline > line && newline[-1] == '\r')
      newline[-1] = '\0';

    printf("Received from server: %s\n", line);

    line = newline + 1;
    lines++;
  }

  recvlen -= line - recvbuf;
  memmove(recvbuf, line, recvlen);

  return lines;
}

void communicate(int client_fd)
{
  while (true)
  {
    int i = 0;
    int c;
    while (i < BUFFER_SIZE - 1 && (c = getchar()) != EOF && c != '\n')
      buffer[i++] = c;
    buffer[i] = '\0';

    if (c == EOF && i == 0)
    {
      printf("Input closed, exiting\n");
      close(client_fd);
      return;
    }

    // A blank line is not a protocol message; sending it would leave us waiting
    // for a response the server has no reason to send.
    if (i == 0)
      continue;

    bool quitting = (strcmp(buffer, "QUIT") == 0);

    buffer[i] = '\n';

    if (write(client_fd, buffer, i + 1) == -1)
    {
      printf("Failed to write to server\n");
      close(client_fd);
      return;
    }

    // A response may be split across several reads, or share one segment with a
    // response still buffered from last time, so keep reading until at least one
    // complete line is available.
    while (drainLines() == 0)
    {
      if (recvlen == sizeof(recvbuf))
      {
        printf("Server sent an over-long response\n");
        close(client_fd);
        return;
      }

      ssize_t bytes_read = read(client_fd, recvbuf + recvlen, sizeof(recvbuf) - recvlen);
      if (bytes_read == -1)
      {
        printf("Failed to read from server\n");
        close(client_fd);
        return;
      }
      else if (bytes_read == 0)
      {
        printf("Server disconnected\n");
        close(client_fd);
        return;
      }

      recvlen += bytes_read;
    }

    if (quitting)
    {
      close(client_fd);
      return;
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
  server_addr.sin_port = htons(PORT);

  inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

  if (connect(client_fd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
  {
    printf("Failed to connect to server\n");
    close(client_fd);
    return 1;
  }

  printf("Connected to server successfully\n");

  communicate(client_fd);

  return 0;
}
