#include "proxy-student.h"
#include "gfserver.h"



#define MAX_REQUEST_N 512
#define BUFSIZE (6226)

// Structure to store downloaded data in chunks
struct memory_chunk {
    char *memory;
    size_t size;
};

// Callback function for libcurl, writes data to memory
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    struct memory_chunk *mem = (struct memory_chunk *)userp;

    // Allocate or expand buffer
    char *ptr = realloc(mem->memory, mem->size + total_size);
    if (ptr == NULL) return 0; // Out of memory

    memcpy(ptr + mem->size, contents, total_size);
    mem->memory = ptr;
    mem->size += total_size;

    return total_size;
}

ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void* arg) {
    CURL *curl;
    CURLcode res;
    struct memory_chunk chunk;
    chunk.memory = NULL;
    chunk.size = 0;

    // Construct full URL
    const char *base_url = (const char *)arg;
    char url[1024];  // Ensure enough space
    snprintf(url, sizeof(url), "%s%s", base_url, path);

    // Initialize CURL
    curl = curl_easy_init();
    if (!curl) return SERVER_FAILURE;

    // Set up CURL options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Handle redirects

    // Perform the request
    res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    // Handle HTTP response codes
    if (res != CURLE_OK || http_code != 200) {
        free(chunk.memory);
        return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
    }

    // Send header to client
    gfs_sendheader(ctx, GF_OK, chunk.size);

    // Send data in chunks
    ssize_t total_sent = 0;
    while (total_sent < chunk.size) {
        ssize_t sent = gfs_send(ctx, chunk.memory + total_sent, chunk.size - total_sent);
        if (sent < 0) {
            free(chunk.memory);
            return SERVER_FAILURE;
        }
        total_sent += sent;
    }

    // Cleanup
    free(chunk.memory);
    return total_sent;
}

/*
 * We provide a dummy version of handle_with_file that invokes handle_with_curl as a convenience for linking!
 */
ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void* arg){
	return handle_with_curl(ctx, path, arg);
}	
