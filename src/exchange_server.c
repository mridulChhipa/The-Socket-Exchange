#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 8080
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

void quitUser(int client_fd, struct ClientConnection **conns)
{
  struct ClientConnection *self = conns[client_fd];

  if (!self->logged_in)
  {
    const char *response = "ERROR Not logged in\n";
    write(client_fd, response, strlen(response));
    return;
  }

  printf("User %s logged out\n", self->username);
  self->logged_in = false;

  const char *response = "OK\n";
  write(client_fd, response, strlen(response));
}

void communicate(int client_fd, int epoll_fd, struct ClientConnection **conns, int curr_cap)
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
      printf("Failed to read from client\n");
      return;
    }
    else if (bytes_read == 0)
    {
      printf("Client disconnected\n");
      return;
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
      return;
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

  server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd == -1)
  {
    printf("Failed to create socket\n");
    return 1;
  }

  printf("Socket created successfully\n");

  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

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

  printf("Listening on port %d\n", PORT);

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
  conns = malloc(curr_cap * sizeof(struct ClientConnection *));

  while (true)
  {
    int nfds = epoll_wait(epoll_fd, events, MIN_CAPACITY, -1);
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
            break;

          setNonBlocking(client_fd);

          if (client_fd >= curr_cap)
          {
            int new_cap = curr_cap;
            while (client_fd >= new_cap)
              new_cap *= 2;

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
        communicate(curr_fd, epoll_fd, conns, curr_cap);
        removeClient(curr_fd, epoll_fd, conns, curr_cap);
      }
    }
  }

  close(server_fd);
  close(epoll_fd);

  printf("Exchange server closed successfully\n");

  return 0;
}
