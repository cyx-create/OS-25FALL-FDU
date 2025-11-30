#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <common/buf.h>
#include <driver/virtio.h>


volatile bool panic_flag;

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
    printk("Hello world! (Core %lld)\n", cpuid());
    proc_test();
    // vm_test();
    user_proc_test();
    io_test();

    /* LAB 4 TODO 3 BEGIN */
    Buf b;
    b.block_no = 0;      // MBR 永远在 LBA=0
    b.flags = 0;          // 读请求

    // 发起读操作
    virtio_blk_rw(&b);

    // MBR 数据在 b.data 中，偏移见实验文档
    u8 *mbr = b.data;

    // 第二分区入口位于 0x1CE
    u8 *p2 = mbr + 0x1CE;

    // offset 0x8 = 4 bytes = 起始 LBA
    u32 part2_lba = *(u32 *)(p2 + 0x8);

    // offset 0xC = 4 bytes = 分区大小（扇区数/块数）
    u32 part2_size = *(u32 *)(p2 + 0xC);

    printk("[MBR] Partition 2 start LBA = %u, size = %u sectors\n",
           part2_lba, part2_size);

    /* LAB 4 TODO 3 END */

    while (1)
        yield();
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