#include "thread.h"
#include "pico/stdlib.h"
#include <stdlib.h>
#include <stdatomic.h>
#include "hardware/sync.h"
#include "hardware/timer.h"
#include <stdio.h>

static thread_t* current_thread = NULL;
static thread_t* thread_list[NUM_THREADS];
static thread_t* ready_queue[NUM_THREADS];
static _Atomic int ready_head = 0;
static _Atomic int ready_tail = 0;
static volatile int alarm_num = 0;
static volatile long num_preemptions = 0;
static volatile uint64_t time = 0;
static volatile thread_t* destroy_this_thread = NULL;

thread_t* get_current_thread() {
    return current_thread;
}

#define READY_QUEUE_LOCK_NUM 0

/**
 * @brief Enqueue a thread onto the ready queue in a thread-safe manner.
 *
 * Disables interrupts, acquires the spinlock, and adds the thread
 * if there is space in the circular buffer.
 *
 * @param thread Pointer to the thread to enqueue.
 */
void ready_queue_push(thread_t* thread) {
    spin_lock_t *lock = spin_lock_instance(READY_QUEUE_LOCK_NUM);
    uint32_t irq_state = spin_lock_blocking(lock);

    int next_tail = (ready_tail + 1) % NUM_THREADS;
    if (next_tail != ready_head) {
        ready_queue[ready_tail] = thread;
        ready_tail = next_tail;
    }

    spin_unlock(lock, irq_state);
}

/**
 * @brief Dequeue the next thread from the ready queue.
 *
 * Disables interrupts, acquires the spinlock, removes and returns
 * the head of the queue, or NULL if the queue is empty.
 *
 * @return Pointer to the next ready thread or NULL.
 */
thread_t* ready_queue_pop() {
    spin_lock_t *lock = spin_lock_instance(READY_QUEUE_LOCK_NUM);
    uint32_t irq_state = spin_lock_blocking(lock);

    if (ready_head == ready_tail) {
        spin_unlock(lock, irq_state);
        return NULL;
    }

    thread_t* thread = ready_queue[ready_head];
    ready_head = (ready_head + 1) % NUM_THREADS;

    spin_unlock(lock, irq_state);
    return thread;
}

__attribute__((naked)) void switch_stack(thread_t* from, thread_t* to) {
    __asm__ volatile (
        "vstmdb sp!, {S16-S31}             \n" // push vfp registers (wasn't doing this at first, very hard to find bug)
        "push {r4-r11, lr}                 \n" // push callee-saved registers (caller-saved pushed by hardware)
        "mov r2, %[sp_offset]              \n" // offset to stack pointer in thread struct
        "str sp, [r0, r2]                  \n" // store current stack pointer in 'from' thread
        "ldr sp, [r1, r2]                  \n" // load new stack pointer from 'to' thread
        "pop {r4-r11, lr}                  \n" // pop callee-saved registers
        "vldmia sp!, {S16-S31}             \n" // pop vfp registers
        "bx lr                             \n" // return to the new thread
        :
        : [sp_offset] "I" (offsetof(thread_t, context) + offsetof(struct thread_context, stack_pointer))
        : "r2"
    );
}

void thread_destroy(thread_t* thread) {
    thread_list[thread->id] = NULL;
    if (thread->stack) {
        free(thread->stack);
    }
    free(thread);
}

/**
 * @brief Clean up any thread marked for destruction.
 */
void handle_zombie() {
    if (destroy_this_thread) {
        thread_t* thread = (thread_t*) destroy_this_thread;
        destroy_this_thread = NULL;
        thread_destroy(thread);
    }
}

void handle_signal() {
    if (current_thread->signal == SIGNAL_EXIT) {
        thread_exit();
        assert(false);
    }
}

/**
 * @brief Perform a scheduling context switch if a ready thread exists.
 *
 * Pops the next thread from the ready queue, updates states of the
 * old and new threads, and switches stacks. After switching, handles
 * any zombies or signals.
 *
 * Must be called with alarms (preemption) disabled.
 *
 * @return 0 on success or THREAD_NONE_READY if no thread was ready.
 */
int context_switch() {
    thread_t* next_thread = ready_queue_pop();
    if (next_thread == NULL) {
        return THREAD_NONE_READY;
    }

    int expect = THREAD_RUNNING;
    if (atomic_compare_exchange_strong(&current_thread->state, &expect, THREAD_READY)) {
        ready_queue_push(current_thread);
    }
    thread_t* old_thread = current_thread;
    current_thread = next_thread;
    current_thread->state = THREAD_RUNNING;
    thread_t* prev_thread = old_thread;

    switch_stack(prev_thread, current_thread);

    handle_zombie();
    handle_signal();

    return 0;
}

// Entry point for the preemption interrupt handler.
static void preempt_entry(uint alarm_num) {
    hardware_alarm_cancel(alarm_num);
    num_preemptions++;

    if (current_thread->state == THREAD_SLEEPING && current_thread->permit > 0) {
        current_thread->permit--;
        current_thread->state = THREAD_RUNNING;
        goto end;
    }

    context_switch();

end:
    uint64_t now = time_us_64();
    hardware_alarm_set_target(alarm_num, now + TIME_QUANTUM_US);
}

