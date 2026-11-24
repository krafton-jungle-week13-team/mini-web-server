#include "sbuf.h"

void sbuf_init(sbuf_t *sp, int capacity)
{
    sp->items = Calloc(capacity, sizeof(int));
    sp->capacity = capacity;
    sp->head = 0;
    sp->tail = 0;
    Sem_init(&sp->lock, 0, 1);
    Sem_init(&sp->empty_slots, 0, capacity);
    Sem_init(&sp->filled, 0, 0);
}

void sbuf_deinit(sbuf_t *sp)
{
    Free(sp->items);
}

void sbuf_insert(sbuf_t *sp, int item)
{
    /* 빈 칸을 먼저 확보한 뒤 잠근다.
     * 잠근 채로 빈 칸을 기다리면 소비자가 lock을 못 얻어 칸이 영원히 안 생긴다. */
    P(&sp->empty_slots);
    P(&sp->lock);
    sp->items[sp->tail] = item;
    sp->tail = (sp->tail + 1) % sp->capacity;
    V(&sp->lock);
    V(&sp->filled);
}

int sbuf_remove(sbuf_t *sp)
{
    int item;

    P(&sp->filled);
    P(&sp->lock);
    item = sp->items[sp->head];
    sp->head = (sp->head + 1) % sp->capacity;
    V(&sp->lock);
    V(&sp->empty_slots);
    return item;
}
