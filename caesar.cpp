#include "caesar.h"
#include <cstring>

static char encrypt_key = 0;

void set_key(char key) {
    encrypt_key = key;
}

void caesar(void* src, void* dst, int len) {
    if (src == nullptr || dst == nullptr || len <= 0) return;

    unsigned char* src_bytes = static_cast<unsigned char*>(src);
    unsigned char* dst_bytes = static_cast<unsigned char*>(dst);

    for (int i = 0; i < len; ++i) {
        dst_bytes[i] = src_bytes[i] ^ encrypt_key;
    }
}