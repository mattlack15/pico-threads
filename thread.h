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


int thread_init();
thread_t* get_current_thread();
int thread_create(void (*entry)(void));
void thread_yield();
void thread_exit();
void thread_sleep(int ms);

void thread_lock_init(thread_spin_lock_t* lock);
void thread_lock_lock(thread_spin_lock_t* lock);
void thread_lock_release(thread_spin_lock_t* lock);
