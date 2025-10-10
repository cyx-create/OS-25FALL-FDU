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
//到底要不要这个={0}
#define UPALIGN(x) (((u64)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
static SpinLock free_page_lock = {0}; // 保护 page 分配的自旋锁
static ListNode list = {&list, &list}; // 维护空闲页链表
static char *mm_end;             // 当前内存分配的结束位置
u64 endp;                        // 内核映像结束地址向上取整
static int pagenum;              // 总页数
_Atomic unsigned *refcnt;        // 每个物理页的引用计数表
static char *zero;               // 零页指针
//
//全局计数器
RefCount kalloc_page_cnt;



//初始化内核内存分配器
void kinit() {
    init_rc(&kalloc_page_cnt);
    //lab1
    //初始化保护空闲链表的自旋锁
    init_spinlock(&free_page_lock);
    init_list_node(&list);

    extern char end[];    // 链接器里定义的符号
    endp = UPALIGN(end);    // 内核结束位置向上对齐到页边界

    // 临时算一下最大页数
    u64 max_pagenum = (P2K(PHYSTOP) - endp) / PAGE_SIZE;

    // 把 refcnt 表放在 endp 之后
    refcnt = (typeof(refcnt))endp;
    endp += UPALIGN(max_pagenum * sizeof(refcnt[0]));

    // 重新计算真实页数
    pagenum = (P2K(PHYSTOP) - endp) / PAGE_SIZE;

    // 设置 mm_end 为新的堆起点
    mm_end = (char *)endp;

    // 打印调试信息
    printk("kinit: end=%p, available=%llx bytes, page_count=%d\n",
           end, (u64)(P2K(PHYSTOP) - endp), pagenum);

    // 分配一个零页并清零
    extern void *kalloc_page();
    zero = kalloc_page();
    if (zero)
        memset(zero, 0, PAGE_SIZE);
 
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);
    //lab1
    acquire_spinlock(&free_page_lock);

    void *ret = NULL;

    /* 直接判断链表是否为空 */
    bool is_empty = (list.next == &list);

    if (is_empty) {
        if ((u64)mm_end + PAGE_SIZE > (u64)P2K(PHYSTOP)) {
            release_spinlock(&free_page_lock);
            decrement_rc(&kalloc_page_cnt);
            return NULL;
        }
        ret = (void *)mm_end;
        mm_end += PAGE_SIZE;
    } else {
        ListNode *node = list.next;
        _detach_from_list(node);
        ret = (void *)node;
    }

    u64 pn = ((u64)ret - (u64)endp) / PAGE_SIZE;
    __atomic_store_n(&refcnt[pn], 1u, __ATOMIC_RELEASE);

    release_spinlock(&free_page_lock);
    return ret;
    //
    //return NULL;
}

void kfree_page(void* p) {
    //decrement_rc(&kalloc_page_cnt);
    //lab1
    if (p == NULL) return;

    acquire_spinlock(&free_page_lock);

    /* 计算页号 */
    u64 pn = ((u64)p - (u64)endp) / PAGE_SIZE;

    /* 原子地将 refcount 减 1 并取得新值 */
    unsigned newcnt = __atomic_sub_fetch(&refcnt[pn], 1u, __ATOMIC_ACQ_REL);

    if (newcnt > 0) {
        /* 仍有引用，不回收 */
       release_spinlock(&free_page_lock);
        return;
    }

    /* 引用归零，真正释放：从全局计数中减去 1 */
    decrement_rc(&kalloc_page_cnt);

    /* 插入回空闲链表（带锁版本宏 _insert_into_list 需要锁外部保护，因此此处直接调用） */
    _insert_into_list(&list, (ListNode *)p);

    release_spinlock(&free_page_lock);

    //
    return;
}

/* ---------- 小块 O(1) 分配器（基于 size-class 每页管理） ---------- */
/* 支持的 size classes */
#define NUM_CLASSES 9
static const unsigned int class_sizes[NUM_CLASSES] = {8,16,32,64,128,256,512,1024,2048};
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef unsigned long       uintptr_t;  // 指针大小
typedef unsigned long       size_t;

// 常量定义
#define UINT32_MAX 0xFFFFFFFFU

#define NCPU 4
/* 每个 class 为每个 CPU 维护一个有"有空闲块"页的链表头（PageHeader*） */
static struct PageHeader *class_page_head[NUM_CLASSES][NCPU];
static SpinLock kalloc_lock[NCPU]; /* per-CPU lock */

/* 页头结构，放在每个页的起始地址 */
typedef struct PageHeader {
    unsigned int class_idx;      // 大小类别索引
    unsigned int block_size;     // 每块字节数
    unsigned int payload_off;    // 从页面基址到第一个块的偏移
    unsigned int total_slots;    // 此页面的总块数
    unsigned int free_count;     // 当前空闲块
    unsigned int first_free_idx; // 第一个空闲块的索引，无则为 UINT32_MAX
    struct PageHeader *next_page; // 每 CPU 类别列表的下一页
} PageHeader;