// Thread entry-point.
void thread_stub(int thread_id) {
    uint64_t time = time_us_64();
    hardware_alarm_set_target(0, time + TIME_QUANTUM_US);

    current_thread->entry();

    thread_exit();
}

int thread_init() {
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_list[i] = NULL;
    }

    thread_t* main_thread = (thread_t*)malloc(sizeof(thread_t));
    main_thread->id = 0;
    main_thread->state = THREAD_RUNNING;
    main_thread->entry = NULL;
    main_thread->stack = NULL;
    main_thread->stack_size = 0;
    main_thread->permit = 0;
    main_thread->context.stack_pointer = NULL;
    main_thread->signal = SIGNAL_NONE;
    current_thread = main_thread;
    thread_list[0] = main_thread;

    alarm_num = hardware_alarm_claim_unused(false);
    if (alarm_num < 0) {
        return -1;
    }
    hardware_alarm_set_callback(alarm_num, preempt_entry);
    hardware_alarm_force_irq(alarm_num);
    return 0;
}

int thread_create(void (*entry)(void)) {
    thread_t* new_thread = (thread_t*)malloc(sizeof(thread_t));
    if (!new_thread) return THREAD_NO_MEMORY;

    new_thread->stack = malloc(STACK_SIZE);
    new_thread->stack_size = STACK_SIZE;
    if (!new_thread->stack) {
        free(new_thread);
        return THREAD_NO_MEMORY;
    }

    int id = -1;
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_t* expect = NULL;
        if (atomic_compare_exchange_strong(&thread_list[i], &expect, new_thread)) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        free(new_thread->stack);
        free(new_thread);
        return THREAD_NO_MORE;
    }

    new_thread->id = id;
    new_thread->state = THREAD_READY;
    new_thread->entry = entry;
    new_thread->permit = 0;
    new_thread->signal = SIGNAL_NONE;

    // Set up that stack
    void *sp_byte = new_thread->stack + new_thread->stack_size;
    sp_byte -= (uint32_t) sp_byte % 16;
    uint32_t *sp = (uint32_t *)sp_byte;
    sp -= 8;

    // Exception frame setup
    sp[0] = (uint32_t)   new_thread->id;     // R0 (first argument to thread_stub)
    sp[1] = (uint32_t)    0x00000000;        // R1
    sp[2] = (uint32_t)    0x00000000;        // R2
    sp[3] = (uint32_t)    0x00000000;        // R3
    sp[4] = (uint32_t)    0x00000000;        // R12
    sp[5] = (uint32_t)    0x00000000;        // LR TODO set to thread_exit
    sp[6] = (uint32_t)   thread_stub;        // PC
    sp[7] = (uint32_t)      (1U<<24);        // xPSR (Thumb bit set)
    
    // Extra stack when switching during stack_switch()
    sp -= 25; // 16 are for S16-S31, 8 for R4-R11, and 1 for LR
    for (int i = 0; i < 8; i++) {
        sp[i] = 0x00000000; // R4-R11
    }

    // What will go into LR. This is the EXC_RETURN magic value for
    // the armv8 architecture (see manual). Specifies that the hardware
    // should pop the exception frame off the stack and return to
    // thread-mode.
    sp[8] = (uint32_t)0xFFFFFFF9; // What will go into LR

    new_thread->context.stack_pointer = sp;

    ready_queue_push(new_thread);
    return 0;
}

void thread_exit() {
    hardware_alarm_cancel(alarm_num);
    current_thread->state = THREAD_EXITED;
    destroy_this_thread = current_thread;
    thread_yield();
    assert(false);
}

void thread_yield() {
    hardware_alarm_force_irq(alarm_num);
}

void thread_sleep(int us) {
    if (us == 0) return;

    uint64_t start = time_us_64();
    uint64_t target = start + us;
    while (time_us_64() < target) {
        int ready_count = (ready_tail - ready_head + NUM_THREADS) % NUM_THREADS;
        if (ready_count > 0) {
            thread_yield();
        } else {
            sleep_us(TIME_QUANTUM_US);
        }
    }
}

void thread_park() {
    current_thread->state = THREAD_SLEEPING;
    thread_yield();
}

void thread_unpark(thread_t* thread) {
    thread->permit = 1;
    int expect = THREAD_SLEEPING;
    if (atomic_compare_exchange_strong(&thread->state, &expect, THREAD_READY)) {
        thread->permit = 0;
        ready_queue_push(thread);
    }
}


void thread_lock_init(struct thread_spin_lock* lock) {
    lock->owner = NULL;
}

void thread_lock_acquire(struct thread_spin_lock* lock) {
    thread_t* self = get_current_thread();
    thread_t* expected = NULL;
    while (!atomic_compare_exchange_strong(&lock->owner, &expected, self)) {
        expected = NULL;
        thread_yield();
    }
}

void thread_lock_release(struct thread_spin_lock* lock) {
    thread_t* self = get_current_thread();
    if (lock->owner == self) {
        lock->owner = NULL;
    }
}