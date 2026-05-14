#include "caesar.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

static void* protected_key = nullptr;
static size_t protected_key_size = 0;

static pthread_mutex_t caesar_mutex = PTHREAD_MUTEX_INITIALIZER;

static void segv_handler(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)context;

    fprintf(stderr, "[SECURITY] Segmentation fault while accessing protected memory at %p\n", info->si_addr);

    _exit(EXIT_FAILURE);
}

static void setup_sigsegv_handler() {
    static bool initialized = false;

    if (initialized) return;

    initialized = true;

    struct sigaction sa{};
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;

    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
        perror("sigaction EGV");
        exit(EXIT_FAILURE);
    }
}

static void destroy_key() {
    if (protected_key == nullptr) return;

    if (mprotect(protected_key,
                 protected_key_size,
                 PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect destroy");
        return;
    }

    memset(protected_key, 0, protected_key_size);

    if (mprotect(protected_key,
                 protected_key_size,
                 PROT_NONE) != 0) {
        perror("mprotect PROT_NONE");
    }

    if (munmap(protected_key, protected_key_size) != 0) {
        perror("munmap");
    }

    protected_key = nullptr;
    protected_key_size = 0;
}

void set_key(char key) {
	pthread_mutex_lock(&caesar_mutex);
	
	setup_sigsegv_handler();
	destroy_key();
	
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "Failed to get page size\n");
		pthread_mutex_unlock(&caesar_mutex);
        exit(EXIT_FAILURE);
    }
	
    protected_key_size = static_cast<size_t>(page_size);

    protected_key = mmap(
        nullptr,
        protected_key_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (protected_key == MAP_FAILED) {
        perror("mmap");
		pthread_mutex_unlock(&caesar_mutex);
        exit(EXIT_FAILURE);
    }

    memcpy(protected_key, &key, sizeof(char));

    if (mprotect(protected_key,
                 protected_key_size,
                 PROT_NONE) != 0) {
        perror("mprotect PROT_NONE");
        destroy_key();
        exit(EXIT_FAILURE);
    }

    atexit(destroy_key);
	pthread_mutex_unlock(&caesar_mutex);
}

void caesar(void* src, void* dst, int len) {
    if (src == nullptr || dst == nullptr || len <= 0) return;
	
	pthread_mutex_lock(&caesar_mutex);
	
    if (protected_key == nullptr) {
        fprintf(stderr, "Key is not initialized\n");
		pthread_mutex_unlock(&caesar_mutex);
        return;
    }
	if (mprotect(protected_key, protected_key_size, PROT_READ) != 0) {
        perror("mprotect PROT_READ");
		pthread_mutex_unlock(&caesar_mutex);
        return;
    }

    unsigned char* src_bytes = static_cast<unsigned char*>(src);
    unsigned char* dst_bytes = static_cast<unsigned char*>(dst);

    for (int i = 0; i < len; ++i) {
        dst_bytes[i] = src_bytes[i] ^ *static_cast<unsigned char*>(protected_key);
    }
	if (mprotect(protected_key, protected_key_size, PROT_NONE) != 0) {
        perror("mprotect PROT_NONE");
    }
	pthread_mutex_unlock(&caesar_mutex);
}

void testAccess() {
	char* ptr = (char*) protected_key;
	*ptr = 123;
}