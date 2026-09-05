#include <stdlib.h>

#include "orderbook.h"

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

void addOrder(struct LimitOrderBook *book, struct Order *ord)
{
    struct Order **own = ownList(book, ord);
    struct Order **opp = oppList(book, ord);

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
}

static int cancelIn(struct Order **head, int id)
{
    struct Order *cur = *head;

    while (cur != NULL)
    {
        if (cur->id == id)
        {
            removeNode(head, cur);
            free(cur);
            return 1;
        }

        cur = cur->next;
    }

    return 0;
}

int cancelOrder(struct LimitOrderBook *book, int id)
{
    if (cancelIn(&book->jnst_buy_orders, id))
        return 1;
    if (cancelIn(&book->jnst_sell_orders, id))
        return 1;
    if (cancelIn(&book->imct_buy_orders, id))
        return 1;
    if (cancelIn(&book->imct_sell_orders, id))
        return 1;

    return 0;
}
