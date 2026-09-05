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

struct LimitOrderBook
{
    struct Order *jnst_buy_orders;
    struct Order *jnst_sell_orders;
    struct Order *imct_buy_orders;
    struct Order *imct_sell_orders;
};

void initOrderbook(struct LimitOrderBook *book);
void addOrder(struct LimitOrderBook *book, struct Order *new_order);
int cancelOrder(struct LimitOrderBook *book, int order_id);

#endif
