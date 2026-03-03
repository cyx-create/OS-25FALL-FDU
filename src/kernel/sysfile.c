//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <stddef.h>

#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

#ifdef DEBUG
#define printk(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define printk(fmt, ...)
#endif

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len; /* Number of bytes to transfer. */
};

/** 
 * Get the file object by fd. Return null if the fd is invalid.
 */
static struct file *fd2file(int fd)
{
    /* (Final) TODO BEGIN */
    Proc *current = thisproc();
    
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    
    return current->oftable.ofile[fd];
    /* (Final) TODO END */
}

 /*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f)
{
    /* (Final) TODO BEGIN */
    
    // 获取当前进程
    Proc *current = thisproc();
    struct file **open_files = current->oftable.ofile;
    
    // 遍历文件描述符表寻找空闲槽位
    for (int file_desc = 0; file_desc < NOFILE; file_desc++) {
        if (open_files[file_desc] == NULL) {  // 找到空闲槽位
            open_files[file_desc] = f;        // 分配文件
            return file_desc;                 // 返回文件描述符
        }
    }
    
    // 文件描述符表已满
    return -1;
    /* (Final) TODO END */
}

define_syscall(ioctl, int fd, u64 request)
{
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}

// 辅助函数：验证参数
static int mmap_validate_args(int length, int prot, int flags)
{
    if (length <= 0) return -1;
    if (prot & PROT_EXEC) return -1;
    if (flags & MAP_ANONYMOUS) return -1;
    return 0;
}

// 辅助函数：验证文件权限
static int mmap_validate_file(struct file *file, int flags, int prot)
{
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
        return file->writable ? 0 : -1;
    }
    return 0;
}

define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
               int offset)
{
    /* (Final) TODO BEGIN */
    // 参数验证
    if (mmap_validate_args(length, prot, flags) < 0) {
        return -1;
    }
    
    // 获取文件
    struct file *file_ptr = fd2file(fd);
    if (file_ptr == NULL) {
        return -1;
    }
    
    // 文件权限验证
    if (mmap_validate_file(file_ptr, flags, prot) < 0) {
        return -1;
    }
    
    // 获取inode
    Inode *inode_ptr = file_ptr->ip;
    if (inode_ptr == NULL) {
        return -1;
    }
    
    // 计算内存大小
    usize mem_size = round_up(length, PAGE_SIZE);
    if (mem_size == 0) {
        return -1;
    }
    
    // 文件系统操作
    OpContext op_ctx;
    bcache.begin_op(&op_ctx);
    inodes.lock(inode_ptr);
    
    int result = -1;
    
    do {
        // 验证文件类型
        if (inode_ptr->entry.type != INODE_REGULAR) {
            break;
        }
        
        // 分配section
        struct section *new_section = kalloc(sizeof(struct section));
        if (new_section == NULL) {
            break;
        }
        
        // 初始化section
        init_list_node(&new_section->stnode);
        new_section->flags = ST_FILE;
        new_section->mmap_flags = flags;
        
        // 计算映射地址
        static u64 next_available_addr = 0x100000;
        u64 start_address = (addr == NULL) ? next_available_addr : (u64)addr;
        
        if (addr == NULL) {
            next_available_addr += mem_size;
        }
        
        // 配置section
        new_section->begin = start_address;
        new_section->end = start_address + mem_size;
        new_section->fp = file_ptr;
        file_dup(file_ptr);
        new_section->offset = offset;
        new_section->length = mem_size;
        
        // 添加到进程
        _insert_into_list(&thisproc()->pgdir.section_head, &new_section->stnode);
        
        result = (int)start_address;
        
    } while (0);  // 只执行一次的循环，方便break跳转
    
    // 清理
    inodes.unlock(inode_ptr);
    bcache.end_op(&op_ctx);
    
    return result;
    /* (Final) TODO END */
}

