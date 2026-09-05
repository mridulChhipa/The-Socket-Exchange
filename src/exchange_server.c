#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024
#define CMD_LENGTH 32
#define USERNAME_LENGTH 128
#define MIN_CAPACITY 10

struct ClientConnection
{
  socklen_t caddr_len;
  int client_fd;
  struct sockaddr_in caddr;
  bool logged_in;
  char username[USERNAME_LENGTH];
};

void setNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1)
  {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void removeClient(int client_fd, int epoll_fd, struct ClientConnection **conns, int curr_cap)
{
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL) == -1)
  {
    printf("Failed to remove client socket from epoll\n");
  }

  close(client_fd);

  if (client_fd < curr_cap && conns[client_fd] != NULL)
  {
    struct ClientConnection *conn = conns[client_fd];

    if (conn->logged_in)
      printf("Client %s:%d (user %s) disconnected\n", inet_ntoa(conn->caddr.sin_addr), ntohs(conn->caddr.sin_port), conn->username);
    else
      printf("Client %s:%d disconnected\n", inet_ntoa(conn->caddr.sin_addr), ntohs(conn->caddr.sin_port));

    free(conn);
    conns[client_fd] = NULL;
  }
}

void loginUser(const char *username, int client_fd, struct ClientConnection **conns, int curr_cap)
{
  struct ClientConnection *self = conns[client_fd];

  if (self->logged_in)
  {
    const char *response = "ERROR Already logged in\n";
    write(client_fd, response, strlen(response));
    return;
  }

  for (int i = 0; i < curr_cap; i++)
  {
    if (conns[i] != NULL && conns[i]->logged_in && strcmp(conns[i]->username, username) == 0)
    {
      const char *response = "ERROR Username already in use\n";
      write(client_fd, response, strlen(response));
      return;
    }
  }

  strncpy(self->username, username, USERNAME_LENGTH - 1);
  self->username[USERNAME_LENGTH - 1] = '\0';
  self->logged_in = true;

  printf("User %s logged in from %s:%d\n", self->username, inet_ntoa(self->caddr.sin_addr), ntohs(self->caddr.sin_port));

  const char *response = "OK\n";
  write(client_fd, response, strlen(response));
}

// QUIT is a graceful disconnect available to both client types and needs no
// login, so it always succeeds. We leave logged_in alone: removeClient still
// needs it to name the user in the disconnect log.
void quitUser(int client_fd, struct ClientConnection **conns)
{
  struct ClientConnection *self = conns[client_fd];

  if (self->logged_in)
    printf("User %s logged out\n", self->username);

  const char *response = "OK\n";
  write(client_fd, response, strlen(response));

  // Half-close: flush our side and send FIN, so the client sees an orderly
  // termination before we tear the socket down.
  if (shutdown(client_fd, SHUT_WR) == -1)
    printf("Failed to shut down write side of client socket\n");
}

bool communicate(int client_fd, int epoll_fd, struct ClientConnection **conns, int curr_cap)
{
  char buffer[BUFFER_SIZE];
  char cmd[CMD_LENGTH];
  char username[USERNAME_LENGTH];

  while (true)
  {
    memset(buffer, 0, sizeof(buffer));

    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
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

    buffer[bytes_read] = '\0';
    printf("Received from client: %s\n", buffer);

    int matched = sscanf(buffer, "%31s %127s", cmd, username);

    if (strcmp(cmd, "LOGIN") == 0 && matched == 2)
    {
      loginUser(username, client_fd, conns, curr_cap);
    }
    else if (strcmp(cmd, "QUIT") == 0 && matched == 1)
    {
      quitUser(client_fd, conns);
      return true;
    }
    else
    {
      write(client_fd, "Invalid command\n", 16);
    }
  }
}

int main(int argc, char *argv[])
{
  int server_fd;
  struct sockaddr_in server_addr;
  socklen_t server_addr_len = sizeof(server_addr);

  // Usage: exchange_server [bind-address] [port]
  // The experiment script picks the port, so both must come from the command
  // line; the previous hardcoded values remain the defaults.
  in_addr_t bind_addr = INADDR_ANY;
  int port = DEFAULT_PORT;

  if (argc > 3)
  {
    printf("Usage: %s [bind-address] [port]\n", argv[0]);
    return 1;
  }

  if (argc >= 2)
  {
    struct in_addr parsed;
    if (inet_pton(AF_INET, argv[1], &parsed) != 1)
    {
      printf("Invalid bind address: %s\n", argv[1]);
      return 1;
    }
    bind_addr = parsed.s_addr;
  }

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

  server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = bind_addr;
  server_addr.sin_port = htons((uint16_t)port);

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

  printf("Listening on port %d\n", port);

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

  struct epoll_event events[MIN_CAPACITY];
  int curr_cap = MIN_CAPACITY;

  struct ClientConnection **conns;
  conns = calloc(curr_cap, sizeof(struct ClientConnection *));
  if (conns == NULL)
  {
    printf("Failed to allocate connection table\n");
    return 1;
  }

  while (true)
  {
    printf("Waiting for events...\n");
    int nfds = epoll_wait(epoll_fd, events, MIN_CAPACITY, -1);
    printf("Number of events: %d\n", nfds);
    for (int i = 0; i < nfds; i++)
    {
      int curr_fd = events[i].data.fd;
      if (curr_fd == server_fd)
      {
        while (true)
        {
          printf("Accepting new client connection...\n");
          struct sockaddr_in caddr;
          socklen_t caddr_len = sizeof(caddr);
          int client_fd = accept(server_fd, (struct sockaddr *)&caddr, &caddr_len);
          if (client_fd == -1)
          {
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

          kev.events = EPOLLIN | EPOLLET;
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
        printf("Handling communication for client %d\n", curr_fd);
        if (communicate(curr_fd, epoll_fd, conns, curr_cap))
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
