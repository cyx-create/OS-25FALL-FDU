#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>

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
    auto this = thisproc();
    this->exitcode = code;

    acquire_spinlock(&plock);
    acquire_sched_lock();

    int times = 0;
    _for_in_list(p, &this->children){
        if(p == &this->children){
            continue;
        }
        auto child = container_of(p, Proc, ptnode);
        child->parent = &root_proc;
        if(child->state == ZOMBIE){
            times++;
        }
    }

    if(!_empty_list(&this->children)){
        _merge_list(&root_proc.children, this->children.next);
        _detach_from_list(&this->children);
        release_sched_lock();
        for(int i = 0; i < times; i++){
            post_sem(&root_proc.childexit);
        }
        acquire_sched_lock();
    }
    free_pgdir(&this->pgdir);

    release_sched_lock();

    post_sem(&thisproc()->parent->childexit);

    acquire_sched_lock();
    release_spinlock(&plock);
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

