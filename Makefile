CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2

SERVER_BIN = exchange_server
TRADER_BIN = trader_client
MD_BIN     = market_data_client

SRC_DIR = src

.PHONY: all clean

all: $(SERVER_BIN) $(TRADER_BIN) $(MD_BIN)

$(SERVER_BIN): $(SRC_DIR)/exchange_server.c
	$(CC) $(CFLAGS) -o $@ $<

$(TRADER_BIN): $(SRC_DIR)/trader_client.c
	$(CC) $(CFLAGS) -o $@ $<

$(MD_BIN): $(SRC_DIR)/market_data_client.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(SERVER_BIN) $(TRADER_BIN) $(MD_BIN)