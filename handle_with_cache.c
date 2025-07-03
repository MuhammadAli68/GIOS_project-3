#include "gfserver.h"
#include "cache-student.h"

#define BUFSIZE (834)
#define CACHE_MQ_NAME "/cache_mq"

shm_pool_t shm_pool;

/*
 __.__
Replace with your implementation
 __.__
*/

ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void* arg) {
    // 1. Acquire a shared memory object from the pool
    shm_object_t *shm_obj = shm_pool_acquire();
    if (!shm_obj) {
        fprintf(stderr, "Failed to acquire shared memory segment.\n");
        return SERVER_FAILURE;
    }

    // 2. Send request to the cache with path + shm_name + shm_size
    if (send_request_to_cache(path, shm_obj->name, shm_pool.segment_size) < 0) {
        fprintf(stderr, "Failed to send request to cache.\n");
        shm_pool_release(shm_obj);
        return SERVER_FAILURE;
    }

    // 3. Wait until cache completes initial status (200/404) setup
    sem_wait(&shm_obj->write_complete);
    printf("proxy worker %lu waiting on response\n",(unsigned long)pthread_self());
    // 4. Handle cache miss
    if (shm_obj->status == 404 || shm_obj->file_size == (size_t)-1) {
        gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
        shm_pool_release(shm_obj);
        return 0;
    }

    // 5. Cache hit - send header
    gfs_sendheader(ctx, GF_OK, shm_obj->file_size);

    // 6. Start receiving and relaying chunks
    size_t bytes_sent = 0;
    while (bytes_sent < shm_obj->file_size) {
        // Wait until cache signals next chunk is ready
        sem_wait(&shm_obj->chunk_ready);

        size_t chunk_size = shm_obj->current_chunk_size;
        if (chunk_size == 0) {
            break; // EOF or cache error
        }

        // Send chunk to client
        ssize_t written = gfs_send(ctx, shm_obj->addr, chunk_size);
        if (written < 0) {
            fprintf(stderr, "gfs_send failed\n");
            shm_pool_release(shm_obj);
            return SERVER_FAILURE;
        }

        bytes_sent += written;

        // Notify cache that this chunk was read
        sem_post(&shm_obj->read_complete);
    }
    printf("Proxy worker: %lu finished working using shared memory: %s",(unsigned long)pthread_self(),shm_obj->name);
    // 7. Release shared memory back to pool
    shm_pool_release(shm_obj);
    return bytes_sent;
}


void shm_pool_init(int num_segments, size_t segment_size) {
    shm_pool.total_segments = num_segments;
    shm_pool.segment_size = segment_size;

    steque_init(&shm_pool.shm_queue);
    pthread_mutex_init(&shm_pool.lock, NULL);
    pthread_cond_init(&shm_pool.cond, NULL);

    for (size_t i = 0; i < num_segments; ++i) {
        shm_object_t *shm_obj = malloc(sizeof(shm_object_t));
        if (!shm_obj) {
            perror("malloc failed for shm_object_t");
            exit(EXIT_FAILURE);
        }

        snprintf(shm_obj->name, sizeof(shm_obj->name), "/shm_seg_%zu", i);

        int shm_fd = shm_open(shm_obj->name, O_CREAT | O_RDWR, 0666);
        if (shm_fd < 0) {
            perror("shm_open");
            exit(EXIT_FAILURE);
        }

        if (ftruncate(shm_fd, segment_size) < 0) {
            perror("ftruncate");
            exit(EXIT_FAILURE);
        }

        shm_obj->addr = mmap(NULL, segment_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_obj->addr == MAP_FAILED) {
            perror("mmap");
            exit(EXIT_FAILURE);
        }

        shm_obj->used = 0;
        shm_obj->file_size = 0;
        shm_obj->current_chunk_size = 0;
        shm_obj->status = 0;

        sem_init(&shm_obj->write_complete, 1, 0);
        sem_init(&shm_obj->read_complete, 1, 0);
        sem_init(&shm_obj->chunk_ready, 1, 0);

        // Store file descriptor for cleanup (optional)
        shm_obj->shm_fd = shm_fd;

        steque_enqueue(&shm_pool.shm_queue, shm_obj);
    }
    printf("SHARED MEMORY INITIALIZED WITH %d SEGMENTS",steque_size(&shm_pool.shm_queue));
}

int send_request_to_cache(const char *path, const char *shm_name, size_t shm_size) {
    mqd_t mq;
    cache_request_t request;

    // Fill request structure
    memset(&request, 0, sizeof(request));
    strncpy(request.path, path, MAX_PATH_LEN - 1);
    strncpy(request.shm_name, shm_name, MAX_SHM_NAME_LEN - 1);
    request.shm_size = shm_size;
    request.proxy_pid = getpid();

    // Open message queue for writing
    mq = mq_open(CACHE_MQ_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) {
        perror("send_request_to_cache: mq_open");
        return -1;
    }

    // Send the message
    if (mq_send(mq, (const char *)&request, sizeof(request), 0) == -1) {
        perror("send_request_to_cache: mq_send");
        mq_close(mq);
        return -1;
    }

    mq_close(mq);
    return 0;
}

void shm_pool_destroy() {
    pthread_mutex_lock(&shm_pool.lock);
    while (!steque_isempty(&shm_pool.shm_queue)) {
        shm_object_t *shm_obj = (shm_object_t *)steque_pop(&shm_pool.shm_queue);

        munmap(shm_obj->addr, shm_pool.segment_size);
        shm_unlink(shm_obj->name);

        sem_destroy(&shm_obj->write_complete);
        sem_destroy(&shm_obj->read_complete);
        sem_destroy(&shm_obj->chunk_ready);

        close(shm_obj->shm_fd);
        free(shm_obj);
    }
    pthread_mutex_unlock(&shm_pool.lock);

    pthread_mutex_destroy(&shm_pool.lock);
    pthread_cond_destroy(&shm_pool.cond);
}

shm_object_t *shm_pool_acquire() {
    pthread_mutex_lock(&shm_pool.lock);
    while (steque_isempty(&shm_pool.shm_queue)) {
        pthread_cond_wait(&shm_pool.cond, &shm_pool.lock);
    }

    shm_object_t *shm_obj = (shm_object_t *)steque_pop(&shm_pool.shm_queue);
    shm_obj->used = 1;
    pthread_mutex_unlock(&shm_pool.lock);
    printf("proxy worker %lu acquired shared memory segment: %s\n",(unsigned long)pthread_self(),shm_obj->name);
    return shm_obj;
}

void shm_pool_release(shm_object_t *shm_obj) {
    shm_obj->used = 0;
    shm_obj->file_size = 0;
    shm_obj->current_chunk_size = 0;
    shm_obj->status = 0;

    pthread_mutex_lock(&shm_pool.lock);
    steque_enqueue(&shm_pool.shm_queue, shm_obj);
    pthread_mutex_unlock(&shm_pool.lock);
    pthread_cond_broadcast(&shm_pool.cond);
    printf("added segment back to stqueue");
}
