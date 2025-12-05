#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

//lab1
#include <common/list.h>
#include<common/string.h>

//lab1
/*统一内存池 + 锁保护 */
RefCount kalloc_page_cnt;
SpinLock mem_lock;
RefCount page_ref[PHYSTOP / PAGE_SIZE];

static QueueNode* page_queue;
void* zeroed_page = 0;
extern char end[];

int total_pages = 0;

typedef struct Block {
    struct Block* next;
    u16 size;
    u16 used;
    bool free;
} Block;

Block* freelist[4];

void kinit() {
    init_rc(&kalloc_page_cnt);
    init_spinlock(&mem_lock);

    // 初始化可用物理页队列
    for (u64 addr = PAGE_BASE((u64)&end) + 2 * PAGE_SIZE; addr < P2K(PHYSTOP); addr += PAGE_SIZE) {
        add_to_queue(&page_queue, (QueueNode*)addr);
        total_pages++;
    }

    zeroed_page = (void*)(PAGE_BASE((u64)&end) + PAGE_SIZE);
    memset(zeroed_page, 0, PAGE_SIZE);
    increment_rc(&page_ref[K2P(zeroed_page) / PAGE_SIZE]);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);
    void* pg = fetch_from_queue(&page_queue);
    increment_rc(&page_ref[K2P(pg) / PAGE_SIZE]);
    return pg;
}

void kfree_page(void* pg) {
    decrement_rc(&page_ref[K2P(pg) / PAGE_SIZE]);
    if (page_ref[K2P(pg) / PAGE_SIZE].count == 0) {
        decrement_rc(&kalloc_page_cnt);
        add_to_queue(&page_queue, (QueueNode*)pg);
    }
}

static void merge_adjacent(Block* head) {
    Block* cur = head->next;
    while (cur && cur->free && PAGE_BASE((u64)head) == PAGE_BASE((u64)cur)) {
        head->next = cur->next;
        head->size += cur->size + sizeof(Block);
        cur = cur->next;
    }
}

void* kalloc(unsigned long long size) {
    acquire_spinlock(&mem_lock);

    // 决定内存对齐
    int align = (size % 8 == 0) ? 8 : 4;
    Block* blk = freelist[cpuid()];

    while (blk) {
        if (!blk->free) {
            u64 new_addr = (u64)blk + sizeof(Block) + blk->used;
            new_addr = (new_addr + align - 1) / align * align;

            if (blk->size >= (new_addr - (u64)blk - sizeof(Block)) + sizeof(Block)) {
                Block* new_blk = (Block*)new_addr;
                new_blk->size = blk->size - (new_addr - (u64)blk - sizeof(Block)) - sizeof(Block);
                new_blk->free = 1;
                new_blk->next = blk->next;
                blk->next = new_blk;
                blk->size = (new_addr - (u64)blk - sizeof(Block));
            }
        } else {
            merge_adjacent(blk);
            u64 aligned = (u64)blk + sizeof(Block);
            aligned = (aligned + align - 1) / align * align;

            if (((u64)blk % align == 0 && blk->size >= size) ||
                ((u64)blk % align != 0 && blk->size >= size + (aligned - (u64)blk - sizeof(Block)) + sizeof(Block))) {

                if ((u64)blk % align != 0) {
                    Block* split_blk = (Block*)aligned;
                    split_blk->size = blk->size - (aligned - (u64)blk - sizeof(Block)) - sizeof(Block);
                    split_blk->free = 1;
                    split_blk->next = blk->next;
                    blk->next = split_blk;
                    blk->size = (aligned - (u64)blk - sizeof(Block));
                    blk = split_blk;
                }
                break;
            }
        }
        blk = blk->next;
    }

    // 若未找到合适块，则新分配一页
    if (!blk) {
        Block* new_page = (Block*)kalloc_page();
        new_page->next = freelist[cpuid()];
        new_page->size = PAGE_SIZE - sizeof(Block);
        new_page->free = 1;
        freelist[cpuid()] = new_page;
        blk = new_page;
    }

    blk->used = size;
    blk->free = 0;

    release_spinlock(&mem_lock);
    return (void*)((u64)blk + sizeof(Block));
}

void kfree(void* ptr) {
    acquire_spinlock(&mem_lock);
    Block* blk = (Block*)((u64)ptr - sizeof(Block));
    merge_adjacent(blk);
    blk->free = 1;
    release_spinlock(&mem_lock);
}

