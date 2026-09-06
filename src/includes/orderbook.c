#include <stdlib.h>

#include "orderbook.h"

#define FILLS_INITIAL_CAPACITY 16

void initOrderbook(struct LimitOrderBook *book)
{
    book->jnst_buy_orders = NULL;
    book->jnst_sell_orders = NULL;
    book->imct_buy_orders = NULL;
    book->imct_sell_orders = NULL;
}

static void append(struct Order **head, struct Order *ord)
{
    ord->next = NULL;
    ord->prev = NULL;

    if (*head == NULL)
    {
        *head = ord;
        return;
    }
    struct Order *cur = *head;
    while (cur->next != NULL)
    {
        cur = cur->next;
    }

    cur->next = ord;
    ord->prev = cur;
}

static void removeNode(struct Order **head, struct Order *node)
{
    if (node->prev != NULL)
    {
        node->prev->next = node->next;
    }
    else
    {
        *head = node->next;
    }

    if (node->next != NULL)
    {
        node->next->prev = node->prev;
    }

    node->next = NULL;
    node->prev = NULL;
}

static struct Order **ownList(struct LimitOrderBook *book, struct Order *ord)
{
    if (ord->instrument == JNST)
    {
        if (ord->type == BUY)
        {
            return &book->jnst_buy_orders;
        }
        else
        {
            return &book->jnst_sell_orders;
        }
    }
    else
    {
        if (ord->type == BUY)
        {
            return &book->imct_buy_orders;
        }
        else
        {
            return &book->imct_sell_orders;
        }
    }
}

static struct Order **oppList(struct LimitOrderBook *book, struct Order *ord)
{
    if (ord->instrument == JNST)
    {
        if (ord->type == BUY)
        {
            return &book->jnst_sell_orders;
        }
        else
        {
            return &book->jnst_buy_orders;
        }
    }
    else
    {
        if (ord->type == BUY)
        {
            return &book->imct_sell_orders;
        }
        else
        {
            return &book->imct_buy_orders;
        }
    }
}

int addOrder(struct LimitOrderBook *book, struct Order *ord, struct Fill **fills)
{
    struct Order **own = ownList(book, ord);
    struct Order **opp = oppList(book, ord);

    int count = 0;
    int cap = 0;

    *fills = NULL;

    struct Order *cur = *opp;
    while (cur != NULL && ord->quantity > 0)
    {
        struct Order *nxt = cur->next;

        if (cur->price == ord->price)
        {
            int qty = cur->quantity;
            if (ord->quantity < qty)
            {
                qty = ord->quantity;
            }

            cur->quantity -= qty;
            ord->quantity -= qty;

            if (count == cap)
            {
                int new_cap = (cap == 0) ? FILLS_INITIAL_CAPACITY : cap * 2;
                struct Fill *grown = realloc(*fills, new_cap * sizeof(struct Fill));

                if (grown != NULL)
                {
                    *fills = grown;
                    cap = new_cap;
                }
            }

            if (count < cap)
            {
                (*fills)[count].price = cur->price;
                (*fills)[count].quantity = qty;
                (*fills)[count].instrument = ord->instrument;

                if (ord->type == BUY)
                {
                    (*fills)[count].buy_client_fd = ord->client_fd;
                    (*fills)[count].sell_client_fd = cur->client_fd;
                }
                else
                {
                    (*fills)[count].buy_client_fd = cur->client_fd;
                    (*fills)[count].sell_client_fd = ord->client_fd;
                }

                count++;
            }

            if (cur->quantity == 0)
            {
                removeNode(opp, cur);
                free(cur);
            }
        }

        cur = nxt;
    }

    if (ord->quantity > 0)
        append(own, ord);
    else
        free(ord);

    return count;
}

static void detachIn(struct Order *head, int client_fd)
{
    struct Order *cur = head;

    while (cur != NULL)
    {
        if (cur->client_fd == client_fd)
            cur->client_fd = -1;

        cur = cur->next;
    }
}

void detachClientOrders(struct LimitOrderBook *book, int client_fd)
{
    detachIn(book->jnst_buy_orders, client_fd);
    detachIn(book->jnst_sell_orders, client_fd);
    detachIn(book->imct_buy_orders, client_fd);
    detachIn(book->imct_sell_orders, client_fd);
}

static int cancelIn(struct Order **head, int id, int client_fd)
{
    struct Order *cur = *head;

    while (cur != NULL)
    {
        if (cur->id == id && cur->client_fd == client_fd)
        {
            removeNode(head, cur);
            free(cur);
            return 1;
        }

        cur = cur->next;
    }

    return 0;
}

int cancelOrder(struct LimitOrderBook *book, int id, int client_fd)
{
    if (cancelIn(&book->jnst_buy_orders, id, client_fd))
        return 1;
    if (cancelIn(&book->jnst_sell_orders, id, client_fd))
        return 1;
    if (cancelIn(&book->imct_buy_orders, id, client_fd))
        return 1;
    if (cancelIn(&book->imct_sell_orders, id, client_fd))
        return 1;

    return 0;
}
