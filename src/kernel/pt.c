#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <aarch64/intrinsic.h>

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc)
{
    // TODO:
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.
    // Return Kernel Address.

    //尝试1
    PTEntriesPtr pt0 = pgdir->pt; //指向页表项的指针，L0表

    //2
    if(!pt0){
        if(!alloc) {
            return NULL;
        }
        pt0 = kalloc_page(); //返回的是内核虚拟地址
        if (!pt0) return NULL; //分配失败
        memset(pt0, 0, PAGE_SIZE); //初始化新分配的页表
        pgdir->pt = pt0; //将新分配的L0页表保存到pgdir中
    }

    //1 L0 -> L1
    if(!(pt0[VA_PART0(va)] )){
        if(!alloc){
            return NULL;
        }
        void* new_page = kalloc_page();
        if(! new_page) return NULL;
        memset(new_page, 0, PAGE_SIZE);
        pt0[VA_PART0(va)] = K2P(new_page) | PTE_TABLE;
    }

    PTEntriesPtr pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));

    // L1 → L2
    if(!(pt1[VA_PART1(va)] )){
        if(!alloc){
            return NULL;
        }
        void* new_page = kalloc_page();
        if(! new_page) return NULL;
        memset(new_page, 0, PAGE_SIZE);
        pt1[VA_PART1(va)] = K2P(new_page) | PTE_TABLE;
    }


    PTEntriesPtr pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));

    // L2 → L3
    if(!(pt2[VA_PART2(va)] )){
        if(!alloc){
            return NULL;
        }
        void* new_page = kalloc_page();
        if(! new_page) return NULL;
        memset(new_page, 0, PAGE_SIZE);
        pt2[VA_PART2(va)] = K2P(new_page) | PTE_TABLE;
    }


    PTEntriesPtr pt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));

    // 返回 L3 页表项指针
    return &pt3[VA_PART3(va)];

}

void init_pgdir(struct pgdir *pgdir)
{
    pgdir->pt = NULL;
}

void free_pgdir(struct pgdir *pgdir)
{
    // TODO:
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE

    //尝试1
    if (!pgdir || !pgdir->pt) return;   // 页表不存在，直接返回

    PTEntriesPtr l0 = pgdir->pt;
    for (int i0 = 0; i0 < N_PTE_PER_TABLE; i0++) {
        if (l0[i0]) {
            PTEntriesPtr l1 = (PTEntriesPtr)P2K(PTE_ADDRESS(l0[i0]));
            for (int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
                if (l1[i1] ) {
                    PTEntriesPtr l2 = (PTEntriesPtr)P2K(PTE_ADDRESS(l1[i1]));
                    for (int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                        if (l2[i2]) {
                            PTEntriesPtr l3 = (PTEntriesPtr)P2K(PTE_ADDRESS(l2[i2]));
                            // L3 表指向的物理页不要释放
                            kfree_page(l3); // 如果 L3 表也是单独分配的一页，可以释放
                        }
                    }
                    kfree_page(l2);  // 释放 L2 页表页
                }
            }
            kfree_page(l1);      // 释放 L1 页表页
        }
    }
    kfree_page(l0);          // 释放 L0 页表页
    pgdir->pt = NULL;
}

void attach_pgdir(struct pgdir *pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}
