#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 8080
#define DEFAULT_SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 1024

char buffer[BUFFER_SIZE];

// write() may accept fewer bytes than asked for, so loop until the whole
// message (including its newline terminator) has gone out.
bool writeAll(int fd, const char *data, size_t len)
{
  size_t sent = 0;
  while (sent < len)
  {
    ssize_t n = write(fd, data + sent, len - sent);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    sent += (size_t)n;
  }
  return true;
}

// Reads one server reply and prints it. Returns false if the connection is
// gone or unreadable.
bool readReply(int client_fd)
{
  memset(buffer, 0, sizeof(buffer));

  ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
  if (bytes_read == -1)
  {
    printf("Failed to read from server\n");
    return false;
  }
  else if (bytes_read == 0)
  {
    printf("Server disconnected\n");
    return false;
  }

  buffer[bytes_read] = '\0';
  printf("Received from server: %s\n", buffer);
  return true;
}

void communicate(int client_fd)
{
  while (true)
  {
    // Leave room for the newline terminator and the NUL.
    int i = 0;
    int c = 0;
    while (i < BUFFER_SIZE - 2 && (c = getchar()) != EOF && c != '\n')
      buffer[i++] = (char)c;
    buffer[i] = '\0';

    if (c == EOF && i == 0)
    {
      printf("Input closed, exiting\n");
      shutdown(client_fd, SHUT_WR);
      close(client_fd);
      return;
    }

    bool quitting = (strcmp(buffer, "QUIT") == 0);

    // Every protocol message is a single line terminated by '\n' (2.9), which
    // getchar() stripped, so put it back before sending.
    buffer[i] = '\n';
    buffer[i + 1] = '\0';

    if (!writeAll(client_fd, buffer, (size_t)i + 1))
    {
      printf("Failed to write to server\n");
      close(client_fd);
      return;
    }

    if (!readReply(client_fd))
    {
      close(client_fd);
      return;
    }

    if (quitting)
    {
      shutdown(client_fd, SHUT_WR);
      close(client_fd);
      return;
    }
  }
}

int main(int argc, char *argv[])
{
  int client_fd;
  struct sockaddr_in server_addr;
  socklen_t server_addr_len = sizeof(server_addr);

  // Usage: trader_client [host] [port] [username]
  // The experiment script supplies all three; the old hardcoded values are the
  // defaults. When a username is given we log in with it straight away.
  const char *server_ip = DEFAULT_SERVER_IP;
  int port = DEFAULT_PORT;
  const char *username = NULL;

  if (argc > 4)
  {
    printf("Usage: %s [host] [port] [username]\n", argv[0]);
    return 1;
  }

  if (argc >= 2)
    server_ip = argv[1];

  if (argc >= 3)
  {
    char *end;
    long parsed = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || parsed < 1 || parsed > 65535)
    {
      printf("Invalid port: %s\n", argv[2]);
      return 1;
    }
    port = (int)parsed;
  }

  if (argc >= 4)
    username = argv[3];

  client_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons((uint16_t)port);

  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1)
  {
    printf("Invalid server address: %s\n", server_ip);
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

  if (username != NULL)
  {
    char login[BUFFER_SIZE];
    int len = snprintf(login, sizeof(login), "LOGIN %s\n", username);
    if (len < 0 || (size_t)len >= sizeof(login))
    {
      printf("Username too long: %s\n", username);
      close(client_fd);
      return 1;
    }

    if (!writeAll(client_fd, login, (size_t)len) || !readReply(client_fd))
    {
      printf("Failed to log in as %s\n", username);
      close(client_fd);
      return 1;
    }
  }

  communicate(client_fd);

  return 0;
}
