#define NUM_THREADS 32
#define TIME_QUANTUM_US 200
#define STACK_SIZE 2048
#define THREAD_NONE_READY -1
#define THREAD_NO_MORE -2
#define THREAD_NO_MEMORY -3

struct thread_context {
    void *stack_pointer;
};

enum thread_state {
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_EXITED
};

enum thread_signal {
    SIGNAL_NONE,
    SIGNAL_EXIT,
};

typedef enum thread_signal thread_signal_t;

struct thread {
    int id;
    _Atomic int permit;
    _Atomic int state;
    void (*entry)(void);
    void *stack;
    struct thread_context context;
    int stack_size;
    thread_signal_t signal;
};
typedef struct thread thread_t;

struct thread_spin_lock {
    _Atomic (thread_t*) owner;
};
typedef struct thread_spin_lock thread_spin_lock_t;

/**
 * @brief Initialize the threading system and main thread.
 *
 * Sets up the main thread record, claims a hardware alarm for
 * preemption, and forces the first IRQ to start scheduling.
 *
 * @return 0 on success or -1 on failure to claim an alarm.
 */
int thread_init();

/**
 * @brief Retrieve the currently running thread.
 *
 * @return Pointer to the current_thread.
 */
thread_t* get_current_thread();

/**
 * @brief Create and start a new thread running the given function.
 *
 * Allocates a stack and thread struct, initializes its context,
 * enqueues it on the ready queue, and returns its ID or an error.
 *
 * @param entry Function pointer to the thread's entry routine.
 * @return 0 on success, or THREAD_NO_MEMORY/THREAD_NO_MORE on failure.
 */
int thread_create(void (*entry)(void));

/**
 * @brief Yield execution by forcing the preemption IRQ.
 */
void thread_yield();

/**
 * @brief Terminate the current thread and schedule its destruction.
 *
 * Cancels the alarm, marks the thread EXITED, sets it as the next
 * zombie to destroy, and yields to the scheduler.
 */
void thread_exit();

/**
 * @brief Busy-sleep the current thread for at least `us` microseconds.
 *
 * If other threads are ready, yields to them; otherwise puts the processor to sleep
 * for single time-slice intervals until the target time is reached.
 *
 * @param us Number of microseconds to sleep.
 */
void thread_sleep(int us);

/**
 * @brief Park (block) the current thread until unparked.
 * If the permit is set, the thread will not sleep and this function
 * will return immediately.
 */
void thread_park();

/**
 * @brief Unpark (wake) a sleeping thread.
 *
 * If the thread is sleeping, it will wake up. If not, the
 * thread's permit is set atomically.
 *
 * @param thread Pointer to the thread to unpark.
 */
void thread_unpark(thread_t* thread);

void thread_lock_init(thread_spin_lock_t* lock);
void thread_lock_acquire(thread_spin_lock_t* lock);
void thread_lock_release(thread_spin_lock_t* lock);
