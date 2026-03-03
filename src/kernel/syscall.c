#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <test/test.h>
#include <aarch64/intrinsic.h>
#include <kernel/paging.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

void init_syscall()
{
    for (u64 *p = (u64 *)&early_init; p < (u64 *)&rest_init; p++)
        ((void (*)()) * p)();
}

void *syscall_table[NR_SYSCALL] = {
    [0 ... NR_SYSCALL - 1] = NULL,
    [SYS_myreport] = (void *)syscall_myreport,
};

void syscall_entry(UserContext *context)
{
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    // be sure to check the range of id. if id >= NR_SYSCALL, panic.

    //lab3尝试1
    // printk("[syscall] id=%llu, ptr=%p\n", context->x[8], syscall_table[context->x[8]]);

    u64 id = context->x[8];  // 取系统调用号 x8
    u64 ret = 0;

    if (id >= NR_SYSCALL) {
        PANIC();
    }
    else if(syscall_table[id] != NULL){
    // 取参数 x0~x5
        u64 a0 = context->x[0];
        u64 a1 = context->x[1];
        u64 a2 = context->x[2];
        u64 a3 = context->x[3];
        u64 a4 = context->x[4];
        u64 a5 = context->x[5];

        // 调用 syscalls[id] 函数
        ret = ((u64(*)(u64, u64, u64, u64, u64, u64))syscall_table[id])(a0, a1, a2, a3, a4, a5);

        // 返回值写回 x0
        context->x[0] = ret;
    }

}

/** 
 * Check if the virtual address [start,start+size) is READABLE by the current
 * user process.
 */
bool user_readable(const void *start, usize size) {
    /* (Final) TODO BEGIN */
    if (size == 0) {
        return true;  // Empty range is trivially readable
    }
    
    Proc *current_proc = thisproc();
    u64 start_addr = (u64)start;
    u64 end_addr = start_addr + size;
    
    // Check each memory block in the range
    for (u64 addr = start_addr; addr < end_addr; addr = ((addr / BLOCK_SIZE) + 1) * BLOCK_SIZE) {
        PTEntry *pte = get_pte(&current_proc->pgdir, addr, false);
        
        // Check if page table entry exists and has user permission
        if (pte == NULL || (*pte & PTE_USER) == 0) {
            return false;
        }
    }
    
    return true;
    /* (Final) TODO END */
}


/**
 * Check if the virtual address [start,start+size) is READABLE & WRITEABLE by
 * the current user process.
 */
bool user_writeable(const void *start, usize size) {
    /* (Final) TODO Begin */
    if (size == 0) {
        return true;  // Empty range is trivially writeable
    }
    
    Proc *current_proc = thisproc();
    u64 start_addr = (u64)start;
    u64 end_addr = start_addr + size;
    
    // Check each memory block in the range
    for (u64 addr = start_addr; addr < end_addr; addr = ((addr / BLOCK_SIZE) + 1) * BLOCK_SIZE) {
        PTEntry *pte = get_pte(&current_proc->pgdir, addr, false);
        
        // Check if page table entry exists, has user permission, and is not read-only
        if (pte == NULL || (*pte & PTE_RO) || (*pte & PTE_USER) == 0) {
            return false;
        }
    }
    
    return true;
    /* (Final) TODO End */
}

/** 
 * Get the length of a string including tailing '\0' in the memory space of
 * current user process return 0 if the length exceeds maxlen or the string is
 * not readable by the current user process.
 */
usize user_strlen(const char *str, usize maxlen) {
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}