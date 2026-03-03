#include <driver/virtio.h>
#include <fs/block_device.h>
#include <common/string.h>
#include <kernel/printk.h>

/**
    @brief a simple implementation of reading a block from SD card.

    @param[in] block_no the block number to read
    @param[out] buffer the buffer to store the data
 */

 //我的磁盘，第一个分区是内核代码之类的系统文件，从0到133119块，也就是133120*512=65MB，在inode里面搞清楚
#define BLOCKNO_OFFSET 0x20800  

static void sd_read(usize block_no, u8 *buffer) {
    Buf b;
    b.block_no = (u32)block_no + BLOCKNO_OFFSET;
    // b.block_no = (u32)block_no;
    b.flags = 0;
    virtio_blk_rw(&b);
    memcpy(buffer, b.data, BLOCK_SIZE);
}

/**
    @brief a simple implementation of writing a block to SD card.

    @param[in] block_no the block number to write
    @param[in] buffer the buffer to store the data
 */
static void sd_write(usize block_no, u8 *buffer) {
    Buf b;
    b.block_no = (u32)block_no + BLOCKNO_OFFSET;
    // b.block_no = (u32)block_no;
    b.flags = B_DIRTY | B_VALID;
    memcpy(b.data, buffer, BLOCK_SIZE);
    virtio_blk_rw(&b);
}

/**
    @brief the in-memory copy of the super block.

    We may need to read the super block multiple times, so keep a copy of it in
    memory.

    @note the super block, in our lab, is always read-only, so we don't need to
    write it back.
 */
static u8 sblock_data[BLOCK_SIZE];

BlockDevice block_device;

void init_block_device() {
    block_device.read = sd_read;
    block_device.write = sd_write;

    // new
    block_device.read(1, sblock_data);
}

const SuperBlock *get_super_block() { return (const SuperBlock *)sblock_data; }