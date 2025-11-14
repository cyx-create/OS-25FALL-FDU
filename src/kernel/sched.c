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

// //lab3尝试1
// static void update_this_state(enum procstate new_state);
// // ---------------- CPU 定时器回调 ----------------
// void timer_handler(struct timer *t) {
//     Proc *p = (Proc *)t->data;
//     if (p && p->state == RUNNING) {
//         acquire_sched_lock();
//         update_this_state(RUNNABLE); // 抢占当前进程
//         sched(RUNNABLE);
//         release_sched_lock();
//     }
// }


// //lab3尝试2
static struct timer CPU_timer[NCPU];

void timer_handler(struct timer* timer){

    if (!timer) return;

    Proc *cur = thisproc();
    if (!cur || cur->idle) {
        return; // idle 进程不抢占
    }

    timer->data = 0;
    acquire_sched_lock();
    sched(RUNNABLE);
    // release_sched_lock();
}



void init_sched()
{
    // TODO: initialize the scheduler
    // 1. initialize the resources (e.g. locks, semaphores)
    // 2. initialize the scheduler info of each CPU
    init_spinlock(&rqlock);
    init_list_node(&rq);

    //lab3尝试2
    for(int i = 0; i < NCPU; i++){
        CPU_timer[i].triggered = 1;
        CPU_timer[i].elapse = 10;
        CPU_timer[i].handler = &timer_handler;
        CPU_timer[i].data = i;
    }

    for(int i=0 ; i<NCPU ; i++)
    {
        struct Proc* p = kalloc(sizeof(struct Proc));
        //memset(p, 0, sizeof(struct Proc)); 
        p->idle = 1;
        p->state = RUNNING;
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
    }

    //lab3尝试1
    // init_clock_handler();
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

bool is_unused(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == UNUSED;
    release_sched_lock();
    return r;
}

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

    Proc *this = thisproc();
    
    // 获取当前CPU的idle进程
    Proc *idle_proc = cpus[cpuid()].sched.idle;
    
    // 如果不是idle进程，且当前状态是RUNNABLE或RUNNING，从运行队列移除
    if (this != idle_proc && (this->state == RUNNABLE || this->state == RUNNING)) {
        _detach_from_list(&this->schinfo.rq);
    }
    
    // 更新状态
    this->state = new_state;
    
    // 如果不是idle进程，且新状态是RUNNABLE或RUNNING，加入运行队列的尾部！！！
    if (this != idle_proc && (new_state == RUNNABLE || new_state == RUNNING)) {
        _insert_into_list(rq.prev, &this->schinfo.rq);
    }

    //lab3尝试1
    // 设置定时器，非 idle 且 RUNNABLE/RUNNING 才设置
    // if(this != idle_proc && this->state == RUNNING) {
    //     static struct timer t;
    //     t.elapse =5; // 时间片，单位可根据实验要求修改
    //     t.handler = &cpu_preempt_handler;
    //     t.data = (u64)this;
    //     set_cpu_timer(&t);
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
        if(p == &rq) continue;
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

    // lab3 尝试2
    if(!CPU_timer[cpuid()].triggered){
        cancel_cpu_timer(&CPU_timer[cpuid()]);
    }

    set_cpu_timer(&CPU_timer[cpuid()]);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    //lab3新加
    if(this->killed && new_state != ZOMBIE){
        release_sched_lock();
        return;
    }

    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        attach_pgdir(&next->pgdir);
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