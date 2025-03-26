#include "shm_channel.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void create_n_segments(int nsegments, size_t segsize, steque_t* free_segments) {
    for (int i = 0; i < nsegments; i++) {
        char shm_name[SHM_NAME_LEN];
        snprintf(shm_name, SHM_NAME_LEN, "/proxy_shm_%d", i);

        shm_segment_t *seg = malloc(sizeof(shm_segment_t));
        if (shm_segment_create(seg, shm_name, segsize) < 0) {
            fprintf(stderr, "Failed to create shared memory: %s\n", shm_name);
            free(seg);
            continue;
        }

        steque_enqueue(free_segments, seg);
    }
}

int shm_segment_create(shm_segment_t *seg, const char *name, size_t size) {
    strncpy(seg->shm_name, name, SHM_NAME_LEN);
    seg->size = size;

    seg->fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (seg->fd < 0) return -1;
    if (ftruncate(seg->fd, size) < 0) return -1;

    seg->addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, seg->fd, 0);
    if (seg->addr == MAP_FAILED) return -1;

    // 初始化 semaphores
    shm_payload_t* payload = (shm_payload_t*)seg->addr;
    sem_init(&payload->sem_proxy_ready, 1, 0);
    sem_init(&payload->sem_cache_ready, 1, 0);
    payload->datalen = 0;

    return 0;
}

int shm_segment_attach(shm_segment_t *seg, const char *name, size_t size) {
    strncpy(seg->shm_name, name, SHM_NAME_LEN);
    seg->size = size;

    seg->fd = shm_open(name, O_RDWR, 0666);
    if (seg->fd < 0) return -1;

    seg->addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, seg->fd, 0);
    if (seg->addr == MAP_FAILED) return -1;

    return 0;
}

int shm_segment_destroy(shm_segment_t *seg) {
    shm_payload_t* payload = (shm_payload_t*) seg->addr;
    sem_destroy(&payload->sem_proxy_ready);
    sem_destroy(&payload->sem_cache_ready);
    
    munmap(seg->addr, seg->size);
    close(seg->fd);
    shm_unlink(seg->shm_name);
    return 0;
}
