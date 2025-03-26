#ifndef __SHM_CHANNEL_H__
#define __SHM_CHANNEL_H__

#include <stddef.h>   // for size_t
#include <semaphore.h>
#include "steque.h"

#define SHM_NAME_LEN 64
#define SHM_SEGMENT_SIZE 5712


typedef struct {
    char shm_name[SHM_NAME_LEN]; // e.g., "/proxy_shm_001"
    size_t size;
    void *addr;  // mmap address
    int fd;      // shm fd
} shm_segment_t;

// 共享内存中的布局
typedef struct {
    sem_t sem_proxy_ready;  // proxy waits here (reader)
    sem_t sem_cache_ready;  // cache waits here (writer)
    size_t datalen;
    int is_last_chunk; // 1 = 最后一块
    size_t total_file_size; // 文件总大小
    char data[]; // flexible array
} shm_payload_t;

// 创建 n 个共享内存段
void create_n_segments(int nsegments, size_t segsize, steque_t* free_segments);

// 创建并初始化共享内存段（Proxy用）
int shm_segment_create(shm_segment_t *seg, const char *name, size_t size);

// 在Cache中 attach 已存在的共享内存
int shm_segment_attach(shm_segment_t *seg, const char *name, size_t size);

// 销毁共享内存段（Proxy清理用）
int shm_segment_destroy(shm_segment_t *seg);

#endif // __SHM_CHANNEL_H__
