/* mini-os/v2-c-kernel/sched_policy.c
 * 调度策略实现：纯逻辑、无内核依赖（只依赖 stdint 与自有头），
 * 可在宿主环境编译运行单元测试（见 tests/test_sched.c）。
 * 就绪队列用环形数组实现：head 指向队头，tail 指向队尾，
 * remove 通过前移后续元素维持连续布局，避免空洞。 */
#include "sched_policy.h"

void policy_readyq_init(policy_readyq_t *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

int policy_readyq_push(policy_readyq_t *q, uint32_t pid) {
    if (q->count >= POLICY_MAX_READY) return -1;
    q->ready[q->tail] = pid;
    q->tail = (q->tail + 1) % POLICY_MAX_READY;
    q->count++;
    return 0;
}

uint32_t policy_readyq_pop(policy_readyq_t *q) {
    if (q->count == 0) return POLICY_NO_PID;
    uint32_t pid = q->ready[q->head];
    q->head = (q->head + 1) % POLICY_MAX_READY;
    q->count--;
    return pid;
}

uint32_t policy_readyq_peek(policy_readyq_t *q) {
    if (q->count == 0) return POLICY_NO_PID;
    return q->ready[q->head];
}

int policy_readyq_contains(policy_readyq_t *q, uint32_t pid) {
    for (uint32_t i = 0; i < q->count; i++) {
        uint32_t idx = (q->head + i) % POLICY_MAX_READY;
        if (q->ready[idx] == pid) return 1;
    }
    return 0;
}

int policy_readyq_remove(policy_readyq_t *q, uint32_t pid) {
    for (uint32_t i = 0; i < q->count; i++) {
        uint32_t idx = (q->head + i) % POLICY_MAX_READY;
        if (q->ready[idx] == pid) {
            /* 把后面的元素逐格前移，最后回退 tail */
            for (uint32_t j = i; j + 1 < q->count; j++) {
                uint32_t src = (q->head + j + 1) % POLICY_MAX_READY;
                q->ready[idx] = q->ready[src];
                idx = src;
            }
            q->tail = (q->tail + POLICY_MAX_READY - 1) % POLICY_MAX_READY;
            q->count--;
            return 0;
        }
    }
    return -1;
}

uint32_t policy_readyq_count(policy_readyq_t *q) { return q->count; }
int      policy_readyq_empty(policy_readyq_t *q) { return q->count == 0; }