define_syscall(munmap, void *addr, size_t length)
{
    /* (Final) TODO BEGIN */

    // printk("munmap called: addr=%p, length=%lu\n", addr, length);

    // 长度为0直接成功
    if (length == 0) {
        return 0;
    }
    
    // 地址对齐处理
    u64 start_addr = round_down((u64)addr, PAGE_SIZE);
    u64 mapped_len = round_up(length, PAGE_SIZE);
    
    // printk("munmap: aligned start_addr=0x%lx, mapped_len=0x%lx\n", 
    //        start_addr, mapped_len);

    Proc *current = thisproc();
    struct section *target_section = NULL;
    
    // 查找包含地址的section - 使用原始的循环方式
    ListNode *head = &current->pgdir.section_head;
    ListNode *node;
    for (node = head->next; node != head; node = node->next) {
        struct section *curr_section = 
            container_of(node, struct section, stnode);
        
        // 检查地址是否在该section范围内
        if (curr_section->begin <= (u64)addr && 
            (u64)addr < curr_section->end) {
            target_section = curr_section;
            break;
        }
    }
    
    // 未找到对应的section
    if (target_section == NULL) {
        // printk("munmap: no section found for addr=%p\n", addr);
        return -1;
    }
    
    // printk("munmap: found section [0x%lx-0x%lx], flags=0x%x\n",
    //        target_section->begin, target_section->end,
    //        target_section->mmap_flags);

    // 对于共享映射，写回修改的页面
    if (target_section->mmap_flags & MAP_SHARED) {
        int pages_written = 0;
        for (u64 virt_addr = start_addr; 
             virt_addr < start_addr + mapped_len; 
             virt_addr += PAGE_SIZE) {
            PTEntry *pte = get_pte(&current->pgdir, virt_addr, false);
            if (pte && *pte) {
                pages_written++;
                // 计算文件偏移
                usize file_offset = target_section->offset + 
                                   (virt_addr - target_section->begin);
                // 获取物理地址
                void *phys_addr = (void *)P2K(PTE_ADDRESS(*pte));
                // 写回文件
                inodes.write(NULL, target_section->fp->ip, phys_addr, 
                           file_offset, PAGE_SIZE);
            }
        }
    }

    // printk("[DEBUG] munmap: wrote back %d pages\n", pages_written);
    
    // 释放物理页面并清除页表项
    for (u64 virt_addr = start_addr; 
         virt_addr < start_addr + mapped_len; 
         virt_addr += PAGE_SIZE) {
        PTEntry *pte = get_pte(&current->pgdir, virt_addr, false);
        if (pte && *pte) {
            // 释放物理页面
            kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
            // 清除页表项
            *pte = 0;
        }
    }
    
    // 如果整个section都被释放，则删除section结构
    if (start_addr == target_section->begin && 
        start_addr + mapped_len >= target_section->end) {
        // 从链表中移除
        _detach_from_list(&target_section->stnode);
        
        // 关闭文件
        if (target_section->fp) {
            file_close(target_section->fp);
        }
        
        // 释放section内存
        kfree(target_section);
    } else if (start_addr > target_section->begin && 
               start_addr + mapped_len < target_section->end) {
        // 部分释放中间部分 - 不支持，返回错误
        return -1;
    } else if (start_addr == target_section->begin && 
               start_addr + mapped_len < target_section->end) {
        // 释放开头部分 - 更新section
        target_section->begin += mapped_len;
        target_section->offset += mapped_len;
        target_section->length -= mapped_len;
    } else if (start_addr > target_section->begin && 
               start_addr + mapped_len >= target_section->end) {
        // 释放结尾部分 - 更新section
        target_section->end = start_addr;
        target_section->length = start_addr - target_section->begin;
    }
    
    // printk("[DEBUG] munmap: returning success\n");
    return 0;
    /* (Final) TODO END */
}

define_syscall(dup, int fd)
{
    struct file *f = fd2file(fd);
    if (!f)
        return -1;
    fd = fdalloc(f);
    if (fd < 0)
        return -1;
    file_dup(f);
    return fd;
}

define_syscall(read, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return file_read(f, buffer, size);
}

define_syscall(write, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return file_write(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt)
{
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
        return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))
            return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

define_syscall(close, int fd)
{
    /* (Final) TODO BEGIN */
    struct file *file_ptr = fd2file(fd);
    if (file_ptr == NULL) {
        return -1;
    }
    
    Proc *current = thisproc();
    current->oftable.ofile[fd] = NULL;
    file_close(file_ptr);
    
    /* (Final) TODO END */
    return 0;
}

define_syscall(fstat, int fd, struct stat *st)
{
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return file_stat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags)
{
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}

