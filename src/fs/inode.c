#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock* sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache* cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static ListNode head;


// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);

    // TODO
    //尝试加锁，但是好像不加锁也行……
    acquire_spinlock(&lock);

    for(usize inode_no = 1; inode_no < sblock->num_inodes; inode_no++) {
        usize block_no = to_block_no(inode_no);
        Block* block = cache->acquire(block_no);
        if (!block) continue;

        InodeEntry* entry = get_entry(block, inode_no);

        if(entry->type == INODE_INVALID) {
            // 找到空闲 inode，清零并设置类型
            memset(entry, 0, sizeof(InodeEntry));
            entry->type = type;
            cache->release(block);
            //下面这行按理需要，但是有没有不影响通过
            // cache->sync(ctx, block);

            release_spinlock(&lock);

            return inode_no;
        }
        cache->release(block);
        // release_spinlock(&lock);
    }
    release_spinlock(&lock);

    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO

    // 持有 inode 自身的睡眠锁
    acquire_sleeplock(&inode->lock);

    // 如果 inode 无效，需要从磁盘读取 inode_entry
    if (!inode->valid) {
        // 如果 inode 还没加载，需要从磁盘读取
        Block *block = cache->acquire(to_block_no(inode->inode_no));
        inode->entry = *get_entry(block, inode->inode_no);
        cache->release(block);
        inode->valid = true;
    }
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO

    // 释放睡眠锁
    // 这允许其他等待该inode的进程获取锁
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    // TODO

    ASSERT(inode != NULL);
    ASSERT(inode->inode_no < sblock->num_inodes);

    // 根据 inode 号找到所在的块
    usize block_no = to_block_no(inode->inode_no);
    Block* block = cache->acquire(block_no);

    if (!block) {
        PANIC();
    }

    // 找到对应的 inode entry
    InodeEntry* entry = get_entry(block, inode->inode_no);

        if (do_write) {
        // 将 inode.entry 写入 block 缓存
        memcpy(entry, &inode->entry, sizeof(InodeEntry));
    } 
    else {
        // 从 block 读取 inode.entry
        memcpy(&inode->entry, entry, sizeof(InodeEntry));
        inode->valid = true;
    }

    cache->release(block);

}

// see `inode.h`.
static Inode* inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);
    // TODO

    // 1. 在缓存列表中查找
    for (ListNode* n = head.next; n != &head; n = n->next) {
        Inode* ino = container_of(n, Inode, node);

        // 找到了缓存的 inode
        if (ino->inode_no == inode_no) {
            increment_rc(&ino->rc); // 增加引用计数
            release_spinlock(&lock);  // 立即释放全局锁
            return ino;
        }
    }

    // 2. 未找到 → 分配新的内存 inode
    Inode* fresh = kalloc(sizeof(Inode));
    if (!fresh) { 
        release_spinlock(&lock);
        return NULL; //// 内存不足（一般不会发生）
    }

    // 初始化新 inode 结构
    init_inode(fresh);
    fresh->inode_no = inode_no;
    // 新取出的 inode 第一次引用
    increment_rc(&fresh->rc);

    // 3. 先把新 inode 插入缓存链表
    //    （因为这部分是元数据，需要全局锁保护）
    _insert_into_list(&head, &fresh->node);

    // 全局锁挂不住 IO，需要尽快释放
    release_spinlock(&lock);

    // 4. 在锁外进行 inode 内容同步（从磁盘加载）
    //    这一步可能会阻塞，因此绝不能在锁内做！
    inode_sync(NULL, fresh, /*write*/ false);

    return fresh;
}
// see `inode.h`.
static void inode_clear(OpContext* ctx, Inode* inode) {
    // TODO
    InodeEntry* e = &inode->entry;

    // 1. 清理直接块 
    for (usize i = 0; i < INODE_NUM_DIRECT; i++) {
        u32 blk = e->addrs[i];
        if (blk != 0) {
            cache->free(ctx, blk);
            e->addrs[i] = 0;               // 更新内存状态
        }
    }

    // 2. 清理间接块
    u32 indirect_blk = e->indirect;
    if (indirect_blk != 0) {

        // 读取 indirect block
        Block* ib = cache->acquire(indirect_blk);

        // 每个条目都是一个 u32 块号
        u32* table = (u32*)ib->data;

        // 间接块容量由磁盘块大小决定，不使用固定值
        usize cap = BLOCK_SIZE / sizeof(u32);

        // 清理所有间接引用的块
        for (usize i = 0; i < cap; i++) {
            if (table[i] != 0) {
                cache->free(ctx, table[i]);
                table[i] = 0;              // 清空
            }
        }

        cache->release(ib);

        // 最后释放整个 indirect block
        cache->free(ctx, indirect_blk);
        e->indirect = 0;
    }

    // 3. 重置文件大小
    e->num_bytes = 0;

    // 4. 更新到磁盘
    inode_sync(ctx, inode, /*do_write=*/true);
}

