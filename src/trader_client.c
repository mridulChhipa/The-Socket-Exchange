#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 1024

char buffer[BUFFER_SIZE];

void communicate(int client_fd)
{
  while (true)
  {
    int i = 0;
    int c;
    while (i < BUFFER_SIZE - 1 && (c = getchar()) != EOF && c != '\n')
      buffer[i++] = (char)c;
    buffer[i] = '\0';

    if (c == EOF && i == 0)
    {
      printf("Input closed, exiting\n");
      close(client_fd);
      return;
    }

    bool quitting = (strcmp(buffer, "QUIT") == 0);

    if (write(client_fd, buffer, i) == -1)
    {
      printf("Failed to write to server\n");
      close(client_fd);
      return;
    }

    memset(buffer, 0, sizeof(buffer));

    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
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

    buffer[bytes_read] = '\0';
    printf("Received from server: %s\n", buffer);

    if (quitting)
    {
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
