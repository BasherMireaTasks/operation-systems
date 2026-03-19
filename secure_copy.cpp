#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include "caesar.h"

#define BLOCK_SIZE 2048
#define MAX_WORKERS 3
#define MAX_FILES 100
#define MUTEX_TIMEOUT_SEC 5

volatile sig_atomic_t keep_running = 1;
pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int completed_files = 0;

typedef struct {
	char files[MAX_FILES][PATH_MAX];
	int total;
	int index;
	pthread_mutex_t mutex;
} file_queue_t;

typedef struct {
    file_queue_t *queue;
    const char *output_dir;
    char key;
} thread_args_t;

file_queue_t file_queue;

void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

void queue_init(file_queue_t *queue) {
    queue->index = 0;
    queue->total = 0;
    pthread_mutex_init(&queue->mutex, NULL);
}

void queue_destroy(file_queue_t *queue) {
    pthread_mutex_destroy(&queue->mutex);
}

int secure_mutex_timedlock(pthread_mutex_t *mutex, const char *thread_id) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += MUTEX_TIMEOUT_SEC;
    
    int ret = pthread_mutex_timedlock(mutex, &ts);
    if (ret == ETIMEDOUT) {
        fprintf(stderr, "Возможная взаимоблокировка: поток %s ожидает мьютекс более %d секунд\n", 
                thread_id, MUTEX_TIMEOUT_SEC);
        return -1;
    } else if (ret != 0) {
        return -1;
    }
    return 0;
}

int queue_get_file(file_queue_t *queue, char *filename, size_t max_len, const char *thread_id) {
    if (secure_mutex_timedlock(&queue->mutex, thread_id) != 0) {
        return -1;
    }
    
    if (queue->index >= queue->total || !keep_running) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    
    strncpy(filename, queue->files[queue->index], max_len - 1);
    filename[max_len - 1] = '\0';
    queue->index++;
    
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void log_operation(const char *thread_id, const char *filename, const char *result, double elapsed_time) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    if (secure_mutex_timedlock(&log_mutex, thread_id) != 0) {
        return;
    }
    
    FILE *log = fopen("log.txt", "a");
    if (log) {
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&ts.tv_sec));
        fprintf(log, "[%s] Поток %s | Файл: %s | Результат: %s | Время: %.2f сек\n",
                time_str, thread_id, filename, result, elapsed_time);
        fclose(log);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

int create_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755);
}

void* worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    char thread_id[32];
    snprintf(thread_id, sizeof(thread_id), "%ld", (long)pthread_self());
    
    char filename[PATH_MAX];
    
    while (keep_running) {
        int result = queue_get_file(&file_queue, filename, sizeof(filename), thread_id);
        if (result <= 0) break;
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        char input_path[PATH_MAX];
        char output_path[PATH_MAX + 64];
		
        snprintf(input_path, sizeof(input_path), "%s", filename);
        
        const char *base_name = strrchr(filename, '/');
        base_name = base_name ? base_name + 1 : filename;
        
		int ret = snprintf(output_path, sizeof(output_path), "%s/%s",
							args -> output_dir, base_name);
		if (ret < 0 || (size_t)ret >= sizeof(output_path)) {
			fprintf(stderr, "Ошибка: путь слишком длинный для '%s'\n", filename);
			continue;
		}
        
        FILE *in = fopen(input_path, "rb");
        if (!in) {
            clock_gettime(CLOCK_MONOTONIC, &end);
            double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
            log_operation(thread_id, filename, "Ошибка открытия входного файла", elapsed);
            continue;
        }
        
        FILE *out = fopen(output_path, "wb");
        if (!out) {
            fclose(in);
            clock_gettime(CLOCK_MONOTONIC, &end);
            double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
            log_operation(thread_id, filename, "Ошибка открытия выходного файла", elapsed);
            continue;
        }
        
        char buffer[BLOCK_SIZE];
        size_t bytes_read;
        int encrypt_error = 0;
        
        while ((bytes_read = fread(buffer, 1, BLOCK_SIZE, in)) > 0) {
            caesar(buffer, buffer, bytes_read);
            if (fwrite(buffer, 1, bytes_read, out) != bytes_read) {
                encrypt_error = 1;
                break;
            }
        }
        
        fclose(in);
        fclose(out);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        
        if (encrypt_error) {
            log_operation(thread_id, filename, "Ошибка записи", elapsed);
        } else {
            log_operation(thread_id, filename, "Успех", elapsed);
            
            if (secure_mutex_timedlock(&counter_mutex, thread_id) == 0) {
                completed_files++;
                pthread_mutex_unlock(&counter_mutex);
            }
        }
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Использование: %s <файл1> [файл2] ... <выходная_директория> <ключ>", argv[0]);
        return 1;
    }
    
    const char *output_dir = argv[argc - 2];
    char key = (char)atoi(argv[argc - 1]);
    int num_files = argc - 3;
    
    if (num_files <= 0) {
        fprintf(stderr, "Ошибка: необходимо указать хотя бы один входной файл\n");
        return 1;
    }
    
    if (num_files > MAX_FILES) {
        fprintf(stderr, "Ошибка: слишком много файлов (максимум %d)\n", MAX_FILES);
        return 1;
    }
    
    set_key(key);
    
    if (create_directory(output_dir) != 0) {
        perror("mkdir");
        return 1;
    }
    
    queue_init(&file_queue);
    file_queue.total = num_files;
    for (int i = 1; i <= num_files; i++) {
        strncpy(file_queue.files[i-1], argv[i], PATH_MAX - 1);
        file_queue.files[i-1][PATH_MAX - 1] = '\0';
    }
    
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        queue_destroy(&file_queue);
        return 1;
    }
    
    pthread_t workers[MAX_WORKERS];
    thread_args_t args = {
        .queue = &file_queue,
        .output_dir = output_dir,
        .key = key
    };
    
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, &args) != 0) {
            perror("pthread_create");
            keep_running = 0;
            for (int j = 0; j < i; j++) {
                pthread_join(workers[j], NULL);
            }
            queue_destroy(&file_queue);
            return 1;
        }
    }
    
    for (int i = 0; i < MAX_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
    
    fprintf(stderr, "\nОбработано файлов: %d\n", completed_files);
    
    queue_destroy(&file_queue);
    pthread_mutex_destroy(&counter_mutex);
    pthread_mutex_destroy(&log_mutex);
    
    return 0;
}