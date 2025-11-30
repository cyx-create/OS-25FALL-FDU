#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device; 

/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;

static LogHeader header; // in-memory copy of log header block.

/**
    @brief a struct to maintain other logging states.
    
    You may wonder where we store some states, e.g.
    
    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */

// 全局缓存块计数
static usize num_cached_blocks = 0;

struct {
    /* your fields here */
    usize running_ops;        // 正在运行的事务数量
    bool checkpointing;       // 是否正在执行checkpoint
    Semaphore op_sem;         // 控制并发事务的信号量
    usize next_ts;           // 下一个时间戳
    SpinLock glock;
} log;

// read the content from disk.
static INLINE void device_read(Block *block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8 *)&header);
}

static Block *find_cached_block(usize block_no) {
    ListNode *p;
    usize count = 0;
    
    _for_in_list(p, &head) {
        if (p == &head) {
            continue;
        }
        Block *curr = container_of(p, Block, node);
        count++;
        if (curr->block_no == block_no) {
            // printk("FOUND: block %d at position %d\n", block_no, count);
            return curr;
        }
    }
    // printk("NOT FOUND: block %d, searched %d cached blocks\n", block_no, count);
    return NULL;
}

static Block *evict_block() {
    ListNode *p = head.prev;  // 从尾部开始（LRU）
    while (p != &head) {
                if(p == &head || num_cached_blocks < EVICTION_THRESHOLD){
                break;
            }
        Block *curr = container_of(p, Block, node);
        ListNode *prev = p->prev;  // 保存前一个节点
        
        if (!curr->acquired && !curr->pinned) {
            // 如果是脏块，需要写回磁盘
            // if (curr->valid) {
            //     acquire_sleeplock(&curr->lock);
            //     device_write(curr);
            //     release_sleeplock(&curr->lock);
            // }

            num_cached_blocks--;
            _detach_from_list(&curr->node);
            kfree(curr);
            return NULL;  // 返回NULL表示成功淘汰，但需要重新分配

        }
        p = prev;
    }
    return NULL;  // 没有可淘汰的块
}


