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

// 空闲页面链表
static ListNode free_pages_list;
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

// final lab - 新增的宏和变量
#define UPALIGN(x) (((u64)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
static char *next_free_addr;  // 下一个可分配地址
u64 memory_start;             // 内存开始地址

// 每个CPU的分配锁
static SpinLock kalloc_lock[4] = {0};

// 计算页面索引（基于memory_start）
u64 get_page_index(void *p) {
    return ((u64)p - memory_start) / PAGE_SIZE;
}

void kinit() {
    init_rc(&kalloc_page_cnt);
    init_spinlock(&mem_lock);
    init_list_node(&free_pages_list);

    // 对齐内核结束地址
    u64 aligned_end = UPALIGN((u64)&end);
    
    // 设置内存开始地址（跳过refcnt数组，因为我们使用page_ref数组）
    memory_start = aligned_end;
    next_free_addr = (char *)memory_start;
    
    // 计算可用页面数
    total_pages = (P2K(PHYSTOP) - memory_start) / PAGE_SIZE;
    
    // printk("mem: end=%p, memory_start=%p, total_pages=%d\n", 
    //        &end, (void*)memory_start, total_pages);

    // 初始化可用物理页链表
    // 跳过前两个页面：一个给zeroed_page，一个作为边界
    for (u64 addr = memory_start + 2 * PAGE_SIZE; addr < P2K(PHYSTOP); addr += PAGE_SIZE) {
        _insert_into_list(&free_pages_list, (ListNode*)addr);
    }

    // 分配zeroed_page
    zeroed_page = (void*)(memory_start + PAGE_SIZE);
    memset(zeroed_page, 0, PAGE_SIZE);
    
    // 设置zeroed_page的引用计数为1
    u64 zero_page_index = get_page_index(zeroed_page);
    increment_rc(&page_ref[zero_page_index]);
    
    // 初始化每个CPU的空闲列表和锁
    for (int i = 0; i < 4; i++) {
        freelist[i] = NULL;
        init_spinlock(&kalloc_lock[i]);
    }
}

u64 left_page_cnt(void) {
    // 剩余页面数 = 总页面数 - 已分配页面数
    return total_pages - kalloc_page_cnt.count;
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&mem_lock);

    void* page = NULL;
    
    // 优先从空闲链表获取
    if (!_empty_list(&free_pages_list)) {
        page = (void*)free_pages_list.next;
        _detach_from_list(free_pages_list.next);
    } else {
        // 如果没有空闲页面，从next_free_addr分配
        page = next_free_addr;
        next_free_addr += PAGE_SIZE;
    }
    
    // 设置引用计数为1
    if (page) {
        u64 page_index = get_page_index(page);
        increment_rc(&page_ref[page_index]);
    }

    release_spinlock(&mem_lock);
    return page;
}

void kfree_page(void* pg) {
    if (!pg) return;
    
    // 减少引用计数
    u64 page_index = get_page_index(pg);
    decrement_rc(&page_ref[page_index]);
    
    // 如果引用计数为0，释放页面
    if (page_ref[page_index].count == 0) {
        decrement_rc(&kalloc_page_cnt);  // 减少总分配计数
        
        acquire_spinlock(&mem_lock);
        
        // 将页面添加到空闲链表
        _insert_into_list(&free_pages_list, (ListNode*)pg);
        
        release_spinlock(&mem_lock);
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
    acquire_spinlock(&kalloc_lock[cpuid()]); //&mem_lock

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

    release_spinlock(&kalloc_lock[cpuid()]); //&mem_lock
    return (void*)((u64)blk + sizeof(Block));
}

void kfree(void* ptr) {
    acquire_spinlock(&kalloc_lock[cpuid()]); //&mem_lock
    Block* blk = (Block*)((u64)ptr - sizeof(Block));
    merge_adjacent(blk);
    blk->free = 1;
    release_spinlock(&kalloc_lock[cpuid()]); //&mem_lock
}

void *get_zero_page() {
    return zeroed_page;
}
