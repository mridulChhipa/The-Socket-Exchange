#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

char buffer[BUFFER_SIZE];

void communicate(int client_fd)
{
  while (true) // Either use 1 or use true from <stdbool.h> for better readability
  {
    memset(buffer, 0, sizeof(buffer)); // Clear the buffer before receiving data

    /*
    read() is a system call that reads data from a file descriptor into a buffer.
    It is defined in the <unistd.h> header
    read() does not wait to receive all count bytes before returning. It copies whatever bytes are currently waiting in the kernel's receive buffer and returns immediately. If you request X bytes, but only Y < X have arrived over the network, read() returns Y. You must loop calls to read() if your protocol expects a fixed-size message or header.
    */
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    if (bytes_read == -1)
    {
      printf("Failed to read from client\n");
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
    printf("Received from client: %s\n", buffer);

    /*
    write() is a system call that writes data from a buffer to a file descriptor. The send() system call is similar to write() but provides additional options for sending data over sockets.
    The write() system call also does not guarantee that all bytes will be written in one call. It may write fewer bytes than requested if kernel's socket buffer has room for fewer bytes.
    You must loop calls to write() if your protocol expects a fixed-size message or header.
    */
    write(client_fd, buffer, bytes_read); // Echo the received data back to the client
  }
}

int main(int argc, char *argv[])
{
  int server_fd; // File descriptor for the server socket
  struct sockaddr_in server_addr;
  socklen_t server_addr_len = sizeof(server_addr);

  /*
  AF_INET is the address family for IPv4(32-bit), AF_INET6(128-bit) is for IPv6 and AF_UNIX is for Unix domain sockets.
  SOCK_STREAM is the socket type for TCP, SOCK_DGRAM is for UDP, and SOCK_RAW is for raw sockets.
  */
  server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  /*
  Initialize the server address structure to zero.
  This is necessary to ensure that all fields are properly initialized before use.
  Unlike C++, C structs do not have constructors, so we need to manually set all fields to zero.
  */
  memset(&server_addr, 0, sizeof(server_addr));

  /*
  The first two bytes of every socket address structure (whether IPv4, IPv6, or Unix domain) always hold the family field.
  */
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any available interfaces
  server_addr.sin_port = htons(PORT);

  /*
  The bind() system call associates a socket with a specific address and port.
  It is used to specify the local address and port that the socket will use to receive data.

  bind(server_fd, &server_addr, sizeof(server_addr))
  */
  if (bind(server_fd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
  {
    printf("Failed to bind socket\n");
    return 1;
  }

  printf("Socket bound successfully\n");

  /*
  The listen() system call that marks a bound stream socket as a passive socket i.e it will be used to accept incoming connection requests using accept(), rather than initiate a connection using connect().
  listen() transitions the socket's internal TCP state to LISTEN, instructing the kernel to automatically handle the TCP three-way handshake for incoming clients in the background.
  The second argument specifies the maximum number of pending connections that can be queued up before the kernel starts rejecting new connection requests.
  SOMAXCONN is a constant defined in <sys/socket.h> that represents the maximum number of pending connections allowed by the system.
  */
  if (listen(server_fd, SOMAXCONN) == -1)
  {
    printf("Failed to listen on socket\n");
    return 1;
  }

  printf("Listening on port %d\n", PORT);

  int client_fd; // File descriptor for the client socket
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
  if (client_fd == -1)
  {
    printf("Failed to accept client connection\n");
    return 1;
  }

  printf("Client connected successfully\n");

  communicate(client_fd);

  close(client_fd);
  close(server_fd);

  printf("Exchange server closed successfully\n");

  return 0;
}
