#ifndef LLIST_H
#define LLIST_H

#include "lib/core/types.h"

typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct {
    list_node_t head;
    uint32_t    count;
} list_t;

#define LIST_ENTRY(ptr, type, member) \
    ((type *)((uint8_t *)(ptr) - (uint32_t)(&((type *)0)->member)))

#define LIST_FOR_EACH(list, node) \
    for ((node) = (list)->head.next; \
         (node) != &(list)->head; \
         (node) = (node)->next)

#define LIST_FOR_EACH_SAFE(list, node, tmp) \
    for ((node) = (list)->head.next, (tmp) = (node)->next; \
         (node) != &(list)->head; \
         (node) = (tmp), (tmp) = (node)->next)

static inline void list_init(list_t *l) {
    l->head.prev = &l->head;
    l->head.next = &l->head;
    l->count     = 0;
}

static inline bool list_empty(const list_t *l) {
    return l->head.next == &l->head;
}

static inline uint32_t list_count(const list_t *l) {
    return l->count;
}

static inline list_node_t *list_front(const list_t *l) {
    return list_empty(l) ? NULL : l->head.next;
}

static inline list_node_t *list_back(const list_t *l) {
    return list_empty(l) ? NULL : l->head.prev;
}

static inline void list_insert_after(list_t *l, list_node_t *prev, list_node_t *node) {
    node->prev = prev;
    node->next = prev->next;
    prev->next->prev = node;
    prev->next = node;
    l->count++;
}

static inline void list_insert_before(list_t *l, list_node_t *next, list_node_t *node) {
    list_insert_after(l, next->prev, node);
}

static inline void list_push_front(list_t *l, list_node_t *node) {
    list_insert_after(l, &l->head, node);
}

static inline void list_push_back(list_t *l, list_node_t *node) {
    list_insert_after(l, l->head.prev, node);
}

static inline void list_remove(list_t *l, list_node_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = NULL;
    l->count--;
}

static inline list_node_t *list_pop_front(list_t *l) {
    if (list_empty(l)) return NULL;
    list_node_t *n = l->head.next;
    list_remove(l, n);
    return n;
}

static inline list_node_t *list_pop_back(list_t *l) {
    if (list_empty(l)) return NULL;
    list_node_t *n = l->head.prev;
    list_remove(l, n);
    return n;
}

static inline void list_splice_back(list_t *dst, list_t *src) {
    if (list_empty(src)) return;
    src->head.prev->next = &dst->head;
    src->head.next->prev =  dst->head.prev;
    dst->head.prev->next =  src->head.next;
    dst->head.prev       =  src->head.prev;
    dst->count          +=  src->count;
    list_init(src);
}

#endif
