#include "And32InlineHook.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <android/log.h>

#define A32_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "A32_HOOK", __VA_ARGS__))
#define A32_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "A32_HOOK", __VA_ARGS__))

static uintptr_t g_PageSize = 0;

#define PAGE_MASK       (~(g_PageSize - 1))
#define PAGE_START(addr) ((addr) & PAGE_MASK)
#define PAGE_END(addr)  (PAGE_START(addr) + g_PageSize)

static void InitPageSize() {
    if (g_PageSize == 0) {
        g_PageSize = sysconf(_SC_PAGESIZE);
    }
}

static bool IsThumbMode(uintptr_t addr) {
    return (addr & 1) != 0;
}

static uintptr_t GetRealAddress(uintptr_t addr) {
    return addr & ~1;
}

static int GetInstructionSize(uintptr_t addr, bool thumb) {
    if (thumb) {
        uint16_t ins = *reinterpret_cast<uint16_t*>(addr);
        if ((ins & 0xF800) == 0xF000 || (ins & 0xFF00) == 0xE800) {
            return 4;
        }
        return 2;
    }
    return 4;
}

static int CalculateHookLength(uintptr_t addr, bool thumb, int min_length) {
    int length = 0;
    int total_bytes = 0;
    
    while (total_bytes < min_length) {
        int ins_size = GetInstructionSize(addr + total_bytes, thumb);
        total_bytes += ins_size;
        length++;
    }
    
    return length;
}

static bool MakeMemoryExecutable(void* addr, size_t size) {
    InitPageSize();
    uintptr_t start = PAGE_START(reinterpret_cast<uintptr_t>(addr));
    uintptr_t end = PAGE_END(reinterpret_cast<uintptr_t>(addr) + size - 1);
    
    return mprotect(reinterpret_cast<void*>(start), end - start, 
                   PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void* A32HookFunctionV(void* const symbol, void* const replace, void* const rwx, const uintptr_t rwx_size) {
    if (!symbol || !replace) {
        A32_LOGE("Invalid parameters: symbol=%p, replace=%p", symbol, replace);
        return nullptr;
    }
    
    uintptr_t target_addr = reinterpret_cast<uintptr_t>(symbol);
    uintptr_t hook_addr = reinterpret_cast<uintptr_t>(replace);
    bool thumb_mode = IsThumbMode(target_addr);
    uintptr_t real_target = GetRealAddress(target_addr);
    
    int min_bytes = thumb_mode ? 6 : 8;
    int instruction_count = CalculateHookLength(real_target, thumb_mode, min_bytes);
    int total_bytes = 0;
    
    uintptr_t current_addr = real_target;
    for (int i = 0; i < instruction_count; i++) {
        total_bytes += GetInstructionSize(current_addr, thumb_mode);
        current_addr += GetInstructionSize(current_addr, thumb_mode);
    }
    
    uint32_t* trampoline = static_cast<uint32_t*>(rwx);
    if (trampoline && rwx_size >= total_bytes + 20) {
        if (thumb_mode) {
            uint16_t* thumb_tramp = reinterpret_cast<uint16_t*>(trampoline);
            
            uint16_t* src = reinterpret_cast<uint16_t*>(real_target);
            uint16_t* dst = thumb_tramp;
            
            for (int i = 0; i < total_bytes / 2; i++) {
                dst[i] = src[i];
            }
            
            uintptr_t return_addr = real_target + total_bytes;
            if (return_addr % 2 == 0) {
                return_addr |= 1;
            }
            
            // ldr.w pc, [pc]
            thumb_tramp[total_bytes/2] = 0xF8DF; 
            thumb_tramp[total_bytes/2 + 1] = 0xF000;
            *reinterpret_cast<uintptr_t*>(&thumb_tramp[total_bytes/2 + 2]) = return_addr;
            
        } else {
            uint32_t* arm_tramp = trampoline;
            
            uint32_t* src = reinterpret_cast<uint32_t*>(real_target);
            uint32_t* dst = arm_tramp;
            
            for (int i = 0; i < instruction_count; i++) {
                dst[i] = src[i];
            }
            
            uintptr_t return_addr = real_target + total_bytes;
            arm_tramp[instruction_count] = 0xE51FF004; // ldr pc, [pc, #-4]
            arm_tramp[instruction_count + 1] = return_addr;
        }
        
        __builtin___clear_cache(reinterpret_cast<char*>(trampoline), 
                               reinterpret_cast<char*>(trampoline) + total_bytes + 20);
    }
    
    if (!MakeMemoryExecutable(reinterpret_cast<void*>(real_target), total_bytes + 8)) {
        A32_LOGE("Failed to make memory executable");
        return nullptr;
    }
    
    if (thumb_mode) {
        uint16_t* code = reinterpret_cast<uint16_t*>(real_target);
        
        // Thumb hook: push lr; ldr r12, [pc, #4]; blx r12; pop {pc}; .addr hook_addr
        code[0] = 0xB500;        // push {lr}
        code[1] = 0x9C02;        // ldr r12, [pc, #8]
        code[2] = 0x47E0;        // blx r12
        code[3] = 0xBD00;        // pop {pc}
        code[4] = hook_addr & 0xFFFF;
        code[5] = hook_addr >> 16;
        
    } else {
        uint32_t* code = reinterpret_cast<uint32_t*>(real_target);
        
        code[0] = 0xE51FF004;
        code[1] = hook_addr;
    }
    
    __builtin___clear_cache(reinterpret_cast<char*>(real_target), 
                           reinterpret_cast<char*>(real_target) + total_bytes);
    if (trampoline) {
        uintptr_t result_ptr = reinterpret_cast<uintptr_t>(trampoline);
        if (thumb_mode) {
            result_ptr |= 1;
        }
        return reinterpret_cast<void*>(result_ptr);
    }
    
    return nullptr;
}

void A32HookFunction(void* const symbol, void* const replace, void** result) {
    InitPageSize();
    
    void* trampoline_mem = nullptr;
    if (result != nullptr) {
        trampoline_mem = mmap(nullptr, g_PageSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        
        if (trampoline_mem == MAP_FAILED) {
            A32_LOGE("Failed to allocate trampoline memory");
            *result = nullptr;
            return;
        }
    }

    void* final_trampoline = A32HookFunctionV(symbol, replace, trampoline_mem, g_PageSize);

    if (result != nullptr) {
        if (final_trampoline != nullptr) {
            *result = final_trampoline;
        } else {
            if (trampoline_mem != nullptr && trampoline_mem != MAP_FAILED) {
                munmap(trampoline_mem, g_PageSize);
            }
            *result = nullptr;
        }
    }
}
