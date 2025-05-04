# Pico Threads
Pico Threads is a tiny preemptive threading library I made for the raspberry pi pico 2w. It allows you to multiplex one core of the process across up to 32 threads by default.

## Features
- Preemptive multiplexing
- thread_sleep(us) (in microseconds)
- thread_park() and thread_unpark() with built-in permit for thread_park (returns immediately if permit available)
- thread_yield() for yielding the processor to another thread
- thread_spin_lock, simple spin-lock implementation with yielding in the loop.

## Stats (measured on a raspberry pi pico 2w)
- Scheduling algorithm = Round robin (200us time slices)
- Context switch time = 2us

## Usage

```c

void entry_point() {
  while (1) {}
}

int main() {
  thread_init(); // Marks the calling thread as the main thread
  thread_create(entry_point); // Create a new thread starting at entry_point()
  while (1) {}
}
```
