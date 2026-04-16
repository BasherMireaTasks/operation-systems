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
#define MAX_WORKERS 4
#define MAX_FILES 100
#define MUTEX_TIMEOUT_SEC 5

volatile sig_atomic_t keep_running = 1;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char files[MAX_FILES][PATH_MAX];
    int total;
    int index;
    pthread_mutex_t mutex;
} file_queue_t;

typedef struct {
    pthread_mutex_t mutex;
    double sum_individual_times;
    int processed_count;
} stats_collector_t;

typedef struct {
    file_queue_t *queue;
    const char *output_dir;
    char key;
    stats_collector_t *collector;
} thread_args_t;

file_queue_t file_queue;

typedef struct {
    double elapsed;
    int success;
} process_result_t;

typedef struct {
    double total_time;
    double avg_file_time;
    int processed_files;
} perf_stats_t;

enum Mode {
    SEQUENTIAL,
    PARALLEL
};

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
   
    if (secure_mutex_timedlock(&log_mutex, thread_id) != 0) return;
   
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


process_result_t process_single_file(const char *filename, const char *output_dir, const char *thread_id) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    char output_path[PATH_MAX + 64];
    const char *base_name = strrchr(filename, '/');
    base_name = base_name ? base_name + 1 : filename;

    int ret = snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, base_name);
    if (ret < 0 || (size_t)ret >= sizeof(output_path)) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        if (thread_id) log_operation(thread_id, filename, "Ошибка: путь слишком длинный", elapsed);
        return (process_result_t){elapsed, 0};
    }

    FILE *in = fopen(filename, "rb");
    if (!in) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        if (thread_id) log_operation(thread_id, filename, "Ошибка открытия входного файла", elapsed);
        return (process_result_t){elapsed, 0};
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fclose(in);
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        if (thread_id) log_operation(thread_id, filename, "Ошибка открытия выходного файла", elapsed);
        return (process_result_t){elapsed, 0};
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

    const char *result_msg = encrypt_error ? "Ошибка записи" : "Успех";
    if (thread_id) log_operation(thread_id, filename, result_msg, elapsed);

    return (process_result_t){elapsed, !encrypt_error};
}

void* worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    char thread_id[32];
    snprintf(thread_id, sizeof(thread_id), "%ld", (long)pthread_self());

    char filename[PATH_MAX];

    while (keep_running) {
        int result = queue_get_file(&file_queue, filename, sizeof(filename), thread_id);
        if (result <= 0) break;

        process_result_t res = process_single_file(filename, args->output_dir, thread_id);

        if (res.success && args->collector != NULL) {
            if (secure_mutex_timedlock(&args->collector->mutex, thread_id) == 0) {
                args->collector->sum_individual_times += res.elapsed;
                args->collector->processed_count++;
                pthread_mutex_unlock(&args->collector->mutex);
            }
        }
    }
    return NULL;
}

