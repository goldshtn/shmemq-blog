#include <stdbool.h>

typedef struct _shmemq shmemq_t;

shmemq_t* shmemq_new(char const* name, unsigned long max_count, unsigned int element_size);
bool shmemq_try_enqueue(shmemq_t* self, void* element, int len);
bool shmemq_try_dequeue(shmemq_t* self, void* element, int len);
void shmemq_destroy(shmemq_t* self, int unlink); 
