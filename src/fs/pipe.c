#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>
#include <kernel/printk.h>

void init_pipe(Pipe *pi)
{
    /* (Final) TODO BEGIN */
    init_spinlock(&pi->lock);
    init_sem(&pi->wlock, 0);
    init_sem(&pi->rlock, 0);
    pi->nread = pi->nwrite = 0;
    pi->readopen = pi->writeopen = 1;
    /* (Final) TODO END */
}

void init_read_pipe(File *readp, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    readp->type = FD_PIPE;
    readp->ref = 1;
    readp->off = 0;
    readp->pipe = pipe;
    readp->readable = 1;
    readp->writable = 0;
    /* (Final) TODO END */
}

void init_write_pipe(File *writep, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    writep->type = FD_PIPE;
    writep->ref = 1;
    writep->off = 0;
    writep->pipe = pipe;
    writep->readable = 0;
    writep->writable = 1;
    /* (Final) TODO END */
}

int pipe_alloc(File **f0, File **f1)
{
    /* (Final) TODO BEGIN */
    int success_flag = 0;
    Pipe *channel = 0;
    
    *f0 = file_alloc();
    if (!*f0) goto cleanup;
    
    *f1 = file_alloc();
    if (!*f1) goto cleanup;
    
    channel = kalloc(sizeof(Pipe));
    if (!channel) goto cleanup;
    
    init_pipe(channel);
    init_read_pipe(*f0, channel);
    init_write_pipe(*f1, channel);
    
    success_flag = 1;
    
cleanup:
    if (!success_flag) {
        if (channel) kfree((char *)channel);
        if (*f0) { file_close(*f0); *f0 = 0; }
        if (*f1) { file_close(*f1); *f1 = 0; }
        return -1;
    }
    
    return 0;
    /* (Final) TODO END */
}

void pipe_close(Pipe *pi, int writable)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    
    switch (writable) {
        case 1:
            pi->writeopen = 0;
            post_sem(&pi->rlock);
            break;
        case 0:
            pi->readopen = 0;
            post_sem(&pi->wlock);
            break;
    }
    
    if (pi->readopen == 0 && pi->writeopen == 0) {
        kfree((void *)pi);
    } else {
        release_spinlock(&pi->lock);
    }
    /* (Final) TODO END */
}

int pipe_write(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    int count = 0;
    char *source = (char *)addr;
    
    acquire_spinlock(&pi->lock);
    
    while (count < n) {
        if (!pi->readopen || thisproc()->killed) {
            release_spinlock(&pi->lock);
            return -1;
        }
        
        if (pi->nwrite == pi->nread + PIPE_SIZE) {
            post_sem(&pi->rlock);
            release_spinlock(&pi->lock);
            unalertable_wait_sem(&pi->wlock);
            acquire_spinlock(&pi->lock);
            continue;
        }
        
        pi->data[pi->nwrite++ % PIPE_SIZE] = source[count++];
    }
    
    post_sem(&pi->rlock);
    release_spinlock(&pi->lock);
    return count;
    /* (Final) TODO END */
}

int pipe_read(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    int obtained = 0;
    char *destination = (char *)addr;
    
    acquire_spinlock(&pi->lock);
    
    do {
        if (pi->nread != pi->nwrite || !pi->writeopen) {
            break;
        }
        
        if (thisproc()->killed) {
            release_spinlock(&pi->lock);
            return -1;
        }
        
        release_spinlock(&pi->lock);
        unalertable_wait_sem(&pi->rlock);
        acquire_spinlock(&pi->lock);
    } while (1);
    
    while (obtained < n && pi->nread != pi->nwrite) {
        destination[obtained++] = pi->data[pi->nread++ % PIPE_SIZE];
    }
    
    post_sem(&pi->wlock);
    release_spinlock(&pi->lock);
    return obtained;
    /* (Final) TODO END */
}