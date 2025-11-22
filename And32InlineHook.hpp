#ifndef A32_INLINE_HOOK_H
#define A32_INLINE_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void A32HookFunction(void *const symbol, void *const replace, void **result);
void *A32HookFunctionV(void *const symbol, void *const replace, void *const rwx, const uintptr_t rwx_size);

#ifdef __cplusplus
}
#endif

#endif // A32_INLINE_HOOK_H