// see `inode.h`.
static Inode* inode_share(Inode* inode) {
    // TODO
    ASSERT(inode != NULL);

    // 原子地增加引用计数
    acquire_spinlock(&lock);
    inode->rc.count++;
    release_spinlock(&lock);

    // 返回同一个 inode 指针
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext* ctx, Inode* inode) {
    // TODO
    // ASSERT(inode != NULL);
    // // 获取 inode 锁，防止并发操作
    // inode_lock(inode);
    // // 原子减少引用计数
    // bool rc_zero = decrement_rc(&inode->rc);
    // // 引用计数不为0，直接释放锁返回
    // if (!rc_zero) {
    //     inode_unlock(inode);
    //     return;
    // }
    // // RC 为0，检查硬链接
    // if (inode->entry.num_links == 0) {
    //     // 清理 inode 数据块
    //     inode_clear(ctx, inode);
    //     // 从全局 inode 链表移除
    //     acquire_spinlock(&lock);
    //     _detach_from_list(&inode->node);
    //     release_spinlock(&lock);
    //     // 标记 inode 无效
    //     inode->valid = false;
    //     inode->inode_no = 0;
    //     // 释放 inode 锁后释放内存
    //     inode_unlock(inode);
    //     kfree(inode);
    //     return;
    // }
    // // RC 为0，但仍有硬链接，只释放 inode 锁
    // inode_unlock(inode);

    //new try
    ASSERT(inode != NULL);

    // 步骤 1：获取 inode 锁，确保操作不可打断
    unalertable_wait_sem(&inode->lock);

    // 步骤 2：先保存引用计数的旧值
    usize old_rc = inode->rc.count;

    // 步骤 3：原子减少引用计数
    decrement_rc(&inode->rc);

    // 步骤 4：检查 inode 是否需要清理
    bool need_clear = (inode->rc.count == 0) && (inode->entry.num_links == 0);

    if (need_clear) {
        // 步骤 4a：将 inode 标记为无效
        inode->entry.type = INODE_INVALID;

        // 步骤 4b：清理 inode 占用的数据块
        inode_clear(ctx, inode);

        // 步骤 4c：同步 inode 元数据
        inode_sync(ctx, inode, true);

        // 步骤 4d：移除 inode 链表节点
        acquire_spinlock(&lock);
        _detach_from_list(&inode->node);
        release_spinlock(&lock);

        // 步骤 4e：释放 inode 锁
        post_sem(&inode->lock);

        // 步骤 4f：释放 inode 内存
        kfree(inode);
    } else {
        // 步骤 5：不需要清理，只释放 inode 锁
        post_sem(&inode->lock);
    }
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.
    
    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
static usize inode_map(OpContext* ctx,
                       Inode* inode,
                       usize offset,
                       bool* modified) {
    // TODO
    ASSERT(inode != NULL);

    // 计算文件中块的索引
    usize block_idx = offset / BLOCK_SIZE;
    usize blk_no = 0;

    // 处理直接块
    if (block_idx < INODE_NUM_DIRECT) {
        blk_no = inode->entry.addrs[block_idx];
        if (blk_no == 0 && ctx != NULL) {
            // 分配新的直接块
            blk_no = cache->alloc(ctx);
            inode->entry.addrs[block_idx] = blk_no;
            if (modified) *modified = true;
            // 需要同步！！！！
            inode_sync(ctx, inode, true);
        }
        return blk_no;
    }

    // 处理间接块
    block_idx -= INODE_NUM_DIRECT;

    if (inode->entry.indirect == 0) {
        if (ctx == NULL) return 0;
        // 分配间接块
        inode->entry.indirect = cache->alloc(ctx);
        if (modified) *modified = true;
        // 初始化间接块内容
        Block* ind_blk = cache->acquire(inode->entry.indirect);
        memset(ind_blk->data, 0, BLOCK_SIZE);
        // 如果不需要同步，可以注释掉下面一行
        // cache->sync(ctx, ind_blk);
        cache->release(ind_blk);
    }

    // 获取间接块并访问对应地址
    Block* ind_blk = cache->acquire(inode->entry.indirect);
    u32* addrs = get_addrs(ind_blk);

    blk_no = addrs[block_idx];
    if (blk_no == 0 && ctx != NULL) {
        blk_no = cache->alloc(ctx);
        addrs[block_idx] = blk_no;
        if (modified) *modified = true;
        // 需要同步！！！
        // cache->sync(ctx, ind_blk);
    }

    cache->release(ind_blk);
    return blk_no;

}

// see `inode.h`.
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);

    // TODO
    usize read_bytes = 0;

    while (read_bytes < count) {
        usize file_offset = offset + read_bytes;
        usize block_idx = file_offset / BLOCK_SIZE;
        usize block_offset = file_offset % BLOCK_SIZE;
        usize remaining = count - read_bytes;
        bool modified = false;

        // 获取块号，不分配新块（ctx=NULL）
        usize blk_no = inode_map(NULL, inode, file_offset, &modified);
        if (blk_no == 0) break;  // 块未分配，无法读取

        Block* blk = cache->acquire(blk_no);
        usize to_copy = BLOCK_SIZE - block_offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy(dest + read_bytes, blk->data + block_offset, to_copy);
        cache->release(blk);

        read_bytes += to_copy;
    }

    return read_bytes;
}

