#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/paging.h>

Proc root_proc;

void kernel_entry();
void proc_entry();

static SpinLock plock = {0};

typedef struct {
    int id;
    ListNode lnode;
} PidNode;


//尝试3
typedef struct {
    int next_pid;     
    ListNode freepid; 
    SpinLock lock;    
} PIDManager;

static PIDManager pmanager;

static void init_pidmanager(PIDManager *m)
{
    m->next_pid = 0;
    init_list_node(&m->freepid);
    init_spinlock(&m->lock);
}

static int pid_get(PIDManager *m)
{
    int id;
    acquire_spinlock(&m->lock);
    if (_empty_list(&m->freepid)) {
        id = m->next_pid++;
    } else {
        PidNode *pn = container_of(m->freepid.next, PidNode, lnode);
        id = pn->id;
        _detach_from_list(&pn->lnode);
        kfree(pn);
    }
    release_spinlock(&m->lock);
    return id;
}

static void pid_reuse(PIDManager *m, int pid)
{
    PidNode *pn = kalloc(sizeof(PidNode));
    pn->id = pid;
    init_list_node(&pn->lnode);
    acquire_spinlock(&m->lock);
    _insert_into_list(&m->freepid, &pn->lnode);
    release_spinlock(&m->lock);
}
//

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    // TODO:
    // 1. init global resources (e.g. locks, semaphores)
    // 2. init the root_proc (finished)
    init_spinlock(&plock);
    init_pidmanager(&pmanager);
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);

}

void init_proc(Proc *p)
{
    // TODO:
    // setup the Proc with kstack and pid allocated设置一个新的进程Proc，使其成为一个可被调度运行的内核或用户进程
    // NOTE: be careful of concurrency
    //初始化基本标志位

    acquire_spinlock(&plock);
    memset(p, 0, sizeof(*p));

    // 分配内核栈空间
    p->kstack = kalloc_page();

    // 初始化同步与子进程结构
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);

    // 初始化调度信息
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);

    p->killed = false;
    p->idle = false;
    p->parent = NULL;
    p->state = UNUSED;
    p->exitcode = 0;

    // 设置上下文指针
    p->kcontext = (KernelContext *)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext = (UserContext *)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));

    p->pid = pid_get(&pmanager);
    release_spinlock(&plock);
}

Proc *create_proc()
{
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

//没问题
void set_parent_to_this(Proc *proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    acquire_spinlock(&plock);
    proc->parent = thisproc(); //获取当前执行函数的父进程
    _insert_into_list(&thisproc()->children, &proc->ptnode); //建立父子关系
    release_spinlock(&plock);
}

//没问题
int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    // TODO:
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency
    // 设置父进程
    //2->3,锁改到了外面
    acquire_spinlock(&plock);
    if(p->parent == NULL)
    {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }
    release_spinlock(&plock);
    // 设置内核上下文
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    //激活进程
    int id = p->pid;
    activate_proc(p);
    return id;  
}

int wait(int *exitcode)
{
    // TODO:
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency
    Proc *this = thisproc();
    acquire_spinlock(&plock);
    if(this->children.next == &this->children){
        release_spinlock(&plock);
        return -1;
    }
    release_spinlock(&plock);

    if(!wait_sem(&this->childexit)){
        return -1;
    }

    acquire_spinlock(&plock);
    acquire_sched_lock();

    Proc* znode = NULL;
    _for_in_list(p, &this->children){
        if(p == &this->children){
            continue;
        }
        auto child = container_of(p, Proc, ptnode);
        if(child->state == ZOMBIE){
            znode = child;
            break;
        }
    }

    if(znode != NULL){
        _detach_from_list(&znode->ptnode);
        _detach_from_list(&znode->schinfo.rq);

        *exitcode = znode->exitcode;

        kfree_page(znode->kstack);

        int ret_pid = znode->pid;
        pid_reuse(&pmanager, znode->pid);
        kfree(znode);

        release_sched_lock();
        release_spinlock(&plock);

        return ret_pid;
    }

    release_sched_lock();
    release_spinlock(&plock);

    return -1;
}