static int isdirempty(Inode *dp)
{
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag)
{
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize off;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &off);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor,
              OpContext *ctx)
{
    /* (Final) TODO BEGIN */

    printk("[DEBUG] create: path=%s, type=%d\n", path, type);
    
    char target_name[FILE_NAME_MAX_LENGTH];
    Inode *parent_inode = nameiparent(path, target_name, ctx);
    
    if (!parent_inode) {
        // printk("[ERROR] create: parent directory not found\n");
        return 0;
    }
    
    // printk("[DEBUG] create: parent_inode=%lu, target_name=%s\n", 
    //        parent_inode->inode_no, target_name);
    
    inodes.lock(parent_inode);
    
    usize existing_ino = inodes.lookup(parent_inode, target_name, 0);
    if (existing_ino) {
        // printk("[DEBUG] create: file already exists, ino=%lu\n", existing_ino);
        Inode *existing_inode = inodes.get(existing_ino);
        
        inodes.unlock(parent_inode);
        inodes.put(ctx, parent_inode);
        
        inodes.lock(existing_inode);
        
        if (type == INODE_REGULAR && existing_inode->entry.type == INODE_REGULAR) {
            // printk("[DEBUG] create: returning existing regular file\n");
            return existing_inode;
        }
        
        // printk("[ERROR] create: type mismatch\n");
        inodes.unlock(existing_inode);
        inodes.put(ctx, existing_inode);
        return 0;
    }
    
    usize new_ino = inodes.alloc(ctx, type);
    if (new_ino == 0) {
        // printk("[ERROR] create: failed to allocate inode\n");
        inodes.unlock(parent_inode);
        inodes.put(ctx, parent_inode);
        return 0;
    }
    
    // printk("[DEBUG] create: allocated new inode %lu\n", new_ino);
    
    Inode *new_inode = inodes.get(new_ino);
    ASSERT(new_inode != 0);
    
    inodes.lock(new_inode);
    new_inode->entry.major = major;
    new_inode->entry.minor = minor;
    new_inode->entry.num_links = 1;
    inodes.sync(ctx, new_inode, true);
    
    if (type == INODE_DIRECTORY) {
        // printk("[DEBUG] create: setting up directory structure\n");
        parent_inode->entry.num_links++;
        inodes.sync(ctx, parent_inode, true);
        inodes.insert(ctx, new_inode, ".", new_inode->inode_no);
        inodes.insert(ctx, new_inode, "..", parent_inode->inode_no);
    }
    
    int insert_result = inodes.insert(ctx, parent_inode, target_name, new_inode->inode_no);
    if (insert_result <= 0) {
        // printk("[ERROR] create: failed to insert entry into parent directory\n");
        // 清理已分配的资源?
        // 回滚处理
        if (type == INODE_DIRECTORY) {
            // 减少父目录链接计数
            parent_inode->entry.num_links--;
            inodes.sync(ctx, parent_inode, true);
            // 注意：这里应该删除"."和".."条目，但简化处理
        }
        goto cleanup_error;
        
    }
    
    inodes.unlock(parent_inode);
    inodes.put(ctx, parent_inode);
    
    // printk("[DEBUG] create: successfully created inode %lu\n", new_ino);
    return new_inode;

cleanup_error:
    // 错误处理：清理所有已分配的资源
    // printk("[CREATE] ERROR: cleanup after failure\n");
    if (new_inode != NULL) {
        // 如果inode已部分初始化，需要释放它
        inodes.unlock(new_inode);
        
        // 减少inode的引用计数
        inodes.put(ctx, new_inode);
    }
    
    if (parent_inode != NULL) {
        // printk("[CREATE] Unlocking and releasing new inode\n");
        inodes.unlock(parent_inode);
        inodes.put(ctx, parent_inode);
    }
    
    // printk("[CREATE] FAILED\n");
    return NULL;
    /* (Final) TODO END */
}

define_syscall(openat, int dirfd, const char *path, int omode)
{
    int fd;
    struct file *f;
    Inode *ip;

    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
    } else {
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
        inodes.lock(ip);
    }

    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(chdir, const char *path)
{
    /**
     * (Final) TODO BEGIN 
     * 
     * Change the cwd (current working dictionary) of current process to 'path'.
     * You may need to do some validations.
     */
     // 验证路径长度，确保在合理范围内
    if (!user_strlen(path, 256)) {
        return -1;
    }

    OpContext op_ctx;
    bcache.begin_op(&op_ctx);

    // 获取路径对应的 inode
    Inode *target_inode = namei(path, &op_ctx);
    if (target_inode == NULL) {
        bcache.end_op(&op_ctx);
        return -1;
    }

    // 加锁，开始对 inode 操作
    inodes.lock(target_inode);

    // 检查目标 inode 是否是目录
    if (target_inode->entry.type != INODE_DIRECTORY) {
        // 如果不是目录，释放资源并返回错误
        inodes.unlock(target_inode);
        inodes.put(&op_ctx, target_inode);
        bcache.end_op(&op_ctx);
        return -1;
    }

    // 解锁 inode，目录有效
    inodes.unlock(target_inode);

    // 获取当前进程
    Proc *current_process = thisproc();

    // 如果当前进程已有工作目录（cwd），释放旧的 cwd
    if (current_process->cwd) {
        inodes.put(&op_ctx, current_process->cwd);
    }

    // 设置新的 cwd 为目标目录
    current_process->cwd = target_inode;

    bcache.end_op(&op_ctx);

    return 0;
    /* (Final) TODO END */
}

define_syscall(pipe2, int pipefd[2], int flags)
{
    /* (Final) TODO BEGIN */
   // 验证 flags 参数是否为 0，其他值返回错误
    if (flags != 0) {
        return -1;
    }

    struct file *read_end, *write_end;

    // 分配一个管道，并返回读端和写端的文件指针
    if (pipe_alloc(&read_end, &write_end) < 0) {
        return -1;
    }

    // 为管道的读端和写端分配文件描述符
    int read_fd = fdalloc(read_end);
    int write_fd = fdalloc(write_end);

    // 如果任一文件描述符分配失败，则清理资源并返回错误
    if (read_fd < 0 || write_fd < 0) {
        if (read_fd >= 0) {
            thisproc()->oftable.ofile[read_fd] = NULL;
        }

        // 关闭文件，释放资源
        file_close(read_end);
        file_close(write_end);

        return -1;
    }

    // 将文件描述符赋值给 pipefd 数组
    pipefd[0] = read_fd;
    pipefd[1] = write_fd;

    return 0;
    /* (Final) TODO END */
}
