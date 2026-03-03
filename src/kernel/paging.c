#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

// 定义映射类型
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02

// 遍历链表的宏
#define for_each_section(node, head) \
    for (ListNode *node = (head)->next; node != (head); node = node->next)

// 忽略kill返回值的辅助函数
static void kill_process(int pid) {
    int result = kill(pid);
    (void)result; // 显式忽略返回值
}

void init_sections(ListNode *section_head) {
    /* (Final) TODO BEGIN */
    // 创建一个初始的堆 section
    struct section *sec = kalloc(sizeof(struct section));
    
    // 设置堆 section 的标志
    sec->flags = ST_HEAP;
    sec->begin = 0;
    sec->end = 0;
    
    // 插入到链表
    _insert_into_list(section_head, &sec->stnode);
    
    // 文件指针为空
    sec->fp = NULL;
    /* (Final) TODO END */
}

void free_sections(struct pgdir *pd) {
    /* (Final) TODO BEGIN */
    // 遍历所有 section
    for_each_section(p, &pd->section_head) {
        struct section *sec = container_of(p, struct section, stnode);
        
        // 释放该 section 中的所有页面
        for (u64 i = PAGE_BASE(sec->begin); i < sec->end; i += PAGE_SIZE) {
            PTEntry *pte = get_pte(pd, i, false);
            if (pte && (*pte & PTE_VALID)) {
                // 释放物理页面
                kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
            }
        }
        
        // 关闭关联的文件
        if (sec->fp) {
            file_close(sec->fp);
        }
        
        // 如果是文件映射的 section，释放 section 结构体
        if (sec->flags & ST_FILE) {
            kfree(sec);
        }
    }
    /* (Final) TODO END */
}

u64 sbrk(i64 size) {
    /**
     * (Final) TODO BEGIN 
     * 
     * Increase the heap size of current process by `size`.
     * If `size` is negative, decrease heap size. `size` must
     * be a multiple of PAGE_SIZE.
     * 
     * Return the previous heap_end.
     */
    
    // 确保 size 是页面大小的倍数
    ASSERT(size % PAGE_SIZE == 0);
    
    // 获取当前进程和页目录
    Proc *proc = thisproc();
    struct pgdir *pd = &proc->pgdir;
    
    // 获取堆 section（第一个 section）
    struct section *sec = container_of(pd->section_head.next, struct section, stnode);
    
    // 保存旧的结束地址
    u64 old_end = sec->end;
    
    // 增加堆大小（惰性分配）
    sec->end += size;
    
    // 如果缩小堆，需要释放页面
    if (size < 0) {
        for (i64 i = 0; i < -size; i += PAGE_SIZE) {
            PTEntry *pte = get_pte(pd, sec->end + i, false);
            if (pte && *pte) {
                // 释放物理页面
                kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
                // 清除页表项
                *pte = 0;
            }
        }
    }
    
    // 刷新 TLB
    arch_tlbi_vmalle1is();
    
    return old_end;
    /* (Final) TODO END */
}

