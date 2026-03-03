#include <elf.h>
#include <common/string.h>
#include <common/defines.h>
#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/printk.h>

extern int fdalloc(struct file *f);
#define USERTOP (1 + ~KSPACE_MASK)  // 0x0001000000000000
#define STACK_PAGE 32               // 128KB 
#define UPALIGN(x) (((x) + 0xf) & ~0xf)



// Helper function to check segment type
static inline int get_section_type(u32 flags) {
    if (flags == (PF_R | PF_X)) return ST_TEXT;
    if (flags == (PF_R | PF_W)) return ST_FILE;
    return -1;
}

// Helper function to validate ELF header
static inline bool is_valid_elf(const Elf64_Ehdr *hdr) {
    return (memcmp(hdr->e_ident, ELFMAG, SELFMAG) == 0) && 
           (hdr->e_ident[EI_CLASS] == ELFCLASS64);
}

// Load a single ELF program segment
static bool map_elf_segment(struct pgdir *dir, Inode *file, 
                           const Elf64_Phdr *phdr) {
    // Validate segment
    if (phdr->p_memsz < phdr->p_filesz) {
        PANIC();  // Invalid segment: memsz < filesz
    }
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) {
        PANIC();  // Address overflow
    }
    
    int seg_type = get_section_type(phdr->p_flags);
    if (seg_type < 0) return false;
    
    // Create and add section entry
    struct section *seg = kalloc(sizeof(struct section));
    if (!seg) return false;
    
    seg->flags = seg_type;
    seg->mmap_flags = 0;
    seg->begin = phdr->p_vaddr;
    seg->end = (seg_type == ST_TEXT) ? phdr->p_vaddr + phdr->p_filesz
                                     : phdr->p_vaddr + phdr->p_memsz;
    seg->fp = NULL;
    
    _insert_into_list(&dir->section_head, &seg->stnode);
    
    // Load file content
    u64 vaddr = phdr->p_vaddr;
    u64 file_pos = phdr->p_offset;
    
    while (vaddr < phdr->p_vaddr + phdr->p_filesz) {
        u64 page_start = PAGE_BASE(vaddr);
        u64 copy_len = MIN(PAGE_SIZE - (vaddr - page_start), 
                          phdr->p_vaddr + phdr->p_filesz - vaddr);
        
        void *page = kalloc_page();
        if (!page) return false;
        
        memset(page, 0, PAGE_SIZE);
        
        u64 pte_flags = PTE_USER_DATA;
        if (seg_type == ST_TEXT) {
            pte_flags |= PTE_RO;
        }
        
        vmmap(dir, page_start, page, pte_flags);
        
        // Read file data into page
        if (inodes.read(file, (u8 *)page + (vaddr - page_start), 
                       file_pos, copy_len) != copy_len) {
            return false;
        }
        
        vaddr += copy_len;
        file_pos += copy_len;
    }
    
    // Align to next page
    if (vaddr != PAGE_BASE(vaddr)) {
        vaddr = PAGE_BASE(vaddr) + PAGE_SIZE;
    }
    
    // Map zero pages for BSS section
    if (seg_type == ST_FILE && phdr->p_memsz > vaddr - phdr->p_vaddr) {
        while (vaddr < phdr->p_vaddr + phdr->p_memsz) {
            u64 page_start = PAGE_BASE(vaddr);
            u64 zero_len = MIN(PAGE_SIZE - (vaddr - page_start), 
                              phdr->p_vaddr + phdr->p_memsz - vaddr);
            
            vmmap(dir, page_start, get_zero_page(), PTE_USER_DATA | PTE_RO);
            vaddr += zero_len;
        }
    }
    
    return true;
}

// Load ELF executable
static bool load_elf_binary(struct pgdir *dir, const char *path, 
                            Elf64_Ehdr *out_hdr) {
    OpContext ctx;
    bcache.begin_op(&ctx);
    
    Inode *file = namei(path, &ctx);
    if (!file) {
        bcache.end_op(&ctx);
        return false;
    }
    
    inodes.lock(file);
    
    // Read and validate ELF header
    Elf64_Ehdr hdr;
    if (inodes.read(file, (u8 *)&hdr, 0, sizeof(hdr)) != sizeof(hdr)) {
        goto cleanup_fail;
    }
    
    if (!is_valid_elf(&hdr)) {
        goto cleanup_fail;
    }
    
    // Process program headers
    for (usize i = 0; i < hdr.e_phnum; i++) {
        u64 phdr_offset = hdr.e_phoff + i * sizeof(Elf64_Phdr);
        Elf64_Phdr phdr;
        
        if (inodes.read(file, (u8 *)&phdr, phdr_offset, sizeof(phdr)) 
            != sizeof(phdr)) {
            goto cleanup_fail;
        }
        
        if (phdr.p_type != PT_LOAD) continue;
        
        if (!map_elf_segment(dir, file, &phdr)) {
            goto cleanup_fail;
        }
    }
    
    *out_hdr = hdr;
    
    inodes.unlock(file);
    inodes.put(&ctx, file);
    bcache.end_op(&ctx);
    return true;
    
cleanup_fail:
    inodes.unlock(file);
    inodes.put(&ctx, file);
    bcache.end_op(&ctx);
    return false;
}

