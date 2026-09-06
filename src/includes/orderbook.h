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

struct LimitOrderBook
{
    struct Order *jnst_buy_orders;
    struct Order *jnst_sell_orders;
    struct Order *imct_buy_orders;
    struct Order *imct_sell_orders;
};

void initOrderbook(struct LimitOrderBook *book);

/*
Returns how many executions occurred and points *fills at them. The caller owns
that array and must free it. Takes ownership of new_order either way: it is
linked into the book or freed.
*/
int addOrder(struct LimitOrderBook *book, struct Order *new_order, struct Fill **fills);

/*
Leaves a departed client's orders resting but marks them ownerless, so fills
against them notify nobody and nobody can cancel them. Descriptors are reused,
so an order still naming a closed fd would otherwise be inherited by whichever
client the OS hands that number to next.
*/
void detachClientOrders(struct LimitOrderBook *book, int client_fd);

int cancelOrder(struct LimitOrderBook *book, int order_id, int client_fd);

#endif
