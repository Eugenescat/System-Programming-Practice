#include <stdio.h>
#include <unistd.h>
#include <printf.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"
#include "gfserver.h"
#include "steque.h"
#include <sys/un.h>
#include <sys/mman.h>

// CACHE_FAILURE
#if !defined(CACHE_FAILURE)
    #define CACHE_FAILURE (-1)
#endif 

#define MAX_CACHE_REQUEST_LEN 6100
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 782  

#define SOCKET_PATH "/tmp/cache_socket"

unsigned long int cache_delay;

static int server_fd;
static steque_t request_queue;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;

typedef struct {
    char shm_name[64];
    char key[1024];
	size_t segment_size;
} cache_task_t;

static void _sig_handler(int signo){
	if (signo == SIGTERM || signo == SIGINT){
		// This is where your IPC clean up should occur
		unlink(SOCKET_PATH);
        if (server_fd > 0) close(server_fd);
		simplecache_destroy();
		exit(signo);
	}
}

static void* _worker_thread(void *arg) {
	printf("[DEBUG] Cache worker thread started\n");

    (void)arg;
    while (1) {
        pthread_mutex_lock(&queue_lock);
        while (steque_isempty(&request_queue)) {
            pthread_cond_wait(&queue_not_empty, &queue_lock);
        }

        cache_task_t *task = steque_pop(&request_queue);
        pthread_mutex_unlock(&queue_lock);

		printf("[CACHE-WORKER] Handling %s (shm: %s, size: %zu)\n", task->key, task->shm_name, task->segment_size);

        shm_segment_t seg;
        if (shm_segment_attach(&seg, task->shm_name, task->segment_size) < 0) {
            fprintf(stderr, "[CACHE] failed to attach shm: %s\n", task->shm_name);
            free(task);
            continue;
        }

        shm_payload_t* payload = (shm_payload_t*)seg.addr;
        size_t max_chunk_size = task->segment_size - sizeof(*payload);

        int fd = simplecache_get(task->key);
        if (fd < 0) {
            fprintf(stderr, "[CACHE] miss: %s\n", task->key);
            payload->datalen = 0;
            payload->is_last_chunk = 1;
            sem_post(&payload->sem_proxy_ready);
            free(task);
            continue;
        }

		int my_fd = dup(fd);  // 线程独立使用自己的副本 fd
		if (my_fd < 0) {
			perror("dup failed");
			free(task);
            continue;
		}

		struct stat st;
		if (fstat(my_fd, &st) < 0) {
			perror("[CACHE] fstat failed");
			close(my_fd);
			free(task);
			continue;
		}
		payload->total_file_size = st.st_size;

        ssize_t n;
		off_t offset = 0;
        while (1) {

			n = pread(my_fd, payload->data, max_chunk_size, offset);

            if (n < 0) {
                perror("[CACHE] read error");
                break;
            }

			if (n == 0) {
                payload->datalen = 0;
                payload->is_last_chunk = 1;
                sem_post(&payload->sem_proxy_ready);
                break;
            }
			offset += n;

            payload->datalen = n;
			printf("[CACHE] read %zd bytes from file: %s\n", n, task->key);

            payload->is_last_chunk = (n < (ssize_t)max_chunk_size); // 最后一块判断

            sem_post(&payload->sem_proxy_ready);
			printf("[CACHE] posted sem_proxy_ready for %s\n", task->key);
            sem_wait(&payload->sem_cache_ready);

            if (payload->is_last_chunk) break;
        }
		close(my_fd);
        free(task);
    }
    return NULL;
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
	printf("[CACHE] started and listening on %s\n", SOCKET_PATH);
	fflush(stdout);
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

	steque_init(&request_queue);

	// Cache should go here
	// 创建工作线程池
	for (int i = 0; i < nthreads; i++) {
		pthread_t tid;
		pthread_create(&tid, NULL, _worker_thread, NULL);
		pthread_detach(tid);
	}

	// Boss thread: 接收 proxy 的请求
	unlink(SOCKET_PATH);
	server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(CACHE_FAILURE);
	}
	if (listen(server_fd, 32) < 0) {
		perror("listen");
		exit(CACHE_FAILURE);
	}

	while (1) {
		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0) continue;

		char buf[MAX_CACHE_REQUEST_LEN];
		ssize_t len = read(client_fd, buf, sizeof(buf) - 1);
		close(client_fd);
		if (len <= 0) continue;

		buf[len] = '\0';
		char *shm_name = strtok(buf, " ");
		char *key = strtok(NULL, " ");
		char *size_str = strtok(NULL, " \n");
		printf("[CACHE-BOSS] Received shm_name=%s, key=%s, size=%s\n", shm_name, key, size_str);
		size_t segment_size = SHM_SEGMENT_SIZE;  // 默认 fallback
		if (size_str != NULL) {
			segment_size = (size_t) atol(size_str);
		}
		key[strcspn(key, "\n")] = '\0'; // remove trailing newline
		if (!shm_name || !key) continue;

		cache_task_t *task = malloc(sizeof(cache_task_t));
		strncpy(task->shm_name, shm_name, sizeof(task->shm_name));
		strncpy(task->key, key, sizeof(task->key));
		task->segment_size = segment_size;

		pthread_mutex_lock(&queue_lock);
		steque_enqueue(&request_queue, task);
		pthread_cond_signal(&queue_not_empty);
		printf("[DEBUG] Boss enqueue: %s\n", task->key);
		pthread_mutex_unlock(&queue_lock);
	}

	// Line never reached
	return -1;
}
