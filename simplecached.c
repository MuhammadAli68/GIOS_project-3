#include <stdio.h>
#include <unistd.h>
#include <printf.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <mqueue.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"
#include "gfserver.h"

// CACHE_FAILURE
#if !defined(CACHE_FAILURE)
    #define CACHE_FAILURE (-1)
#endif 

#define MAX_CACHE_REQUEST_LEN 6100
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 782  

#define CACHE_MQ_NAME "/cache_mq"
#define MAX_MSG_SIZE sizeof(cache_request_t)

mqd_t cache_mq;

unsigned long int cache_delay;

void* cache_worker(void* arg);

static void _sig_handler(int signo){
	if (signo == SIGTERM || signo == SIGINT){
		// This is where your IPC clean up should occur
		mq_close(cache_mq);
		mq_unlink(CACHE_MQ_NAME);
		exit(signo);
	}
}

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  simplecached [options]\n"                                                  \
"options:\n"                                                                  \
"  -c [cachedir]       Path to static files (Default: ./)\n"                  \
"  -t [thread_count]   Thread count for work queue (Default is 8, Range is 1-100)\n"      \
"  -d [delay]          Delay in simplecache_get (Default is 0, Range is 0-2500000 (microseconds)\n "	\
"  -h                  Show this help message\n"

//OPTIONS
static struct option gLongOptions[] = {
  {"cachedir",           required_argument,      NULL,           'c'},
  {"nthreads",           required_argument,      NULL,           't'},
  {"help",               no_argument,            NULL,           'h'},
  {"hidden",			 no_argument,			 NULL,			 'i'}, /* server side */
  {"delay", 			 required_argument,		 NULL, 			 'd'}, // delay.
  {NULL,                 0,                      NULL,             0}
};

void Usage() {
  fprintf(stdout, "%s", USAGE);
}

int main(int argc, char **argv) {
	int nthreads = 8;
	char *cachedir = "locals.txt";
	char option_char;

	/* disable buffering to stdout */
	setbuf(stdout, NULL);

	while ((option_char = getopt_long(argc, argv, "d:ic:hlt:x", gLongOptions, NULL)) != -1) {
		switch (option_char) {
			default:
				Usage();
				exit(1);
			case 't': // thread-count
				nthreads = atoi(optarg);
				break;				
			case 'h': // help
				Usage();
				exit(0);
				break;    
            case 'c': //cache directory
				cachedir = optarg;
				break;
            case 'd':
				cache_delay = (unsigned long int) atoi(optarg);
				break;
			case 'i': // server side usage
			case 'o': // do not modify
			case 'a': // experimental
				break;
		}
	}

	if (cache_delay > 2500000) {
		fprintf(stderr, "Cache delay must be less than 2500000 (us)\n");
		exit(__LINE__);
	}

	if ((nthreads>100) || (nthreads < 1)) {
		fprintf(stderr, "Invalid number of threads must be in between 1-100\n");
		exit(__LINE__);
	}
	if (SIG_ERR == signal(SIGINT, _sig_handler)){
		fprintf(stderr,"Unable to catch SIGINT...exiting.\n");
		exit(CACHE_FAILURE);
	}
	if (SIG_ERR == signal(SIGTERM, _sig_handler)){
		fprintf(stderr,"Unable to catch SIGTERM...exiting.\n");
		exit(CACHE_FAILURE);
	}
	/*Initialize cache*/
	simplecache_init(cachedir);

	// Cache should go here
	struct mq_attr attr;
	attr.mq_flags = 0;
	attr.mq_maxmsg = MAX_SIMPLE_CACHE_QUEUE_SIZE;
	attr.mq_msgsize = MAX_MSG_SIZE;
	attr.mq_curmsgs = 0;

	mq_unlink(CACHE_MQ_NAME); // Ensure old queue is gone
	cache_mq = mq_open(CACHE_MQ_NAME, O_CREAT | O_RDONLY, 0644, &attr);
	if (cache_mq == -1) {
		perror("mq_open");
		exit(EXIT_FAILURE);
	}

	// Create worker threads
	pthread_t workers[nthreads];
	for (int i = 0; i < nthreads; ++i) {
		if (pthread_create(&workers[i], NULL, cache_worker, NULL) != 0) {
			perror("pthread_create");
			exit(EXIT_FAILURE);
		}
	}
	// Line never reached
	return -1;
}

void* cache_worker(void* arg) {
    cache_request_t request;

    while (1) {
        ssize_t received = mq_receive(cache_mq, (char*)&request, MAX_MSG_SIZE, NULL);
        if (received == -1) {
            perror("mq_receive");
            continue;
        }

        // Locate and map the shared memory region
        int shm_fd = shm_open(request.shm_name, O_RDWR, 0666);
        if (shm_fd < 0) {
            perror("shm_open (worker)");
            continue;
        }

        void* addr = mmap(NULL, request.shm_size, PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (addr == MAP_FAILED) {
            perror("mmap (worker)");
            close(shm_fd);
            continue;
        }

        shm_object_t* shm_obj = (shm_object_t*)addr;

        // Set default: file not found
        shm_obj->status = 404;
        shm_obj->file_size = (size_t)-1;

        int fd = simplecache_get(request.path);

		printf("Cache worker %d providing response to request in segment %s\n",pthread_self(),shm_obj->name);
        if (fd >= 0) {
            shm_obj->status = 200;

            struct stat st;
            fstat(fd, &st);
            shm_obj->file_size = st.st_size;

            size_t total_sent = 0;
            ssize_t read_bytes;

            char* dest = (char*)shm_obj->addr;
            while ((read_bytes = read(fd, dest, request.shm_size)) > 0) {
                shm_obj->current_chunk_size = read_bytes;

                // Signal to proxy: chunk is ready
                sem_post(&shm_obj->chunk_ready);

                // Wait for proxy to read before writing next
                sem_wait(&shm_obj->read_complete);
                total_sent += read_bytes;
            }
			printf("Cache worker %d finished sending %d bytes of file",pthread_self(),shm_obj->file_size);
            close(fd);
        }
        // Signal completion of metadata (status and file_size)
        sem_post(&shm_obj->write_complete);

        munmap(addr, request.shm_size);
        close(shm_fd);
    }

    return NULL;
}
