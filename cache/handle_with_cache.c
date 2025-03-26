#include "gfserver.h"
#include "cache-student.h"
#include "shm_channel.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/cache_socket"
#define MAX_RETRIES 5
#define RETRY_DELAY_SEC 1

ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void* arg) {
    printf("[PROXY] handle_with_cache called with path: %s\n", path);

    void** args = (void**) arg;
    steque_t* pool = (steque_t*) args[0];
    size_t segsize = *((size_t*) args[1]);
    pthread_mutex_t* lock = (pthread_mutex_t*) args[2];

    pthread_mutex_lock(lock);
    if (steque_isempty(pool)) {
        pthread_mutex_unlock(lock);
        fprintf(stderr, "No free shared memory segments!\n");
        return gfs_sendheader(ctx, GF_ERROR, 0);
    }
    shm_segment_t* seg = steque_pop(pool);
    pthread_mutex_unlock(lock);

    shm_payload_t* payload = (shm_payload_t*) seg->addr;

    char request[1024];
    snprintf(request, sizeof(request), "%s %s %zu\n", seg->shm_name, path, segsize);

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int connected = 0;
    for (int i = 0; i < 5; ++i) {
        if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }
        sleep(1);
    }
    if (!connected) {
        close(sockfd);
        fprintf(stderr, "[PROXY] ERROR reason A\n");
        goto error;
    }

    if (write(sockfd, request, strlen(request)) < 0) {
        close(sockfd);
        fprintf(stderr, "[PROXY] ERROR reason A\n");
        goto error;
    }
    close(sockfd);

    // wait for the first chunk before sending header
    printf("[PROXY] waiting on sem_proxy_ready for %s\n", path);
    if (sem_wait(&payload->sem_proxy_ready) < 0) {
        perror("sem_wait");
        goto error;
    }

    if (payload->datalen == 0) {
        if (payload->is_last_chunk) {
            fprintf(stderr, "[PROXY] cache miss or empty file: %s\n", path);
        } else {
            fprintf(stderr, "[PROXY] unexpected zero-length chunk for: %s\n", path);
        }
        goto error;
    }

    size_t total_file_size = payload->total_file_size;
    if (gfs_sendheader(ctx, GF_OK, total_file_size) < 0) {
        fprintf(stderr, "[PROXY] failed to send header for %s\n", path);
        goto error;
    }
    fprintf(stderr, "[PROXY] sent GF_OK header for %s\n", path);

    ssize_t total_sent = 0;


    while (1) {
        printf("[PROXY] got chunk: %zu bytes, last=%d for path=%s\n", payload->datalen, payload->is_last_chunk, path);
        
        if (payload->datalen > segsize - sizeof(*payload)) {
            fprintf(stderr, "[PROXY] chunk length overflow: %zu\n", payload->datalen);
            goto error;
        }

        printf("[PROXY] sending chunk: %zu bytes, last=%d\n", payload->datalen, payload->is_last_chunk);
        ssize_t sent = gfs_send(ctx, payload->data, payload->datalen);
        if (sent < 0) {
            perror("[PROXY] gfs_send failed");
            goto error;
        }
        total_sent += sent;

        sem_post(&payload->sem_cache_ready);

        if (payload->is_last_chunk) {
            fprintf(stderr, "[PROXY] finished sending all data for %s\n", path);
            break;
        }

        if (sem_wait(&payload->sem_proxy_ready) < 0) {
            perror("sem_wait");
            goto error;
        }
    }

    pthread_mutex_lock(lock);
    steque_enqueue(pool, seg);
    pthread_mutex_unlock(lock);

    return total_sent;

error:
    pthread_mutex_lock(lock);
    steque_enqueue(pool, seg);
    pthread_mutex_unlock(lock);
    return gfs_sendheader(ctx, GF_ERROR, 0);
}