perf_stats_t execute_processing(Mode mode, char **input_files, int num_files, const char *output_dir) {
    if (mode == SEQUENTIAL) {
        struct timespec start_all, end_all;
        clock_gettime(CLOCK_MONOTONIC, &start_all);

        double sum_individual = 0.0;
        int processed = 0;

        for (int i = 0; i < num_files; i++) {
            if (!keep_running) break;
            process_result_t res = process_single_file(input_files[i], output_dir, "SEQ");
            if (res.success) {
                sum_individual += res.elapsed;
                processed++;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &end_all);
        double total_wall = (end_all.tv_sec - start_all.tv_sec) + (end_all.tv_nsec - start_all.tv_nsec) / 1e9;
        double avg = (processed > 0) ? sum_individual / processed : 0.0;

        return (perf_stats_t){total_wall, avg, processed};
    } else {
        queue_init(&file_queue);
        file_queue.total = num_files;
        for (int i = 0; i < num_files; i++) {
            strncpy(file_queue.files[i], input_files[i], PATH_MAX - 1);
            file_queue.files[i][PATH_MAX - 1] = '\0';
        }

        stats_collector_t collector;
        pthread_mutex_init(&collector.mutex, NULL);
        collector.sum_individual_times = 0.0;
        collector.processed_count = 0;

        thread_args_t args = {
            .queue = &file_queue,
            .output_dir = output_dir,
            .key = 0,
            .collector = &collector
        };

        struct timespec start_all, end_all;
        clock_gettime(CLOCK_MONOTONIC, &start_all);

        pthread_t workers[MAX_WORKERS];
        int created = 0;
        for (int i = 0; i < MAX_WORKERS; i++) {
            if (pthread_create(&workers[i], NULL, worker_thread, &args) != 0) {
                perror("pthread_create");
                keep_running = 0;
                break;
            }
            created++;
        }

        for (int i = 0; i < created; i++) {
            pthread_join(workers[i], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &end_all);
        double total_wall = (end_all.tv_sec - start_all.tv_sec) + (end_all.tv_nsec - start_all.tv_nsec) / 1e9;

        double avg = (collector.processed_count > 0) ? collector.sum_individual_times / collector.processed_count : 0.0;
        int processed = collector.processed_count;

        queue_destroy(&file_queue);
        pthread_mutex_destroy(&collector.mutex);

        return (perf_stats_t){total_wall, avg, processed};
    }
}

void print_stats(const char *mode_name, const perf_stats_t *stats) {
    printf("\n=== Статистика для режима %s ===\n", mode_name);
    printf("Общее время выполнения: %.3f секунд\n", stats->total_time);
    printf("Среднее время обработки одного файла: %.3f секунд\n", stats->avg_file_time);
    printf("Количество обработанных файлов: %d\n", stats->processed_files);
}

void print_comparison(perf_stats_t seq_stats, perf_stats_t par_stats, Mode chosen_mode) {
    printf("\n=== Сравнительная таблица (автоматический режим) ===\n");
    printf("%-12s | %-18s | %-25s | %s\n", "Режим", "Общее время (сек)", "Среднее на файл (сек)", "Файлов");
    printf("-------------|---------------------|---------------------------|--------\n");
    printf("%-12s | %-18.3f | %-25.3f | %d\n", "Sequential", seq_stats.total_time, seq_stats.avg_file_time, seq_stats.processed_files);
    printf("%-12s | %-18.3f | %-25.3f | %d\n", "Parallel", par_stats.total_time, par_stats.avg_file_time, par_stats.processed_files);

    const char *chosen_name = (chosen_mode == SEQUENTIAL) ? "Sequential" : "Parallel";
    double chosen_t = (chosen_mode == SEQUENTIAL) ? seq_stats.total_time : par_stats.total_time;
    double alt_t = (chosen_mode == SEQUENTIAL) ? par_stats.total_time : seq_stats.total_time;

    printf("\nВыбран режим: %s (время %.3f сек)\n", chosen_name, chosen_t);
    printf("Альтернативный режим занял бы: %.3f сек\n", alt_t);
    printf("Разница: %.3f сек в пользу выбранного режима\n", alt_t - chosen_t);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Использование: %s [--mode=sequential|parallel] <файл1> [файл2 ...] <выходная_директория> <ключ>\n", argv[0]);
        return 1;
    }

    int file_start = 1;
    enum Mode selected_mode;
    bool is_auto = true;

    if (argc > 1 && strncmp(argv[1], "--mode=", 7) == 0) {
        char *mval = argv[1] + 7;
        if (strcmp(mval, "sequential") == 0) {
            selected_mode = SEQUENTIAL;
        } else if (strcmp(mval, "parallel") == 0) {
            selected_mode = PARALLEL;
        } else {
            fprintf(stderr, "Ошибка: неизвестный режим '%s'. Допустимы: sequential или parallel.\n", mval);
            return 1;
        }
        is_auto = false;
        file_start = 2;
    } else {
        is_auto = true;
    }

    if (file_start + 2 > argc) {
        fprintf(stderr, "Ошибка: недостаточно аргументов. Укажите файлы, выходную директорию и ключ.\n");
        return 1;
    }

    int num_files = argc - file_start - 2;
    if (num_files <= 0) {
        fprintf(stderr, "Ошибка: необходимо указать хотя бы один входной файл\n");
        return 1;
    }
    if (num_files > MAX_FILES) {
        fprintf(stderr, "Ошибка: слишком много файлов (максимум %d)\n", MAX_FILES);
        return 1;
    }

    char **input_files = &argv[file_start];
    const char *output_dir = argv[file_start + num_files];
    char key = (char)atoi(argv[file_start + num_files + 1]);

    set_key(key);

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    if (create_directory(output_dir) != 0) {
        perror("Не удалось создать выходную директорию");
        return 1;
    }

    if (is_auto) {
        selected_mode = (num_files < 5) ? SEQUENTIAL : PARALLEL;
    }

    perf_stats_t chosen_stats;

    if (is_auto) {
        Mode alt_mode = (selected_mode == SEQUENTIAL) ? PARALLEL : SEQUENTIAL;
        char alt_dir[PATH_MAX];
        snprintf(alt_dir, sizeof(alt_dir), "%s_alt_comparison", output_dir);

        if (create_directory(alt_dir) == 0) {
            perf_stats_t alt_stats = execute_processing(alt_mode, input_files, num_files, alt_dir);
            chosen_stats = execute_processing(selected_mode, input_files, num_files, output_dir);

            perf_stats_t seq_stats, par_stats;
            if (selected_mode == SEQUENTIAL) {
                seq_stats = chosen_stats;
                par_stats = alt_stats;
            } else {
                seq_stats = alt_stats;
                par_stats = chosen_stats;
            }
            print_comparison(seq_stats, par_stats, selected_mode);
        } else {
            fprintf(stderr, "Предупреждение: не удалось создать директорию для сравнения, сравнение пропущено.\n");
            chosen_stats = execute_processing(selected_mode, input_files, num_files, output_dir);
        }
    } else {
        chosen_stats = execute_processing(selected_mode, input_files, num_files, output_dir);
    }

    const char *mode_name = (selected_mode == SEQUENTIAL) ? "sequential" : "parallel";
    print_stats(mode_name, &chosen_stats);

    pthread_mutex_destroy(&log_mutex);
    return 0;
}