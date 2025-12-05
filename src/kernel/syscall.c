#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <test/test.h>
#include <aarch64/intrinsic.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

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

#pragma GCC diagnostic pop