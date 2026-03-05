#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "caesar.h"

#define BLOCK_SIZE 2048
#define MAX_BLOCKS 4

volatile sig_atomic_t keep_running = 1;

typedef struct {
    char data[MAX_BLOCKS][BLOCK_SIZE];
    size_t sizes[MAX_BLOCKS];
    int head;
    int tail;
    int count;
    int producer_done;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} shared_buffer_t;

typedef struct {
    const char *input_file;
    const char *output_file;
    shared_buffer_t *buffer;
    size_t file_size;
    size_t *processed;
    pthread_mutex_t *progress_mutex;
} thread_args_t;

void buffer_init(shared_buffer_t *buf) {
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->producer_done = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
}

void buffer_destroy(shared_buffer_t *buf) {
    pthread_mutex_destroy(&buf->mutex);
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
}

void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

void* producer_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    shared_buffer_t *buf = args->buffer;
    FILE *in = fopen(args->input_file, "rb");
    if (!in) {
        perror("fopen input");
        return NULL;
    }

    size_t total_processed = 0;
    struct timespec last_update = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last_update);

    while (keep_running) {
        pthread_mutex_lock(&buf->mutex);
        while (buf->count == MAX_BLOCKS && keep_running) {
            pthread_cond_wait(&buf->not_full, &buf->mutex);
        }
        if (!keep_running) {
            pthread_mutex_unlock(&buf->mutex);
            break;
        }

        int index = buf->head;
        pthread_mutex_unlock(&buf->mutex);

        size_t bytes_read = fread(buf->data[index], 1, BLOCK_SIZE, in);
        if (bytes_read == 0) {
            break;
        }

        caesar(buf->data[index], buf->data[index], bytes_read);

        total_processed += bytes_read;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - last_update.tv_sec) * 1000 +
                  (now.tv_nsec - last_update.tv_nsec) / 1000000;
        if (ms >= 100 || total_processed == args->file_size) {
            int percent = (int)((total_processed * 100) / args->file_size);
            int bar_width = 50;
            int pos = (percent * bar_width) / 100;
            char bar[bar_width + 1];
            memset(bar, '=', pos);
            memset(bar + pos, ' ', bar_width - pos);
            bar[bar_width] = '\0';
            fprintf(stderr, "\r[%s] %d%%", bar, percent);
            fflush(stderr);
            last_update = now;
        }

        pthread_mutex_lock(&buf->mutex);
        buf->sizes[index] = bytes_read;
        buf->head = (buf->head + 1) % MAX_BLOCKS;
        buf->count++;
        pthread_cond_signal(&buf->not_empty);
        pthread_mutex_unlock(&buf->mutex);
    }

    pthread_mutex_lock(&buf->mutex);
    buf->producer_done = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);

    fclose(in);
    return NULL;
}

void* consumer_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    shared_buffer_t *buf = args->buffer;
    FILE *out = fopen(args->output_file, "wb");
    if (!out) {
        perror("fopen output");
        return NULL;
    }

    while (keep_running) {
        pthread_mutex_lock(&buf->mutex);

        while (buf->count == 0 && !buf->producer_done && keep_running) {
            pthread_cond_wait(&buf->not_empty, &buf->mutex);
        }
        if (!keep_running || (buf->count == 0 && buf->producer_done)) {
            pthread_mutex_unlock(&buf->mutex);
            break;
        }

        int index = buf->tail;
        size_t block_size = buf->sizes[index];

        char local_block[BLOCK_SIZE];
        memcpy(local_block, buf->data[index], block_size);
		
        pthread_mutex_unlock(&buf->mutex);

        fwrite(local_block, 1, block_size, out);

        pthread_mutex_lock(&buf->mutex);
        buf->tail = (buf->tail + 1) % MAX_BLOCKS;
        buf->count--;
        pthread_cond_signal(&buf->not_full);
        pthread_mutex_unlock(&buf->mutex);
    }

    fclose(out);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Использование: %s <входной_файл> <выходной_файл> <ключ>\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    const char *output_file = argv[2];
    char key = (char)atoi(argv[3]);

    struct stat st;
    if (stat(input_file, &st) != 0) {
        perror("stat");
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Ошибка: '%s' не является обычным файлом\n", input_file);
        return 1;
    }
    size_t file_size = st.st_size;

    set_key(key);

    shared_buffer_t buffer;
    buffer_init(&buffer);

    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
    size_t processed = 0;

    thread_args_t args = {
        .input_file = input_file,
        .output_file = output_file,
        .buffer = &buffer,
        .file_size = file_size,
        .processed = &processed,
        .progress_mutex = &progress_mutex
    };

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        buffer_destroy(&buffer);
        return 1;
    }

    pthread_t producer, consumer;
    if (pthread_create(&producer, NULL, producer_thread, &args) != 0) {
        perror("pthread_create producer");
        buffer_destroy(&buffer);
        return 1;
    }
    if (pthread_create(&consumer, NULL, consumer_thread, &args) != 0) {
        perror("pthread_create consumer");
        pthread_cancel(producer);
        pthread_join(producer, NULL);
        buffer_destroy(&buffer);
        return 1;
    }

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    fprintf(stderr, "\n");

    if (!keep_running) {
        fprintf(stderr, "Операция прервана пользователем\n");
    }

    buffer_destroy(&buffer);
    pthread_mutex_destroy(&progress_mutex);

    return 0;
}