/*
 You can use this however you want.
 */
 #ifndef __CACHE_STUDENT_H__844
 
 #define __CACHE_STUDENT_H__844

 #include "steque.h"

#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "steque.h"

#define SHM_NAME_PREFIX "/shm_seg_"
#define MAX_SHM_NAME_LEN 64
#define MAX_PATH_LEN 1024
#define MAX_SEGMENTS 256

/* Shared memory object used in the pool */
/* Shared memory object used in the pool */
typedef struct {
    char name[MAX_SHM_NAME_LEN];
    int shm_fd;                     // <-- file descriptor for shm segment
    void* addr;                     // pointer to shared memory segment
    size_t file_size;              // total file size (only used by proxy)
    int used;                      // 0 = free, 1 = in use
    sem_t write_complete;          // stops proxy when cache is writing
    sem_t read_complete;           // stops cache until proxy reads
    sem_t chunk_ready;             // sync per-chunk copy
    int status;                    // 200 or 404
    size_t current_chunk_size;     // actual chunk size
} shm_object_t;

/* Request structure used to communicate from proxy to cache */
typedef struct {
    char path[MAX_PATH_LEN];
    char shm_name[MAX_SHM_NAME_LEN];
    size_t shm_size;
    pid_t proxy_pid;
} cache_request_t;

/* Shared memory pool structure */
typedef struct {
    steque_t shm_queue;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int total_segments;
    size_t segment_size;
} shm_pool_t;

/* Global pool instance (to be initialized in webproxy.c) */
extern shm_pool_t shm_pool;

/* Function prototypes */
void shm_pool_init(int num_segments, size_t segment_size);
void shm_pool_destroy();
shm_object_t* shm_pool_acquire();
void shm_pool_release(shm_object_t* shm_obj);


 #endif // __CACHE_STUDENT_H__844