/* 四舍五入以对齐 */
static inline unsigned int roundup_u(unsigned int x, unsigned int align) {
    return (x + align - 1) & ~(align - 1u);
}

/* 为给定size（向上取整）查找大小类别索引 */
static unsigned int size_to_class(unsigned int size) {
    for (unsigned int i = 0; i < NUM_CLASSES; ++i) {
        if (size <= class_sizes[i]) return i;
    }
    return (unsigned int)-1;
}

/* 为给定类初始化一个页面（放置在 page_kva） */
static void init_page_for_class(void *page_kva, unsigned int cls) {
    PageHeader *ph = (PageHeader *)page_kva;
    unsigned int bsize = class_sizes[cls];

    /* 基本字段 */
    ph->class_idx = cls;
    ph->block_size = bsize;

    /* 计算有效载荷偏移量（先将页面头对齐到8，然后对齐到块大小） */
    unsigned int meta_sz = roundup_u((unsigned int)sizeof(PageHeader), 8u);
    unsigned int payload_off = roundup_u(meta_sz, bsize);
    ph->payload_off = payload_off;

    if (payload_off >= PAGE_SIZE) {
        ph->total_slots = 0;
        ph->free_count = 0;
        ph->first_free_idx = UINT32_MAX;
        ph->next_page = NULL;
        return;
    }

    unsigned int usable = PAGE_SIZE - payload_off;
    unsigned int total = usable / bsize;
    ph->total_slots = total;
    ph->free_count = total;
    ph->next_page = NULL;

    if (total == 0) {
        ph->first_free_idx = UINT32_MAX;
        return;
    }

    /* 在页面内构建空闲列表：在每个块的起始位置存储下一个索引（uint32_t） */
    for (unsigned int i = 0; i < total; ++i) {
        uint32_t next = (i + 1 < total) ? (i + 1) : UINT32_MAX;
        uint32_t *slot = (uint32_t *)((char *)page_kva + payload_off + (size_t)i * bsize);
        *slot = next;
    }
    ph->first_free_idx = 0;
}

/* 根据页面头和索引计算块地址 */
static inline void *page_block_addr(PageHeader *ph, unsigned int idx) {
    return (void *)((char *)ph + ph->payload_off + (size_t)idx * ph->block_size);
}

/* 从指针和页面头计算索引 */
static inline unsigned int page_index_from_ptr(PageHeader *ph, void *ptr) {
    uintptr_t base = (uintptr_t)ph;
    uintptr_t off = (uintptr_t)ptr - (base + ph->payload_off);
    return (unsigned int)(off / ph->block_size);
}

/* 从类头列表中移除头页面（调用者必须持有每 CPU 锁） */
static inline PageHeader *pop_head_page(unsigned int cls, unsigned int cpu) {
    PageHeader *h = class_page_head[cls][cpu];
    if (!h) return NULL;
    class_page_head[cls][cpu] = h->next_page;
    h->next_page = NULL;
    return h;
}

/* 将页面推入类头列表（调用者必须持有每CPU锁） */
static inline void push_head_page(unsigned int cls, unsigned int cpu, PageHeader *ph) {
    ph->next_page = class_page_head[cls][cpu];
    class_page_head[cls][cpu] = ph;
}

/* 确保每个 CPU 锁已初始化（可安全多次调用） */
static inline void ensure_cpu_lock_init(unsigned int cpu) {
    init_spinlock(&kalloc_lock[cpu]);
}

/* 实现公共函数: kalloc / kfree */
void* kalloc(unsigned long long size) {
    if (size == 0) return NULL;
    if (size > PAGE_SIZE/2) {
        /* 这里不处理大分配 */
        printk("kalloc: request too large\n");
        return NULL;
    }

    /* 选择类别 */
    unsigned long long requested = size;
    /* 如果大小不是8的倍数，使用4字节对齐类 */
    unsigned int cls;
    if (requested & 0x7) {
        /* 对齐到4字节，然后映射到大于或等于该大小的最小类 */
        requested = (requested + 3) & ~0x3ULL;
    } else {
        requested = (requested + 7) & ~0x7ULL;
    }

    cls = size_to_class((unsigned int)requested);
    if (cls == (unsigned int)-1) return NULL; /* 对该类别来说太大 */

    unsigned int cpu = cpuid();
    ensure_cpu_lock_init(cpu);
    acquire_spinlock(&kalloc_lock[cpu]);

    PageHeader *ph = class_page_head[cls][cpu];

    /* 如果 head 为 NULL 或 head 没有可用块，则分配一个新页面并初始化 */
    if (!ph || ph->first_free_idx == UINT32_MAX) {
        void *page = kalloc_page();
        if (!page) {
            release_spinlock(&kalloc_lock[cpu]);
            return NULL;
        }
        init_page_for_class(page, cls);
        ph = (PageHeader *)page;
        /* 将新页面插入为头部（它有空闲块） */
        push_head_page(cls, cpu, ph);
    }

    /* 从头页分配一个块（ph 指向头页，保证有空闲） */
    unsigned int idx = ph->first_free_idx;
    /* 从块中读取下一个索引 */
    uint32_t *block_ptr = (uint32_t *)page_block_addr(ph, idx);
    uint32_t next_idx = *block_ptr;
    ph->first_free_idx = (next_idx == UINT32_MAX) ? UINT32_MAX : next_idx;
    ph->free_count--;

    /* 如果页面已满（free_count == 0），则将其从头部移除（这样未来分配就不会扫描它） */
    if (ph->free_count == 0) {
        /* pop head */
        PageHeader *popped = pop_head_page(cls, cpu);
        (void)popped; // popped should be ph
    }

    release_spinlock(&kalloc_lock[cpu]);

    /* 返回指向有效载荷的指针（块起始位置） */
    void *userptr = page_block_addr(ph, idx);
    return userptr;
}


