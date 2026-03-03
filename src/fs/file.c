#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <fs/pipe.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable.

    init_spinlock(&ftable.flock);
        for (int i = 0; i < NFILE; i++) {
        File* f = &ftable.flist[i];
        
        // 初始化文件对象状态
        f->type = FD_NONE;
        f->ref = 0;
        f->readable = false;
        f->writable = false;
        f->pipe = NULL;
        f->ip = NULL;
        f->off = 0;
    }
    // printk("[FS] File table initialized with %d entries\n", NFILE);

}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process.

    // 1. 清空所有文件描述符条目
    for (usize i = 0; i < NOFILE; ++i) {
        oftable->ofile[i] = NULL;
    }
}

/* Allocate a file structure. */
struct file* file_alloc() {
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.flock);  // 假设锁字段名是flock
    
    for (int i = 0; i < NFILE; i++) {
        if (ftable.flist[i].ref == 0) {  // 假设数组字段名是flist
            // 找到空闲文件对象，初始化它
            struct file* f = &ftable.flist[i];
            
            // 清空或初始化文件对象
            f->ref = 1;                // 设置引用计数为1
            f->type = FD_NONE;         // 初始类型为无
            f->readable = false;       // 默认不可读
            f->writable = false;       // 默认不可写
            f->pipe = NULL;            // 清空管道指针
            f->ip = NULL;              // 清空inode指针
            f->off = 0;                // 偏移量归零
            
            release_spinlock(&ftable.flock);
            return f;
        }
    }
    
    release_spinlock(&ftable.flock);

    /* (Final) TODO END */
    return 0;
}

/* Increment ref count for file f. */
struct file* file_dup(struct file* f) {
    /* (Final) TODO BEGIN */

    if (!f) return NULL;
    acquire_spinlock(&ftable.flock);

    f->ref++;  // 增加引用计数
    release_spinlock(&ftable.flock);

    /* (Final) TODO END */
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file* f) {
    /* (Final) TODO BEGIN */
    if (!f) return;
    
    acquire_spinlock(&ftable.flock);  // 假设锁字段名是lock，不是flock
    
    // 验证引用计数
    if (f->ref < 1) {
        release_spinlock(&ftable.flock);
        // printk("[FILE ERROR] file_close: ref count is already 0 or negative\n");
        return;
    }
    
    // 减少引用计数
    f->ref--;
    
    // 如果还有引用，直接返回
    if (f->ref > 0) {
        release_spinlock(&ftable.flock);
        return;
    }
    
    // 引用计数为0，需要真正关闭文件
    // 保存需要的信息（在释放锁之前）
    int file_type = f->type;
    struct pipe* pipe_ptr = f->pipe;
    Inode* ip_ptr = f->ip;
    bool writable = f->writable;
    
    // 清理文件对象（标记为未使用）
    f->type = FD_NONE;
    f->pipe = NULL;
    f->ip = NULL;
    f->readable = false;
    f->writable = false;
    f->off = 0;
    
    release_spinlock(&ftable.flock);
    
    // 根据文件类型执行不同的关闭操作
    switch (file_type) {
        case FD_PIPE:
            if (pipe_ptr) {
                pipe_close(pipe_ptr, writable);
            }
            break;
            
        case FD_INODE:
            if (ip_ptr) {
                OpContext ctx;
                bcache.begin_op(&ctx);   // 开始文件系统操作
                inodes.put(&ctx, ip_ptr); // 释放inode引用
                bcache.end_op(&ctx);     // 结束文件系统操作
            }
            break;
            
        case FD_NONE:
            // 已经是空文件，什么都不做
            break;
            
        default:
            // printk("[FILE WARN] file_close: unknown file type %d\n", file_type);
            break;
    }
    /* (Final) TODO END */
}

/* Get metadata about file f. */
int file_stat(struct file* f, struct stat* st) {
    /* (Final) TODO BEGIN */
    // 基本参数检查
    if (!f || !st) { return -1;}
    
    // 只有INODE类型文件支持stat
    if (f->type != FD_INODE) { return -1;}
    
    // inode指针检查
    if (!f->ip) { return -1;}
    
    // 获取stat信息
    inodes.lock(f->ip);
    stati(f->ip, st);
    inodes.unlock(f->ip);
    
    return 0;
    /* (Final) TODO END */
}

/* Read from file f. */
isize file_read(struct file* f, char* addr, isize n) {
    /* (Final) TODO BEGIN */

    if (f->readable == 0) return -1;
    
    switch (f->type) {
        case FD_PIPE:
            return pipe_read(f->pipe, (u64)addr, n);
        
        case FD_INODE: {
            usize r;
            inodes.lock(f->ip);
            r = inodes.read(f->ip, (u8*)addr, f->off, n);
            f->off = f->off + r;
            inodes.unlock(f->ip);
            return r;
        }
        
        default:
            PANIC();
            return 0;
    }
    /* (Final) TODO END */
    // return 0;
}

/* Write to file f. */
isize file_write(struct file* f, char* addr, isize n) {
    /* (Final) TODO BEGIN */
    if (!f->writable)
        return -1;

    // 如果是管道文件，调用管道的写入方法
    if (f->type == FD_PIPE)
        return pipe_write(f->pipe, (u64)addr, n);

    // 如果是inode文件，执行写入操作
    if (f->type == FD_INODE) {
        // 计算每次写入的最大字节数，基于操作的最大块数和inode结构的限制
        isize maxbytes = ((OP_MAX_NUM_BLOCKS - 4) / 2) * BLOCK_SIZE;  // 每次写入的最大字节数
        isize idx = 0;  // 当前写入的位置

        // 循环写入数据，直到写入完成
        while (idx < n) {
            // 计算当前需要写入的数据长度，确保不超过最大字节数
            isize len = MIN(n - idx, maxbytes);

            // 开始一个新的操作上下文
            OpContext ctx;
            bcache.begin_op(&ctx);  // 启动操作

            // 锁定inode并进行写入
            inodes.lock(f->ip);
            isize bytes_written = inodes.write(&ctx, f->ip, (u8 *)(addr + idx), f->off, len);

            // 更新文件偏移量
            f->off += bytes_written;
            inodes.unlock(f->ip);  // 解锁inode

            // 结束操作
            bcache.end_op(&ctx);

            // 如果写入的字节数不等于预期的字节数，返回错误
            if (bytes_written != len) {
                return -1;
            }

            idx += bytes_written;  // 更新写入的字节数
        }

        // 如果所有字节都成功写入，返回写入的总字节数
        if (idx == n) {
            return n;
        }
    }

    // 如果文件类型不支持写入，触发错误
    PANIC();
    /* (Final) TODO END */
    return 0;
}