#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <stdio.h>

#include "shmemq.h"

#ifndef DATA_SIZE
  #define DATA_SIZE 256
#endif

struct message {
  int type;
  char data[DATA_SIZE - sizeof(int)];
};

#ifndef REPETITIONS
  #define REPETITIONS 100000
#endif

#define QUEUE_SIZE 1000

void server(char const* server_queue_name, char const* client_queue_name) {
  shmemq_t *server_queue, *client_queue;
  int i;
  struct message msg = { .type = 42, .data = "Hello" };

  server_queue = client_queue = NULL;

  server_queue = shmemq_new(server_queue_name, QUEUE_SIZE, sizeof(struct message));
  if (!server_queue) {
    perror("error creating server queue");
    goto FAIL;
  } 
  client_queue = shmemq_new(client_queue_name, QUEUE_SIZE, sizeof(struct message));
  if (!client_queue) {
    perror("error creating client queue");
    goto FAIL;
  }

  printf("server started on queue %s with client queue %s\n", server_queue_name, client_queue_name);

  for (i = 0; i < REPETITIONS; ++i) {
    while (!shmemq_try_dequeue(server_queue, &msg, sizeof(msg)))
      ;
    while (!shmemq_try_enqueue(client_queue, &msg, sizeof(msg)))
      ;
    if (i % (REPETITIONS/10) == 0) {
      printf("s");
      fflush(stdout);
    }
  }

FAIL:
  if (server_queue)
    shmemq_destroy(server_queue, 0);
  if (client_queue)
    shmemq_destroy(client_queue, 0);
}

void client(char const* client_queue_name, char const* server_queue_name) {
  shmemq_t *server_queue, *client_queue;
  int i;
  struct message msg = { .type = 42, .data = "Hello" };

  server_queue = client_queue = NULL;

  server_queue = shmemq_new(server_queue_name, QUEUE_SIZE, sizeof(struct message));
  if (!server_queue) {
    perror("error creating server queue");
    goto FAIL;
  } 
  client_queue = shmemq_new(client_queue_name, QUEUE_SIZE, sizeof(struct message));
  if (!client_queue) {
    perror("error creating client queue");
    goto FAIL;
  }

  printf("client started on queue %s with server queue %s\n", client_queue_name, server_queue_name);

  for (i = 0; i < REPETITIONS; ++i) {
    while (!shmemq_try_enqueue(server_queue, &msg, sizeof(msg)))
      ;
    while (!shmemq_try_dequeue(client_queue, &msg, sizeof(msg)))
      ;
    if (i % (REPETITIONS/10) == 0) {
      printf("c");
      fflush(stdout);
    }
  }

FAIL:
  if (server_queue)
    shmemq_destroy(server_queue, 0);
  if (client_queue)
    shmemq_destroy(client_queue, 0);
}

int main(int argc, char* argv[]) {
  pid_t pid, server_pid, client_pid;
  shmemq_t *server_queue, *client_queue;

  if (argc < 3) {
    printf("USAGE: %s <server_queue_name> <client_queue_name>\n", argv[0]);
    return 1;
  }

  // Preallocate the queues so that there is no race when the processes access them
  server_queue = shmemq_new(argv[1], QUEUE_SIZE, sizeof(struct message));
  client_queue = shmemq_new(argv[2], QUEUE_SIZE, sizeof(struct message)); 

  if (!server_queue || !client_queue) {
    printf("error creating queues\n");
    return 1;
  }

  if ((server_pid = fork()) == 0) {
    server(argv[1], argv[2]);
  } else {
    if ((client_pid = fork()) == 0) {
      client(argv[2], argv[1]);
    } else {
      int status, i, pid;
      for (i = 0; i < 2; ++i) {
        pid = wait(&status);
        if (!WIFEXITED(status)) {
          printf("child %d did not exit successfully (server was %d client was %d)!\n", pid, server_pid, client_pid);
          if (WIFSIGNALED(status)) printf("  killed by signal %d\n", WTERMSIG(status));
        }
      }
      printf("\n\ntotal data passed between client and server: %2.2f MB, %2.2f million packets\n",
             2.0f*DATA_SIZE*REPETITIONS/(1024*1024), 2*REPETITIONS/1e6f);
      shmemq_destroy(server_queue, 1);
      shmemq_destroy(client_queue, 1);
    }
  }

  return 0;
}
