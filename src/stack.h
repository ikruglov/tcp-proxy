#ifndef __STACK_H__
#define __STACK_H__

#include "common.h"

typedef struct {
    int top, size;
    int items[];
} int_stack_t;

inline static
int_stack_t* stack_init(size_t size)
{
    if (!size) return NULL;
    int new_size = sizeof(int_stack_t) + (sizeof(int) * size);
    int_stack_t* s = (int_stack_t*) malloc(new_size);
    if (!s) return NULL;

    s->top = -1;
    s->size = size;
    return s;
}

inline static
int_stack_t* stack_grow(int_stack_t* s, size_t size)
{
    if (!size) return s;
    int new_size = sizeof(int_stack_t) + (sizeof(int) * size);
    int_stack_t* stack = (int_stack_t*) realloc(s, new_size);
    if (!stack) return s;

    s->size = size;
    return stack;
}

inline static
void stack_free(int_stack_t* s)
{
    s->top = -1;
    s->size = 0;
    free(s);
}

inline static
int stack_empty(int_stack_t* s)
{
    return s->top == -1;
}

inline static
int stack_full(int_stack_t* s)
{
    return s->top + 1 == s->size;
}

inline static
void stack_push(int_stack_t* s, int v)
{
    s->items[++s->top] = v;
}

inline static
int stack_pop(int_stack_t* s)
{
    return s->items[s->top--];
}

inline static
int stack_peek(int_stack_t* s)
{
    return !stack_empty(s) ? s->items[s->top] : -1;
}

#endif
