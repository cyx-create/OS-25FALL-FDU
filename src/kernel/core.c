#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <common/buf.h>
#include <driver/virtio.h>
#include <driver/memlayout.h>


volatile bool panic_flag;
void set_parent_to_this(Proc *proc);
void trap_return(u64);

NO_RETURN void idle_entry()
{
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap
        {
            arch_wfi();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

NO_RETURN void kernel_entry()
{
    init_filesystem();

    printk("Hello world! (Core %lld)\n", cpuid());
    // proc_test();
    // vm_test();
    // user_proc_test();
    // io_test();

    // /* LAB 4 TODO 3 BEGIN */
    // Buf b;
    // b.block_no = 0;      // MBR 永远在 LBA=0
    // b.flags = 0;          // 读请求

    // // 发起读操作
    // virtio_blk_rw(&b);

    // // MBR 数据在 b.data 中，偏移见实验文档
    // u8 *mbr = b.data;

    // // 第二分区入口位于 0x1CE
    // u8 *p2 = mbr + 0x1CE;

    // // offset 0x8 = 4 bytes = 起始 LBA
    // u32 part2_lba = *(u32 *)(p2 + 0x8);

    // // offset 0xC = 4 bytes = 分区大小（扇区数/块数）
    // u32 part2_size = *(u32 *)(p2 + 0xC);

    // printk("[MBR] Partition 2 start LBA = %u, size = %u sectors\n",
    //        part2_lba, part2_size);

    // /* LAB 4 TODO 3 END */

    /**
     * (Final) TODO BEGIN 
     * 
     * Map init.S to user space and trap_return to run icode.
     */
    // 添加extern声明（引用init.S的符号）
    extern char icode[];   // init代码开始
    extern char eicode[];  // init代码结束

    // 1. 创建用户进程并分配资源
    Proc *p = create_proc();
    // 2. 将 init.S 程序映射到用户进程的地址空间
    u64 start_addr = (u64)icode;
    u64 end_addr = (u64)eicode;

    // 假设 EXTMEM 是用户空间的起始地址（内存布局）
    for (u64 q = start_addr; q < end_addr; q += PAGE_SIZE) {
        // 为用户进程分配一页虚拟内存并映射到物理地址
        *get_pte(&p->pgdir, EXTMEM + q - start_addr, true) = K2P(q) | PTE_USER_DATA;
    }
    
    // 确保页表设置成功
    ASSERT(p->pgdir.pt);  

    // 3. 设置用户进程的初始上下文（用户程序入口）
    p->ucontext->x[0] = 0;  // 假设没有参数传递
    p->ucontext->elr = EXTMEM;  // 用户程序的入口地址（即 init.S 开始的地址）
    p->ucontext->spsr = 0;  // 设置为用户态的 SPSR

    // 4. 设置进程的工作目录（假设根目录是 "/"）
    OpContext ctx;
    bcache.begin_op(&ctx);
    p->cwd = namei("/", &ctx);  // 获取根目录的 inode
    bcache.end_op(&ctx);
    
    // 5. 设置该进程的父进程为当前内核进程（让它成为 init 的父进程）
    set_parent_to_this(p);

    // printk("[KERNEL] About to start init process\n");
    
    // 6. 启动用户进程，传递给定的返回地址（trap_return）
    start_proc(p, trap_return, 0);
    // printk("[KERNEL] Should not reach here\n");
    

    // 7. 若程序执行到这里，说明用户进程创建失败，内核进入空闲状态
    while (1) {
        //  printk("[IDLE] Current pid: %d\n", thisproc()->pid);
        // 这里只是一个空闲等待，防止内核死锁
        yield();
        arch_with_trap {
            arch_wfi();
        }
    }

    /* (Final) TODO END */
}

NO_INLINE NO_RETURN void _panic(const char *file, int line)
{
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}