#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

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

  printf("Data client subscribed to %s from %s:%d\n", instrument, inet_ntoa(self->caddr.sin_addr), ntohs(self->caddr.sin_port));

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

  printf("Data client unsubscribed from %s at %s:%d\n", instrument, inet_ntoa(self->caddr.sin_addr), ntohs(self->caddr.sin_port));

  return sendLine(self, "OK\n");
}
