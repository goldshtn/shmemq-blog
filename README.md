## Shared Memory Queue, Adaptive `pthread_mutex`, and Dynamic Tracing

In one of my recent training classes, I was asked to demonstrate some practical uses of shared memory. My knee-jerk reply was that shared memory can be used for inter-process communication and message-passing. In fact, most IPC mechanisms are based on shared memory in their implementation. The question was whether it's worth the effort to build a message-passing interface on top of shared memory queues, or whether sockets or pipes could produce a better result in terms of performance with a minimal implementation effort. The basic requirement is that two processes pass arbitrary-sized messages to one another -- each process waits for a message before sending a reply.

Well, your mileage may vary, but I decided to see how fast a naive queue implementation on top of shared memory can be. My original test was on Windows, but I ported it to Linux so that I can use dynamic tracing to see the impact of locking on the shared queue.

### Benchmark Structure

The heart of the implementation is in the [**shmemq.c**](shmemq.c) file. Here is the interface of the shared memory queue that we'll be using:

```
typedef struct _shmemq shmemq_t;

shmemq_t* shmemq_new         (char const* name, unsigned long max_count, unsigned int element_size);
bool      shmemq_try_enqueue (shmemq_t* self, void* element, int len);
bool      shmemq_try_dequeue (shmemq_t* self, void* element, int len);
void      shmemq_destroy     (shmemq_t* self, int unlink);
```

Simply put, a shared queue is identified by its name. You can put elements of size `element_size` in it, up to a statically-defined maximum of `max_count`. If more than `max_count` elements are in the queue, you can't enqueue any more elements in until someone dequeues them. The enqueue and dequeue functions are not blocking: they simply return `false` if the queue is full or empty, respectively.

The internal implementation of the queue is based on a circular buffer. There are two indices: the write index and the read index. When enqueuing an element, it goes into the write index -- unless the queue is full. When dequeuing an element, it is copied from the read index -- unless the queue is empty. The enqueue and dequeue operations advance the respective index. In the following ASCII diagram, the elements marked with an **x** are waiting for be dequeued.

```
read_index --\       write_index ----\
             |                       |
             |                       |
[ ] [ ] [ ] [x] [x] [x] [x] [x] [x] [ ] [ ] [ ] [ ]
```

The benchmark itself is in [**main.c**](main.c), and it creates two processes, using two queues. Their roles are almost symmetric. The server process waits for a message to arrive before sending a reply, and the client process sends a message first and then waits for a reply. This ping-pong match repeats `REPETITIONS` times. For example, here's the client code:

```
for (i = 0; i < REPETITIONS; ++i) {
  while (!shmemq_try_enqueue(server_queue, &msg, sizeof(msg)))
    ;
  while (!shmemq_try_dequeue(client_queue, &msg, sizeof(msg)))
    ;
}
```

### Synchronization

The queue indices must be protected from concurrent access. This is a naive solution, so I picked a simple mutex. It may be interesting to benchmark an implementation that doesn't rely on locking at all, but I was looking for simplicity in this particular benchmark. The `shmemq_try_enqueue` and `shmemq_try_dequeue` functions lock the mutex prior to checking or modifying the queue, and unlock it afterwards.

Let's take a look at some performance results when using a simple `pthread_mutex` with no clever optimizations. First, let's make the messages 256 bytes and repeat the send/receive ping-pong 1000000 times:

```
$ make run DATA_SIZE=256 REPETITIONS=1000000 ADAPTIVE_MUTEX=0
total data passed between client and server: 488.28 MB, 2.00 million packets
real    0m1.833s
user    0m2.258s
sys     0m1.371s
```

