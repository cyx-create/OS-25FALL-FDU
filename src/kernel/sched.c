#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <common/list.h>
#include <common/string.h>

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

//new
static SpinLock rqlock;
static ListNode rq;



void init_sched()
{
    // TODO: initialize the scheduler
    // 1. initialize the resources (e.g. locks, semaphores)
    // 2. initialize the scheduler info of each CPU
    init_spinlock(&rqlock);
    init_list_node(&rq);

    for(int i=0 ; i<NCPU ; i++)
    {
        struct Proc* p = kalloc(sizeof(struct Proc));
        //memset(p, 0, sizeof(struct Proc));  // 清零
        p->idle = 1;
        p->state = RUNNING;
        //p->pid = -1; 
        //init_schinfo(&p->schinfo); //在proc的init里面
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
    }
}

//没问题
Proc *thisproc()
{
    // TODO: return the current process
    return cpus[cpuid()].sched.thisproc;
}

//没问题
void init_schinfo(struct schinfo *p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p -> rq);
}

//没问题
void acquire_sched_lock()
{
    // TODO: acquire the sched_lock if need
    acquire_spinlock(&rqlock);
}

//没问题
void release_sched_lock()
{
    // TODO: release the sched_lock if need
    release_spinlock(&rqlock);
}

bool is_zombie(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}

//没问题
bool activate_proc(Proc *p)
{
    // TODO:
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic
    acquire_sched_lock();

    if(p->state == RUNNING || p->state ==  RUNNABLE || p->state ==  ZOMBIE)
    {
        release_sched_lock();
        return false;
    }
    //acquire_sched_lock();
    if(p->state == SLEEPING || p->state == UNUSED)
    {
        p->state = RUNNABLE;
        _insert_into_list(&rq, &p->schinfo.rq);
        release_sched_lock();
        return true;
    }
    else
        PANIC();
    release_sched_lock();
    return true;
}

//改过了
static void update_this_state(enum procstate new_state)
{
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary
    // thisproc()->state = new_state;

    //尝试2
    // thisproc()->state = new_state;
    Proc *this = thisproc();
    
    // 获取当前CPU的idle进程
    Proc *idle_proc = cpus[cpuid()].sched.idle;
    
    // 如果不是idle进程，且当前状态是RUNNABLE或RUNNING，从运行队列移除
    if (this != idle_proc && (this->state == RUNNABLE || this->state == RUNNING)) {
        _detach_from_list(&this->schinfo.rq);
    }
    
    // 更新状态
    this->state = new_state;
    
    // 如果不是idle进程，且新状态是RUNNABLE或RUNNING，加入运行队列
    if (this != idle_proc && (new_state == RUNNABLE || new_state == RUNNING)) {
        _insert_into_list(&rq, &this->schinfo.rq);
    }

    //尝试1 暂时靠谱版
    // thisproc()->state = new_state; // 修改当前运行进程的状态
    
    // if(new_state == ZOMBIE || new_state == SLEEPING) {
    //     // 从就绪队列中移除
    //     _detach_from_list(&thisproc()->schinfo.rq);
    // }



}

extern bool panic_flag;
//没问题
static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    if(panic_flag)
    {
        return cpus[cpuid()].sched.idle;
    }
    _for_in_list(p, &rq)
    {
        if(p == &rq)
        continue;
        auto proc = container_of(p, struct Proc, schinfo.rq);
        if(proc->state == RUNNABLE)
        {
            return proc;
        }
    }
    return cpus[cpuid()].sched.idle;
}

//没问题
static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process
    //reset_clock(1000);
    cpus[cpuid()].sched.thisproc = p; 
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        swtch(next->kcontext, &this->kcontext);
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