// initialize a block struct.
static void init_block(Block *block) {
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// see `cache.h`.
static usize get_num_cached_blocks() {
    // TODO
    return num_cached_blocks;
}

// see `cache.h`.
static Block *cache_acquire(usize block_no) {
    // TODO

    acquire_spinlock(&lock);

    Block *blk = find_cached_block(block_no);
    
    // 如果块在缓存中
    if (blk) {
        // printk("CACHE HIT: block %d\n", block_no);
        // 更新LRU：移动到头部
        _detach_from_list(&blk->node);
        _insert_into_list(&head, &blk->node);
        blk->acquired = true;
        release_spinlock(&lock);
        
        // 获取块锁
        acquire_sleeplock(&blk->lock);
        
        // 确保数据有效
        if (!blk->valid) {
            device_read(blk);
            blk->valid = true;
        }
        return blk;
    }
    else{
        // printk("CACHE MISS: block %d, cached blocks: %d\n", block_no, num_cached_blocks);
    }
    
    // 块不在缓存中，需要分配

    if (num_cached_blocks >= EVICTION_THRESHOLD) {
        // 尝试淘汰块
        evict_block();
        // 注意：evict_block释放了空间，但我们需要重新分配
    }
    
    // 分配新块
    blk = kalloc(sizeof(Block));
    if (!blk) {
        release_spinlock(&lock);
        return NULL;
    }
    
    init_block(blk);
    blk->block_no = block_no;
    blk->acquired = true;
    _insert_into_list(&head, &blk->node);
    num_cached_blocks++;
    
    release_spinlock(&lock);
    
    // 获取锁并读取数据
    acquire_sleeplock(&blk->lock);
    device_read(blk);
    blk->valid = true;
    return blk;
}

// see `cache.h`.
static void cache_release(Block *block) {
    // TODO
    if (!block) return;
    
    // release_sleeplock(&block->lock);
    
    acquire_spinlock(&lock);
    block->acquired = false;
    post_sem(&block->lock);
    release_spinlock(&lock);
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx) {
    // TODO
    if (!ctx) PANIC();

    acquire_spinlock(&log.glock);

    // 等待直到可以开始新事务
    while(log.checkpointing || (header.num_blocks + (log.running_ops + 1) * OP_MAX_NUM_BLOCKS) > LOG_MAX_SIZE){
        release_spinlock(&log.glock);

        // 等待信号量（可能被kill打断）
        unalertable_wait_sem(&log.op_sem);
        acquire_spinlock(&log.glock);
    }

    log.running_ops++;
    ctx->rm = OP_MAX_NUM_BLOCKS;
    ctx->ts = log.next_ts++; 

    release_spinlock(&log.glock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block) {
    // TODO
    if (!ctx) {
        // 直接写入磁盘
        device_write(block);
        return;
    }
    acquire_spinlock(&log.glock);

    // 标记块为脏（固定）
    block->pinned = true;

    // 检查是否已在日志中
    for(usize i = 0; i < header.num_blocks; i++){
        if(block->block_no == header.block_no[i]){
            // 块已在日志中，更新对应的日志数据块，实际上不要求
            // usize log_block_no = sblock->log_start + 1 + i;
            // device->write(log_block_no, block->data);
            release_spinlock(&log.glock);
            return;
        }
    }

    // 如果不在日志中，添加到日志

    if(ctx->rm <= 0 || header.num_blocks >= LOG_MAX_SIZE){
        release_spinlock(&log.glock);
        PANIC();
    }

    // 记录块号到日志头
    header.block_no[header.num_blocks] = block->block_no;
    header.num_blocks++;

    // 将块数据写入日志区域，实际上不要求
    // usize log_block_no = sblock->log_start + 1 + header.num_blocks;
    // device->write(blog_block_no, block->data);
    // 更新日志头到磁盘
    // write_header();

    ctx->rm--;
    release_spinlock(&log.glock);
}

//执行checkpoint操作的新函数
static void do_checkpoint(void) {
    // 标识正在执行 checkpoint
    log.checkpointing = 1;

    // 将日志区中的内容逐块回写到其原始位置
    for (usize idx = 0; idx < header.num_blocks; idx++) {
        Block *src_block = NULL;
        Block *log_block = NULL;

        // cache_acquire 内会处理自己的锁，因此需暂时释放 glock
        release_spinlock(&log.glock);

        src_block = cache_acquire(header.block_no[idx]);
        log_block = cache_acquire(sblock->log_start + 1 + idx);

        // 数据复制
        for (usize k = 0; k < BLOCK_SIZE; k++)
            log_block->data[k] = src_block->data[k];

        cache_sync(NULL, log_block);

        cache_release(src_block);
        cache_release(log_block);

        acquire_spinlock(&log.glock);
    }

    // 更新日志头
    release_spinlock(&log.glock);
    write_header();
    acquire_spinlock(&log.glock);

    // 从原始块重新写回磁盘并解除 pinned
    for (usize i = 0; i < header.num_blocks; i++) {
        Block *blk = cache_acquire(header.block_no[i]);
        cache_sync(NULL, blk);
        blk->pinned = 0;
        cache_release(blk);
    }

    header.num_blocks = 0;

    // 第二次写出 header，使日志真正被清空
    release_spinlock(&log.glock);
    write_header();
    acquire_spinlock(&log.glock);

    // checkpoint 完成，恢复标志
    log.checkpointing = 0;

    // 唤醒所有等待进程
    post_all_sem(&log.op_sem);

    release_spinlock(&log.glock);
}



// see `cache.h`.
static void cache_end_op(OpContext *ctx) {
    // TODO
    if (!ctx) PANIC();

    acquire_spinlock(&log.glock);
    if(log.checkpointing){
        PANIC();
    }
    log.running_ops--;

    // 如果是最后一个运行的事务且日志不为空，执行checkpoint
    if (log.running_ops == 0 ) {
        do_checkpoint();
    }
    else{
        // 等待当前checkpoint完成
        while (log.checkpointing) {
            release_spinlock(&log.glock);
            wait_sem(&log.op_sem);
            acquire_spinlock(&log.glock);
        }
        post_sem(&log.op_sem);
        release_spinlock(&log.glock);
        return;

    }

}

static SpinLock bitmap_lock;
// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
    sblock = _sblock;
    device = _device;

    // TODO
    init_spinlock(&lock);
    init_spinlock(&log.glock);
    init_spinlock(&bitmap_lock);
    init_list_node(&head);
    num_cached_blocks = 0;
    header.num_blocks = 0;

    // 初始化日志系统
    log.running_ops = 0;
    log.checkpointing =  false;
    log.next_ts = 1;
    init_sem(&log.op_sem, 0);

    // 读取日志头
    read_header();

    if (header.num_blocks == 0) {
        return;
    }
    if (header.num_blocks > 0) {
        // printk("Recovering %d blocks from log\n", header.num_blocks);
        // 从日志区读取，覆盖回内存里的缓存块
        for(usize i = 0; i < header.num_blocks; i++){
        Block* log_blk = cache_acquire(sblock->log_start + 1 + i);
        Block* orig_blk = cache_acquire(header.block_no[i]);

        for (usize j = 0; j < BLOCK_SIZE; ++j) {
            orig_blk->data[j] = log_blk->data[j];
        }

        cache_sync(NULL, orig_blk);
        cache_release(log_blk);
        cache_release(orig_blk);
        }

        // 清空日志
        header.num_blocks = 0;
        memset(header.block_no, 0, sizeof(header.block_no));
        write_header();
    }
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx) {
    if (!ctx) PANIC();

    acquire_spinlock(&bitmap_lock);

    usize total_blocks = sblock->num_blocks;
    usize blocks_per_bitmap = BLOCK_SIZE * 8;

    for (usize start = 0; start < total_blocks; start += blocks_per_bitmap) {
        usize bitmap_index = sblock->bitmap_start + start / blocks_per_bitmap;
        Block *bitmap = cache_acquire(bitmap_index);

        usize limit = (start + blocks_per_bitmap < total_blocks) ? start + blocks_per_bitmap : total_blocks;

        for (usize blk = start; blk < limit; blk++) {
            usize byte_idx = (blk - start) / 8;
            usize bit_idx  = (blk - start) % 8;

            if (!(bitmap->data[byte_idx] & (1 << bit_idx))) {
                // 标记该块为已分配
                bitmap->data[byte_idx] |= (1 << bit_idx);
                cache_sync(ctx, bitmap);
                cache_release(bitmap);

                // 清空数据块内容
                Block *new_blk = cache_acquire(blk);
                memset(new_blk->data, 0, BLOCK_SIZE);
                cache_sync(ctx, new_blk);
                cache_release(new_blk);

                release_spinlock(&bitmap_lock);
                return blk;
            }
        }

        cache_release(bitmap);
    }

    release_spinlock(&bitmap_lock);
    PANIC(); // 没有空闲块
    return 0;
}


// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no) {
    if (!ctx) PANIC();

    acquire_spinlock(&bitmap_lock);

    usize bitmap_block = sblock->bitmap_start + block_no / (BLOCK_SIZE * 8);
    Block* map_blk = cache_acquire(bitmap_block);
    usize map_byte = (block_no % (BLOCK_SIZE * 8)) / 8;
    usize map_bit  = (block_no % (BLOCK_SIZE * 8)) % 8;

    // 安全清除位
    map_blk->data[map_byte] &= ~(1 << map_bit);

    cache_sync(ctx, map_blk);
    cache_release(map_blk);
    release_spinlock(&bitmap_lock);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};