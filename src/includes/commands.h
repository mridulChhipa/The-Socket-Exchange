#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>

#include "server.h"

bool loginUser(const char *username, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap);
bool subscribeClient(const char *instrument, struct ClientConnection *self);
bool quitUser(struct ClientConnection *self);
bool unsubscribeClient(const char *instrument, struct ClientConnection *self);

// Orderbook commands
bool placeOrder(enum OrderSide side, const char *instrument, int quantity, int price, struct ClientConnection *self, struct ClientConnection **conns, int curr_cap, struct LimitOrderBook *orderbook);

bool cancelUserOrder(const char *order_id_str, struct ClientConnection *self, struct LimitOrderBook *orderbook);

#endif
