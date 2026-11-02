#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
static char *last;
const char *mock_last_input(void) { return last; }
typedef void (*Callback)(int, long long, const char *);
bool mock_load(void *, const char *, Callback) __asm__("_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E");
bool mock_load(void *self, const char *json, Callback callback) {
    assert(self==(void *)0x1234 && callback==NULL);
    free(last); last=strdup(json); return true;
}
__attribute__((destructor)) static void cleanup(void) { free(last); }
