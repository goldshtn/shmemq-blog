REPETITIONS = 1000000
DATA_SIZE = 256
ADAPTIVE_MUTEX = 1

all: main.c shmemq.c
	gcc -g -DADAPTIVE_MUTEX=$(ADAPTIVE_MUTEX) -DDATA_SIZE=$(DATA_SIZE) -DREPETITIONS=$(REPETITIONS) -lrt -lpthread -std=c99 main.c shmemq.c -o shmemq_bench

run: all
	rm -f /dev/shm/*queue && time ./shmemq_bench /svrqueue /cliqueue && rm -f /dev/shm/*queue

probe: all
	sudo perf probe -f -x /lib64/libpthread.so.0 'pthread_mutex_lock:73 cnt max_cnt'
	sudo perf stat -e 'probe_libpthread:pthread_mutex_lock' -a ./shmemq_bench /svrqueue /cliqueue
	sudo perf probe --del='probe_libpthread:pthread_mutex_lock*'

stap: all
	sudo stap -v contention_stats.stp -c "./shmemq_bench /svrqueue /cliqueue"

syscount: all
	sudo ../perf-tools/syscount -c ./shmemq_bench /svrqueue /cliqueue

clean:
	sudo rm -f /dev/shm/*queue
	rm shmemq_bench
