#ifndef SBUF_H
#define SBUF_H

#include "csapp.h"

/* 여러 스레드가 함께 쓰는 유한 FIFO 버퍼 (생산자-소비자, CS:APP 12.5.4)
 * main 스레드가 연결 기술자를 넣고, worker 스레드들이 꺼내 간다. */
typedef struct {
    int *items;        /* 원형 배열 */
    int capacity;      /* 최대 칸 수 */
    int head;          /* 다음에 꺼낼 위치 */
    int tail;          /* 다음에 넣을 위치 */
    sem_t lock;        /* 배열과 head/tail 접근 보호 (초기 1) */
    sem_t empty_slots; /* 비어 있는 칸 수 (초기 capacity) */
    sem_t filled;      /* 채워진 칸 수 (초기 0) */
} sbuf_t;

void sbuf_init(sbuf_t *sp, int capacity);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);   /* 빈 칸이 생길 때까지 대기 */
int sbuf_remove(sbuf_t *sp);              /* 항목이 들어올 때까지 대기 */

#endif