// see `inode.h`.
static usize inode_write(OpContext* ctx,
                         Inode* inode,
                         u8* src,
                         usize offset,
                         usize count) {
    InodeEntry* entry = &inode->entry;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    usize written = 0;

    while (written < count) {
        usize file_offset = offset + written;
        usize block_idx = file_offset / BLOCK_SIZE;
        usize block_offset = file_offset % BLOCK_SIZE;
        usize remaining = count - written;
        bool modified = false;

        // 获取块号，如果未分配则分配新块
        usize blk_no = inode_map(ctx, inode, file_offset, &modified);
        if (blk_no == 0) PANIC();

        Block* blk = cache->acquire(blk_no);

        usize to_copy = BLOCK_SIZE - block_offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy(blk->data + block_offset, src + written, to_copy);

        cache->sync(ctx, blk);  // 同步到日志
        cache->release(blk);

        written += to_copy;
    }

    // 更新文件大小
    if (offset + written > entry->num_bytes) {
        entry->num_bytes = offset + written;
        inode_sync(ctx, inode, true);
    }

    return written;
}

// see `inode.h`.
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    usize num_entries = entry->num_bytes / sizeof(DirEntry); // 目录条目数量
    DirEntry dir_entry;

    for (usize i = 0; i < num_entries; i++) {
        bool modified = false;
        usize block_no = inode_map(NULL, inode, i * sizeof(DirEntry), &modified);
        if (block_no == 0) continue; // 块未分配，跳过

        Block* blk = cache->acquire(block_no);
        usize offset_in_block = (i * sizeof(DirEntry)) % BLOCK_SIZE;

        memcpy(&dir_entry, blk->data + offset_in_block, sizeof(DirEntry));
        cache->release(blk);

        if (strcmp(dir_entry.name, name) == 0) {
            if (index) *index = i;
            return dir_entry.inode_no;
        }
    }

    return 0; // 未找到
}

// see `inode.h`.
static usize inode_insert(OpContext* ctx,
                          Inode* inode,
                          const char* name,
                          usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
   // 1. 检查是否已存在
    usize existing_index;
    if (inode_lookup(inode, name, &existing_index) != 0) {
        return existing_index; // 文件已存在，返回其索引
    }

    // 2. 找到插入位置
    usize index = entry->num_bytes / sizeof(DirEntry); // 默认在末尾插入
    bool modified;
    usize block_no = inode_map(ctx, inode, index * sizeof(DirEntry), &modified);
    ASSERT(block_no != 0);

    Block* blk = cache->acquire(block_no);
    usize offset_in_block = (index * sizeof(DirEntry)) % BLOCK_SIZE;

    // 3. 构造新条目
    DirEntry new_entry;
    memset(&new_entry, 0, sizeof(DirEntry));
    new_entry.inode_no = inode_no;
    strncpy(new_entry.name, name, FILE_NAME_MAX_LENGTH);
    new_entry.name[FILE_NAME_MAX_LENGTH - 1] = '\0';

    // 写入到块
    memcpy(blk->data + offset_in_block, &new_entry, sizeof(DirEntry));
    cache->sync(ctx, blk);
    cache->release(blk);

    // 4. 更新目录 inode 大小并同步
    entry->num_bytes += sizeof(DirEntry);
    inode_sync(ctx, inode, true);

    return index; // 返回新插入的条目索引
}

// see `inode.h`.
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    // TODO
    DirEntry curr;
    inode_read(inode, (u8*)&curr, index, sizeof(DirEntry));
    curr.inode_no = 0;
    inode_write(ctx, inode, (u8*)&curr, index, sizeof(DirEntry));
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};

/**
    @brief read the next path element from `path` into `name`.
    
    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example 
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char* skipelem(const char* path, char* name) {
    const char* s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.
    
    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is true), 
    or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode* namex(const char* path,
                    bool nameiparent,
                    char* name,
                    OpContext* ctx) {
    /* (Final) TODO BEGIN */
    
    /* (Final) TODO END */
    return 0;
}

Inode* namei(const char* path, OpContext* ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode* nameiparent(const char* path, char* name, OpContext* ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode* ip, struct stat* st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
        case INODE_REGULAR:
            st->st_mode = S_IFREG;
            break;
        case INODE_DIRECTORY:
            st->st_mode = S_IFDIR;
            break;
        case INODE_DEVICE:
            st->st_mode = 0;
            break;
        default:
            PANIC();
    }
}