So, overall it took us 1.8 seconds to exchange 2 million messages. There's a considerable amount of time spent in the kernel, though, and it could only be explained by locking. Let's see which system calls we're making, using Brendan Gregg's [**syscount**](https://github.com/brendangregg/perf-tools/blob/master/syscount) script (an **ftrace** front-end):

```
$ make syscount DATA_SIZE=256 REPETITIONS=1000000 ADAPTIVE_MUTEX=0
Tracing while running: "./shmemq_bench /svrqueue /cliqueue"...
SYSCALL              COUNT
...some output snipped for brevity...
close                   10
open                    12
mmap                    21
write                   21
futex              5974233
```

Oh. Almost 6 million **futex** system calls might explain the huge amount of time spent in the kernel. This is vanilla `pthread_mutex`. But there's yet hope. We can make the mutex [*adaptive*](https://lwn.net/Articles/534758/), which means it would first spin in user-mode before giving up and making a system call:

```
#if ADAPTIVE_MUTEX
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
```

Let's take it for a spin (ha-ha!):

```
$ make syscount DATA_SIZE=256 REPETITIONS=1000000 ADAPTIVE_MUTEX=1
Tracing while running: "./shmemq_bench /svrqueue /cliqueue"...
SYSCALL              COUNT
...some output snipped for brevity...
close                   10
open                    12
mmap                    21
write                   21
futex                 5846
```

The same benchmark now generates less than 6,000 **futex** system calls -- 1000x fewer than previously! This means most conflicts were resolved in user-mode. How about the overall timing?

```
$ make run DATA_SIZE=256 REPETITIONS=1000000 ADAPTIVE_MUTEX=1
total data passed between client and server: 488.28 MB, 2.00 million packets
real    0m0.754s
user    0m1.354s
sys     0m0.148s
```

Well, that's a nice improvement. We're down to 0.754 seconds (from 1.833 seconds previously). Now that we have a better-performing solution, let's try it out with different message sizes:

```
$ make run DATA_SIZE=16
total data passed between client and server: 30.52 MB, 2.00 million packets
real    0m0.739s
user    0m1.341s
sys     0m0.131s

$ make run DATA_SIZE=128
total data passed between client and server: 244.14 MB, 2.00 million packets
real    0m0.737s
user    0m1.338s
sys     0m0.128s

$ make run DATA_SIZE=1024
total data passed between client and server: 1953.12 MB, 2.00 million packets
real    0m0.949s
user    0m1.673s
sys     0m0.217s

$ make run DATA_SIZE=8192
total data passed between client and server: 15625.00 MB, 2.00 million packets
real    0m3.221s
user    0m5.697s
sys     0m0.712s

$ make run DATA_SIZE=65536
total data passed between client and server: 125000.00 MB, 2.00 million packets
real    0m59.469s
user    1m34.108s
sys     0m15.756s
```

Well, it looks like small messages aren't very efficient -- there are some queue management overheads that dominate the running time. When messages get bigger (and don't fit in any level of cache anymore), performance goes down. But still, take a look at the 8KB numbers. We transferred 15GB of data (2 million 8KB messages) in 3.2 seconds. And that's with a fairly naive queue implementation and coarse-grained locking.

### Tracing

