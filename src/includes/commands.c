#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include <arpa/inet.h>

#include "commands.h"
#include "server.h"
#include "config.h"
#include "io.h"

bool loginUser(const char *username, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap)
{
  if (self->type == market)
    return replyError(self, "Market data clients cannot log in");

  if (self->logged_in)
    return replyError(self, "Already logged in");

  for (int i = 0; i < curr_cap; i++)
  {
    if (conns[i] != NULL && conns[i]->logged_in && strcmp(conns[i]->username, username) == 0)
      return replyError(self, "Username already in use");
  }

  self->type = trader;

  strncpy(self->username, username, USERNAME_LENGTH - 1);
  self->username[USERNAME_LENGTH - 1] = '\0';
  self->logged_in = true;

  printf("User %s logged in from %s:%d\n", self->username, inet_ntoa(self->caddr.sin_addr), ntohs(self->caddr.sin_port));

  return sendLine(self, "OK\n");
}

bool subscribeClient(const char *instrument, struct ClientConnection *self)
{
  if (self->type == trader)
    return replyError(self, "Traders cannot subscribe to market data");

  if (strcmp(instrument, "JNST") == 0)
  {
    if (self->subscribedJNST)
      return replyError(self, "Already subscribed to JNST");
    self->subscribedJNST = true;
  }
  else if (strcmp(instrument, "IMCT") == 0)
  {
    if (self->subscribedIMCT)
      return replyError(self, "Already subscribed to IMCT");
    self->subscribedIMCT = true;
  }
  else
  {
    return replyError(self, "Invalid instrument");
  }

  self->type = market;

  return sendLine(self, "OK\n");
}

bool quitUser(struct ClientConnection *self)
{
  self->closing = true;

  return sendLine(self, "OK\n") || self->outlen == 0;
}

bool unsubscribeClient(const char *instrument, struct ClientConnection *self)
{
  if (self->type == trader)
    return replyError(self, "Traders cannot unsubscribe from market data");

  if (strcmp(instrument, "JNST") == 0)
  {
    if (!self->subscribedJNST)
      return replyError(self, "Not subscribed to JNST");
    self->subscribedJNST = false;
  }
  else if (strcmp(instrument, "IMCT") == 0)
  {
    if (!self->subscribedIMCT)
      return replyError(self, "Not subscribed to IMCT");
    self->subscribedIMCT = false;
  }
  else
  {
    return replyError(self, "Invalid instrument");
  }

  return sendLine(self, "OK\n");
}

static int next_order_id = 0;
static void notifyClient(int client_fd, const char *msg, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap, bool *done)
{
  if (client_fd < 0 || client_fd >= curr_cap || conns[client_fd] == NULL)
    return;

  bool failed = sendLine(conns[client_fd], msg);

  if (client_fd == self->client_fd && failed)
    *done = true;
}

bool placeOrder(enum OrderSide side, const char *instrument, int quantity, int price, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap, struct LimitOrderBook *orderbook)
{
  enum Instrument inst;

  if (strcmp(instrument, "JNST") == 0)
    inst = JNST;
  else if (strcmp(instrument, "IMCT") == 0)
    inst = IMCT;
  else
    return replyError(self, "Invalid instrument");

  if (quantity <= 0 || price <= 0)
    return replyError(self, "Quantity and price must be positive");

  struct Order *order = calloc(1, sizeof(struct Order));
  if (order == NULL)
    return replyError(self, "Internal server error");

  order->id = next_order_id++;
  order->price = price;
  order->quantity = quantity;
  order->client_fd = self->client_fd;
  order->type = side;
  order->instrument = inst;

  char msg[128];
  snprintf(msg, sizeof(msg), "ORDER_ACCEPTED %d\n", order->id);

  bool done = sendLine(self, msg);

  struct Fill *fills;
  int num_fills = addOrder(orderbook, order, &fills);

  for (int i = 0; i < num_fills; i++)
  {
    struct Fill *fill = &fills[i];
    const char *name = (fill->instrument == JNST) ? "JNST" : "IMCT";

    snprintf(msg, sizeof(msg), "BOUGHT %s %d %d\n", name, fill->quantity, fill->price);
    notifyClient(fill->buy_client_fd, msg, self, conns, curr_cap, &done);

    snprintf(msg, sizeof(msg), "SOLD %s %d %d\n", name, fill->quantity, fill->price);
    notifyClient(fill->sell_client_fd, msg, self, conns, curr_cap, &done);

    snprintf(msg, sizeof(msg), "TRADE %s %d %d\n", name, fill->quantity, fill->price);

    for (int j = 0; j < curr_cap; j++)
    {
      struct ClientConnection *sub = conns[j];

      if (sub == NULL || sub->type != market)
        continue;

      if (fill->instrument == JNST ? sub->subscribedJNST : sub->subscribedIMCT)
        sendLine(sub, msg);
    }
  }

  free(fills);

  return done;
}

bool cancelUserOrder(const char *order_id_str, struct ClientConnection *self, struct LimitOrderBook *orderbook)
{
  char *end;
  long id = strtol(order_id_str, &end, 10);

  if (*end != '\0' || end == order_id_str || id < 0 || id > INT_MAX)
    return replyError(self, "Invalid order id");

  if (!cancelOrder(orderbook, (int)id, self->client_fd))
    return replyError(self, "No such order");

  char msg[128];
  snprintf(msg, sizeof(msg), "ORDER_CANCELLED %d\n", (int)id);

  return sendLine(self, msg);
}
