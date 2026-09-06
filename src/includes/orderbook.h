#ifndef ORDERBOOK_H
#define ORDERBOOK_H

struct Order
{
    struct Order *next;
    struct Order *prev;
    int id;
    int quantity;
    int price;
    int client_fd;

    enum OrderSide
    {
        BUY,
        SELL
    } type; // buy (0) or sell (1)

    enum Instrument
    {
        JNST,
        IMCT
    } instrument; // 0 for jnst and 1 for imct
};

struct Fill
{
    int price;
    int quantity;
    int buy_client_fd;
    int sell_client_fd;
    enum Instrument instrument;
};

#define MAX_FILLS 64

struct LimitOrderBook
{
    struct Order *jnst_buy_orders;
    struct Order *jnst_sell_orders;
    struct Order *imct_buy_orders;
    struct Order *imct_sell_orders;
};

void initOrderbook(struct LimitOrderBook *book);

int addOrder(struct LimitOrderBook *book, struct Order *new_order, struct Fill *fills, int max_fills);

int cancelOrder(struct LimitOrderBook *book, int order_id, int client_fd);

#endif