Now, I still had some questions. How often is the mutex contended when a thread attempts to acquire it? Even if the mutex was contended, what's the typical spin count before getting it anyway -- or giving up and making a system call? These are questions that are best answered with some form of dynamic tracing. Let's see if we can identify the location where `pthread_mutex_lock` gives up and stops spinning. Enter [**perf**](https://perf.wiki.kernel.org/index.php/Main_Page):

```
$ perf probe -x /lib64/libpthread.so.0 -L 'pthread_mutex_lock'
<__pthread_mutex_lock@/usr/src/debug/glibc-2.22/nptl/../nptl/pthread_mutex_lock.c:0>
      0  __pthread_mutex_lock (mutex)
              pthread_mutex_t *mutex;
      2  {
           assert (sizeof (mutex->__size) >= sizeof (mutex->__data));
...
58    else if (__builtin_expect (PTHREAD_MUTEX_TYPE (mutex)
                             == PTHREAD_MUTEX_ADAPTIVE_NP, 1))
        {
61        if (! __is_smp)
           goto simple;

64        if (LLL_MUTEX_TRYLOCK (mutex) != 0)
           {
66           int cnt = 0;
67           int max_cnt = MIN (MAX_ADAPTIVE_COUNT,
                                mutex->__data.__spins * 2 + 10);
             do
               {
71               if (cnt++ >= max_cnt)
                   {
73                   LLL_MUTEX_LOCK (mutex);
                     break;
                   }
76               atomic_spin_nop ();
               }
78           while (LLL_MUTEX_TRYLOCK (mutex) != 0);

80           mutex->__data.__spins += (cnt - mutex->__data.__spins) / 8;
           }
          assert (mutex->__data.__owner == 0);
...
```

What we have here are the annotated sources of `pthread_mutex_lock` (glibc debug info required). Specifically, we're dealing with an adaptive mutex, so we need lines 61-80. If the `LLL_MUTEX_TRYLOCK` macro fails to acquire the mutex, we begin spinning. The `cnt` variable is the number of spin iterations performed so far, and the `max_cnt` variable is the number of spin iterations to perform before giving up. If we get to line 73, spinning has failed and we perform a system call. So let's set up a probe and see how often we hit it:

```
# perf probe -f -x /lib64/libpthread.so.0 'pthread_mutex_lock:73'                                               
Added new event:
  probe_libpthread:pthread_mutex_lock (on pthread_mutex_lock:73 in /usr/lib64/libpthread-2.22.so)

You can now use it in all perf tools, such as:

        perf record -e probe_libpthread:pthread_mutex_lock -aR sleep 1

# perf stat -e 'probe_libpthread:pthread_mutex_lock' -a ./shmemq_bench /svrqueue /cliqueue
total data passed between client and server: 4882.81 MB, 20.00 million packets

 Performance counter stats for 'system wide':

            14,921      probe_libpthread:pthread_mutex_lock                                   

       7.313603914 seconds time elapsed

# perf probe --del=*
Removed event: probe_libpthread:pthread_mutex_lock
```

I could also have run **sudo perf record** to get the individual events, but I really only care about the count. In this case, the count was 14,921 -- that's the number of times the mutex was contended and we had to perform a system call.

But then I was hungry for even more details -- I wanted a histogram of `cnt` to understand how often we're spinning succesfully and how many iterations are typical. That's something you *could* do with **perf**, but it requires ugly hacks (such as [**perf-stat-hist**](https://github.com/brendangregg/perf-tools/blob/master/misc/perf-stat-hist), which only works with kernel tracepoints and not uprobes anyway). Instead, let's try [SystemTap](https://sourceware.org/systemtap/). Here is a script that places probes on multiple lines in `pthread_mutex_lock`, to track how often we acquire the mutex, how often it is already taken (contended), how often we give up spinning and do a system call, and what the distribution of spin counts is:

```
$ cat contention_stats.stp
global hist
global total
global contended
global syscalls

// Trying to acquire
probe process("/lib64/libpthread.so.0").statement("__pthread_mutex_lock@pthread_mutex_lock.c+64") {
        total++
}

// Lock was not immediately available, beginning to spin
probe process("/lib64/libpthread.so.0").statement("__pthread_mutex_lock@pthread_mutex_lock.c+66") {
        contended++
}

// Finished spinning one way or another
probe process("/lib64/libpthread.so.0").statement("__pthread_mutex_lock@pthread_mutex_lock.c+80") {
        numspins = $cnt
        hist <<< numspins
}

// Failed to spin, making a system call
probe process("/lib64/libpthread.so.0").statement("__pthread_mutex_lock@pthread_mutex_lock.c+73") {
        syscalls++
}

probe end {
        printf("total: %d contended: %d went to kernel: %d\n", total, contended, syscalls);
        println("histogram of spin iterations:");
        print(@hist_log(hist));
}

# stap contention_stats.stp -c "./shmemq_bench /svrqueue /cliqueue > /dev/null"
total: 63509587 contended: 722800 went to kernel: 1287
histogram of spin iterations:
value |-------------------------------------------------- count
    0 |                                                        0
    1 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  720918
    2 |                                                      256
    4 |                                                      203
    8 |                                                       69
   16 |                                                     1328
   32 |                                                       26
   64 |                                                        0
  128 |                                                        0

Pass 5: run completed in 15230usr/52340sys/34290real ms.
```

Now we have our answers! The total number of times the mutex was acquired is 6.35 million. It was only contended 722,800 times. And here's where the adaptive mutex really shines -- we only had to make a system call 1,287 times! What's more, in the vast majority of the cases (720,918 times out of 722,800 = 99.73% of the time) we only had to spin once.

### Summary

These somewhat-random musings do have a common theme. And the theme is observability. If you understand what your library is doing for you, and have the tools to observe and trace its behavior, tuning becomes much easier. Incidentally, the concepts also becomes much easier to explain. In this case, I was able to stretch a fairly naive shared memory queue implementation to quite decent performance.

*I'm also using Twitter for shorter bites, links, and comments. Follow me: [@goldshtn](https://twitter.com/goldshtn)*