// Lock for synchronizing exec operations
static SleepLock exec_load_lock = {
    .lock = {0}, 
    .val = 1, 
    .sleeplist = {&exec_load_lock.sleeplist, &exec_load_lock.sleeplist}
};

int execve(const char *path, char *const argv[], char *const envp[])
{
    /* (Final) TODO BEGIN */
    // printk("[EXEC] Loading program: %s\n", path);
    
    // Allocate new page directory
    struct pgdir *new_dir = kalloc(sizeof(struct pgdir));
    if (!new_dir) return -1;
    init_pgdir(new_dir);
    
    Elf64_Ehdr elf_hdr;
    
    // Load ELF file with synchronization
    unalertable_acquire_sleeplock(&exec_load_lock);
    bool load_success = load_elf_binary(new_dir, path, &elf_hdr);
    release_sleeplock(&exec_load_lock);
    
    if (!load_success) {
        free_pgdir(new_dir);
        return -1;
    }
    
    // Set up user stack
    u64 stack_ptr = USERTOP;
    
    // Allocate stack pages
    for (int i = 1; i <= STACK_PAGE; i++) {
        void *page = kalloc_page();
        memset(page, 0, PAGE_SIZE);
        vmmap(new_dir, stack_ptr - i * PAGE_SIZE, page, PTE_USER_DATA);
    }
    
    // Record stack section
    struct section *stack_sec = kalloc(sizeof(struct section));
    memset(stack_sec, 0, sizeof(struct section));
    stack_sec->flags = ST_FILE;
    stack_sec->begin = stack_ptr - STACK_PAGE * PAGE_SIZE;
    stack_sec->end = stack_ptr;
    _insert_into_list(&new_dir->section_head, &stack_sec->stnode);
    
    // Prepare arguments and environment
    u64 argc = 0, envc = 0;
    while (envp && envp[envc]) envc++;
    while (argv && argv[argc]) argc++;
    
    uint64_t arg_addrs[argc + 1];
    uint64_t env_addrs[envc + 1];
    
    // Push environment strings
    stack_ptr -= 16;
    copyout(new_dir, (void *)stack_ptr, 0, 8);
    
    if (envp) {
        for (int i = envc - 1; i >= 0; i--) {
            usize str_len = strlen(envp[i]) + 1;
            stack_ptr -= str_len;
            stack_ptr -= stack_ptr % 16;  // 16-byte alignment
            
            copyout(new_dir, (void *)stack_ptr, envp[i], str_len);
            env_addrs[i] = stack_ptr;
        }
    }
    env_addrs[envc] = 0;
    
    stack_ptr -= 8;
    copyout(new_dir, (void *)stack_ptr, 0, 8);
    
    // Push argument strings
    if (argv) {
        for (int i = argc - 1; i >= 0; i--) {
            usize str_len = strlen(argv[i]) + 1;
            stack_ptr -= str_len;
            stack_ptr -= stack_ptr % 16;  // 16-byte alignment
            
            copyout(new_dir, (void *)stack_ptr, argv[i], str_len);
            arg_addrs[i] = stack_ptr;
        }
    }
    arg_addrs[argc] = 0;
    
    // Push environment pointer array
    stack_ptr -= (envc + 1) * 8;
    copyout(new_dir, (void *)stack_ptr, env_addrs, (envc + 1) * 8);
    
    // Push argument pointer array
    stack_ptr -= (argc + 1) * 8;
    copyout(new_dir, (void *)stack_ptr, arg_addrs, (argc + 1) * 8);
    
    // Push argument count
    stack_ptr -= 8;
    copyout(new_dir, (void *)stack_ptr, &argc, sizeof(argc));
    
    // Switch to new address space
    Proc *proc = thisproc();
    struct pgdir old_dir = proc->pgdir;
    proc->pgdir = *new_dir;
    
    _insert_into_list(&new_dir->section_head, &proc->pgdir.section_head);
    _detach_from_list(&new_dir->section_head);
    
    proc->ucontext->elr = elf_hdr.e_entry;
    proc->ucontext->sp = stack_ptr;
    
    attach_pgdir(&proc->pgdir);
    arch_tlbi_vmalle1is();
    free_pgdir(&old_dir);
    
    return 0;
    /* (Final) TODO END */
}