/* ---------- 新增的 helper（用于 optional 回收） ---------- */

/* 获取并持有所有 CPU 的 kalloc_lock（按 CPU 索引升序以避免死锁） */
static inline void acquire_all_cpu_locks(void) {
    for (unsigned int i = 0; i < NCPU; ++i) {
        ensure_cpu_lock_init(i);
    }
    for (unsigned int i = 0; i < NCPU; ++i) {
        acquire_spinlock(&kalloc_lock[i]);
    }
}

/* 释放所有 CPU 的 kalloc_lock（释放顺序可以与获取顺序相同） */
static inline void release_all_cpu_locks(void) {
    for (unsigned int i = 0; i < NCPU; ++i) {
        release_spinlock(&kalloc_lock[i]);
    }
}

/* 在持有所有 CPU 锁的前提下，从每个 class_page_head 中查找并移除 ph（如果存在） */
static bool remove_page_from_all_lists(unsigned int cls, PageHeader *ph) {
    bool removed = false;
    for (unsigned int c = 0; c < NCPU; ++c) {
        PageHeader *prev = NULL;
        PageHeader *cur = class_page_head[cls][c];
        while (cur) {
            if (cur == ph) {
                /* 从链表中摘除 cur */
                if (prev == NULL) {
                    class_page_head[cls][c] = cur->next_page;
                } else {
                    prev->next_page = cur->next_page;
                }
                cur->next_page = NULL;
                removed = true;
                break; /* 一个页最多出现在一个 cpu 的链表里（作为链表节点），找到后可以退出 */
            }
            prev = cur;
            cur = cur->next_page;
        }
        if (removed) break;
    }
    return removed;
}

/* 尝试把一个完全空闲页返还给物理页分配器（线程安全）。
   调用约定：调用时 caller 不必持有任何 kalloc_lock（本函数会获取所有 CPU 的锁）。
*/
static void try_return_page_to_system(PageHeader *ph) {
    if (!ph) return;
    unsigned int cls = ph->class_idx;
    uintptr_t page_base = (uintptr_t)ph;

    /* 获取所有 cpu 的锁，保证我们能查看并修改所有 class_page_head 列表 */
    acquire_all_cpu_locks();

    /* 再次确认页仍然完全空闲（在我们释放当前 CPU 锁后别的 CPU 可能分配了它） */
    if (ph->free_count != ph->total_slots) {
        /* 不再完全空闲，放弃回收 */
        release_all_cpu_locks();
        return;
    }

    /* 从所有 class lists 中移除该页（如果存在的话） */
    remove_page_from_all_lists(cls, ph);

    /* 释放所有 kalloc_lock */
    release_all_cpu_locks();

    /* 现在安全地把物理页归还给 kfree_page（kfree_page 自身会获取 free_page_lock） */
    kfree_page((void *)page_base);
}

/* ---------- 修改后的 kfree（把 optional 回收逻辑并入） ---------- */

void kfree(void *ptr) {
    if (!ptr) return;

    /* 定位页头 */
    uintptr_t page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
    PageHeader *ph = (PageHeader *)page_base;

    unsigned int cls = ph->class_idx;
    if (cls >= NUM_CLASSES) {
        /* 不是小块分配页，直接忽略（或打印错误） */
        return;
    }

    unsigned int idx = page_index_from_ptr(ph, ptr);

    unsigned int cpu = cpuid();
    ensure_cpu_lock_init(cpu);
    acquire_spinlock(&kalloc_lock[cpu]);

    /* 将此块推回页面的空闲列表（LIFO） */
    uint32_t old_head = (ph->first_free_idx == UINT32_MAX) ? UINT32_MAX : ph->first_free_idx;
    uint32_t *block_slot = (uint32_t *)ptr;
    *block_slot = old_head;
    ph->first_free_idx = idx;
    ph->free_count++;

    /* 如果页面之前已满（即现在 free_count == 1），则将其插回到表头列表 */
    if (ph->free_count == 1) {
        push_head_page(cls, cpu, ph);
    }

    /* 检查是否现在完全空闲，若是则尝试回收到物理分配器 */
    bool fully_free = (ph->free_count == ph->total_slots);

    /* 释放当前 CPU 锁（之后 try_return_page_to_system 会获取所有锁并再次检查） */
    release_spinlock(&kalloc_lock[cpu]);

    if (fully_free) {
        try_return_page_to_system(ph);
    }
}

