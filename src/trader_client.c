#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h> // Didn't used <netinet/in.h> because we also need inet_<name> functions

#define PORT 8080
#define SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 1024

char buffer[BUFFER_SIZE];

void communicate(int client_fd)
{
  while (true)
  {
    memset(buffer, 0, sizeof(buffer));
    // write(client_fd, buffer, sizeof(buffer)); // Send an empty buffer to the server to initiate communication
    memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    if (bytes_read == -1)
    {
      printf("Failed to read from server\n");
      close(client_fd);
      return;
    }
    else if (bytes_read == 0)
    {
      printf("Client disconnected\n");
      close(client_fd);
      return;
    }

    buffer[bytes_read] = '\0';
    printf("Received from server: %s\n", buffer);
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

  /*
  The thrird argument of inet_pton() is a void pointer to the destination address structure.
  All non-function pointer type arguments are implicitly converted to void pointers in C, so we can pass the address of the sin_addr field directly without casting it to a void pointer.

  Obsolete: server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  */
  inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

  if (connect(client_fd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
  {
    printf("Failed to connect to server\n");
    close(client_fd);
    return 1;
  }

  printf("Connected to server successfully\n");

  communicate(client_fd);
  close(client_fd);
}