NO_RETURN void exit(int code)
{
    // TODO:
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency

    Proc *current_proc = thisproc();
    ASSERT(current_proc != &root_proc);
    current_proc->exitcode = code;

    // 关闭所有打开的文件
    for (int file_index = 0; file_index < NOFILE; ++file_index) {
        if (current_proc->oftable.ofile[file_index]) {
            file_close(current_proc->oftable.ofile[file_index]);
            current_proc->oftable.ofile[file_index] = NULL;
        }
    }

    // 释放当前工作目录
    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, current_proc->cwd);
    bcache.end_op(&ctx);
    current_proc->cwd = NULL;

    // 释放内存段
    free_sections(&current_proc->pgdir);

    // 处理子进程和父进程通知
    acquire_spinlock(&plock);
    post_sem(&current_proc->parent->childexit);

    // 统计僵尸子进程数量
    int zombie_count = 0;
    ListNode *child_node = current_proc->children.next;
    while (child_node != &current_proc->children) {
        Proc *child_proc = container_of(child_node, Proc, ptnode);
        child_proc->parent = &root_proc;
        if (child_proc->state == ZOMBIE) {
            zombie_count++;
        }
        child_node = child_node->next;
    }

    // 转移子进程给根进程
    if (!_empty_list(&current_proc->children)) {
        ListNode *first_child = current_proc->children.next;
        ListNode *last_child = current_proc->children.prev;
        
        // 连接到根进程的子进程链表
        last_child->next = &root_proc.children;
        first_child->prev = root_proc.children.prev;
        root_proc.children.prev->next = first_child;
        root_proc.children.prev = last_child;
        
        // 清空当前进程的子进程链表
        _detach_from_list(&current_proc->children);
        
        // 通知根进程有僵尸子进程
        for (int i = 0; i < zombie_count; i++) {
            post_sem(&root_proc.childexit);
        }
    }

    // 获取调度锁并释放页目录
    acquire_sched_lock();
    free_pgdir(&current_proc->pgdir);
    release_spinlock(&plock);

    // 切换到僵尸状态
    sched(ZOMBIE);

    PANIC(); // prevent the warning of 'no_return function returns'
}

int kill(int pid)
{
    // TODO:
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).
    //尝试1，不确定
    int ret = -1;  // 默认找不到

    acquire_spinlock(&plock); // 锁住整个进程树

    Proc *stack[128];          // 用于模拟递归
    int top = 0;
    stack[top++] = &root_proc;

    while (top > 0) {
        Proc *current = stack[--top];

        // 如果找到目标进程且不是 UNUSED
        if (current->pid == pid && !is_unused(current)) {
            current->killed = 1;    // 设置 killed 标记
            alert_proc(current);    // 唤醒进程
            ret = 0;                // 标记成功
            break;                  // 找到就结束
        }

        // 将子进程加入 stack
        _for_in_list(p, &current->children) {
            if (p == &current->children) continue;
            Proc *child = container_of(p, Proc, ptnode);
            stack[top++] = child;
        }
    }

    release_spinlock(&plock);

    return ret;
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
int fork(void)
{
    /**
     * (Final) TODO BEGIN
     * 
     * 1. Create a new child process.
     * 2. Copy the parent's memory space.
     * 3. Copy the parent's trapframe.
     * 4. Set the parent of the new proc to the parent of the parent.
     * 5. Set the state of the new proc to RUNNABLE.
     * 6. Activate the new proc and return its pid.
     */

    // 1. Create a new child process
    Proc *child_proc = create_proc();
    if (child_proc == NULL) {
        return -1;
    }
    
    // 2. Get current parent process
    Proc *parent_proc = thisproc();
    
    // 3. Copy parent's memory space with Copy-on-Write
    acquire_spinlock(&parent_proc->pgdir.lock);
    
    // Copy page directory sections with Copy-on-Write
    ListNode *section_head = &parent_proc->pgdir.section_head;
    ListNode *current = section_head->next;
    
    // 遍历所有 section 节点
    while (current != section_head) {
        struct section *st = container_of(current, struct section, stnode);
        
        // Apply Copy-on-Write for each page in the section
        for (u64 va = PAGE_BASE(st->begin); va < st->end; va += PAGE_SIZE) {
            PTEntriesPtr old_pte = get_pte(&parent_proc->pgdir, va, false);
            
            if ((old_pte == NULL) || !(*old_pte & PTE_VALID)) {
                continue;
            }
            
            // Map the same physical page but mark as read-only for CoW
            vmmap(&child_proc->pgdir, 
                  va, 
                  (void *)P2K(PTE_ADDRESS(*old_pte)), 
                  PTE_FLAGS(*old_pte) | PTE_RO);
        }
        
        current = current->next;
    }
    
    // Copy section metadata
    copy_sections(&parent_proc->pgdir.section_head, 
                  &child_proc->pgdir.section_head);
    
    release_spinlock(&parent_proc->pgdir.lock);
    
    // 4. Copy parent's trapframe/context
    memcpy(child_proc->ucontext, parent_proc->ucontext, sizeof(*child_proc->ucontext));
    
    // Fork returns 0 in the child process
    child_proc->ucontext->x[0] = 0;
    
    // 5. Set parent relationship
    set_parent_to_this(child_proc);
    
    // 6. Copy file descriptors
    for (usize i = 0; i < NOFILE; i++) {
        if (parent_proc->oftable.ofile[i]) {
            child_proc->oftable.ofile[i] = file_dup(parent_proc->oftable.ofile[i]);
        }
    }
    
    // 7. Copy current working directory
    child_proc->cwd = inodes.share(parent_proc->cwd);
    
    // 8. Start the child process and return its PID
    return start_proc(child_proc, trap_return, 0);
    
    /* (Final) TODO END */
}