int pgfault_handler(u64 iss) {
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr = arch_get_far();

    /** 
     * (Final) TODO BEGIN
     * 
     * 1. Find the section struct which contains the faulting address `addr`.
     * 2. Check section flags to determine page fault type.
     * 3. Handle the page fault accordingly.
     * 4. Return to user code or kill the process.
     */
    
    // 检查地址是否有效
    if ((addr & KSPACE_MASK) || addr < 4 * PAGE_SIZE) {
        // 非法地址访问
        kill_process(p->pid);
        return iss;
    }
    
    // 查找包含该地址的 section
    struct section *sec = NULL;
    for_each_section(node, &pd->section_head) {
        sec = container_of(node, struct section, stnode);
        if (sec->begin <= addr && addr < sec->end) {
            break;
        }
    }
    
    // 如果找不到 section，终止进程
    if (!sec || !(sec->begin <= addr && addr < sec->end)) {
        kill_process(p->pid);
        return iss;
    }
    
    // 处理文件映射的页面错误
    if (sec->mmap_flags) {
        // 分配新页面
        void *new_page = kalloc_page();
        if (new_page == NULL) {
            kill_process(p->pid);
            return iss;
        }
        
        // 建立映射
        vmmap(pd, addr, new_page, PTE_USER_DATA);
        
        // 刷新 TLB
        arch_tlbi_vmalle1is();
        
        // 从文件读取数据
        struct file *f = sec->fp;
        if (f == NULL || !f->readable || f->type != FD_INODE) {
            kill_process(p->pid);
            return iss;
        }
        
        // 计算文件偏移
        u64 offset = sec->offset + (addr - sec->begin);
        
        // 锁定 inode 并读取数据
        inodes.lock(f->ip);
        int n = inodes.read(f->ip, new_page, offset, PAGE_SIZE);
        inodes.unlock(f->ip);
        
        // 如果读取的字节数不足一页，用0填充剩余部分
        if (n < PAGE_SIZE) {
            memset(new_page + n, 0, PAGE_SIZE - n);
        }
        
        return iss;
    }
    
    // 处理堆或栈的页面错误
    PTEntry *pte = get_pte(pd, addr, true);
    if (pte == NULL) {
        kill_process(p->pid);
        return iss;
    }
    
    if (*pte == 0) {
        // Lazy allocation：分配新页面
        // printk("Lazy allocation\n");
        void *new_page = kalloc_page();
        if (new_page == NULL) {
            kill_process(p->pid);
            return iss;
        }
        vmmap(pd, addr, new_page, PTE_USER_DATA);
    } else if (*pte & PTE_RO) {
        // Copy on Write：分配新页面并复制内容
        // printk(" Copy on Write\n");
        void *new_page = kalloc_page();
        if (new_page == NULL) {
            kill_process(p->pid);
            return iss;
        }
        memcpy(new_page, (void *)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
        vmmap(pd, addr, new_page, PTE_USER_DATA);
    }
    
    // 刷新 TLB 并返回
    arch_tlbi_vmalle1is();
    return iss;
    /* (Final) TODO END */
}

void copy_sections(ListNode *from_head, ListNode *to_head)
{
    /* (Final) TODO BEGIN */
    // 遍历源进程的所有 sections
    for_each_section(p, from_head) {
        struct section *from_sec = container_of(p, struct section, stnode);
        
        // 分配新的 section 结构体
        struct section *to_sec = kalloc(sizeof(struct section));
        if (to_sec == NULL) {
            continue;
        }
        
        // 复制 section 内容
        memcpy(to_sec, from_sec, sizeof(struct section));
        
        // 插入到目标链表
        _insert_into_list(to_head, &to_sec->stnode);
        
        // 复制文件指针（如果存在）
        if (from_sec->fp) {
            to_sec->fp = file_dup(from_sec->fp);
        }
        
        // 获取对应的页目录
        struct pgdir *from_pd = container_of(from_head, struct pgdir, section_head);
        struct pgdir *to_pd = container_of(to_head, struct pgdir, section_head);
        
        // 复制页面映射
        for (u64 va = PAGE_BASE(from_sec->begin); va < from_sec->end; va += PAGE_SIZE) {
            PTEntry *pte_from = get_pte(from_pd, va, false);
            if (!pte_from || !(*pte_from & PTE_VALID)) {
                continue;
            }
            
            // 如果是共享映射，直接共享物理页
            if (from_sec->mmap_flags & MAP_SHARED) {
                PTEntry *pte_to = get_pte(to_pd, va, true);
                if (pte_to) {
                    *pte_to = *pte_from;  // 共享同一个物理页
                }
            } else {
                // 私有映射：分配新页并复制内容
                void *new_page = kalloc_page();
                if (new_page == NULL) {
                    continue;
                }
                memcpy(new_page, (void *)P2K(PTE_ADDRESS(*pte_from)), PAGE_SIZE);
                PTEntry *pte_to = get_pte(to_pd, va, true);
                if (pte_to) {
                    *pte_to = K2P(new_page) | (PTE_FLAGS(*pte_from) & ~PTE_RO);
                }
            }
        }
    }
    /* (Final) TODO END */
}