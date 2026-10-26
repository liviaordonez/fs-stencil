#include "string.h"

// string stuff
void strncpy(char *dest, const char *src, long len) {
    for (int i = 0; i < len; i++) {
        dest[i] = src[i];
    }
}

int strlen(const char *str) {
    int len;
    for (len = 0; str[len] != '\0' && len < MAX_STRING_LEN; len++) {  };
    return len;
}

void *memcpy(void *dest, const void *src, long n) {
    for (int i = 0; i < n; i++) {
        ((char *) dest)[i] = ((const char *) src)[i];
    }

    return dest;
}
