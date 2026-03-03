#include <kernel/console.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <driver/uart.h>
#include <common/string.h>
#include <driver/interrupt.h>

struct console cons;
#define LINES 32  // Define the number of lines
#define IBUF_SIZE 128  // Define the buffer size 
#define BACKSPACE 0xff

void console_init()
{
    /* (Final) TODO BEGIN */
    // 初始化自旋锁
    init_spinlock(&cons.lock);
    
    // 初始化信号量，初始值为 0
    init_sem(&cons.sem, 0);

    // 初始化控制台缓冲区
    for (usize i = 0; i < IBUF_SIZE; i++) {
        cons.buf[i] = 0;  // 清空缓冲区，防止旧数据干扰
    }

    // 初始化索引
    cons.read_idx = 0;   // 读索引初始化为 0
    cons.write_idx = 0;  // 写索引初始化为 0
    cons.edit_idx = 0;   // 编辑索引初始化为 0
    /* (Final) TODO END */
}

/**
 * console_write - write to uart from the console buffer.
 * @ip: the pointer to the inode
 * @buf: the buffer
 * @n: number of bytes to write
 */
isize console_write(Inode *ip, char *buf, isize n)
{
    /* (Final) TODO BEGIN */
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    for (isize i = 0; i < n; i++) {
        char c = buf[i];
        if (c == BACKSPACE) {
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        } else {
            uart_put_char(c);
        }
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return n;
    /* (Final) TODO END */
}

/**
 * console_read - read to the destination from the buffer
 * @ip: the pointer to the inode
 * @dst: the destination
 * @n: number of bytes to read
 */
isize console_read(Inode *ip, char *dst, isize n)
{
    /* (Final) TODO BEGIN */
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    isize m = n;
    while (n > 0) {
        while (cons.read_idx == cons.write_idx) {
            if (thisproc()->killed) {
                release_spinlock(&cons.lock);
                inodes.lock(ip);
                return -1;
            }
            release_spinlock(&cons.lock);
            unalertable_wait_sem(&cons.sem);
            acquire_spinlock(&cons.lock);
        }
        int c = cons.buf[cons.read_idx % IBUF_SIZE];
        cons.read_idx += 1;
        if (c == C('D')) {
            if (n < m)
                cons.read_idx--;
            break;
        }
        *dst++ = c;
        --n;
        if (c == '\n')
            break;
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return m - n;
    /* (Final) TODO END */
}

void console_intr(char c)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&cons.lock);

    switch (c) {
    case C('C'):
        uart_put_char('^');
        uart_put_char('C');
        uart_put_char('\n');
        __attribute__((fallthrough));  // [[fallthrough]];

    case C('U'):
        // Clear line (equivalent to clear_line)
        while (cons.edit_idx != cons.write_idx && cons.buf[(cons.edit_idx - 1) % IBUF_SIZE] != '\n') {
            cons.edit_idx--;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;

    case C('H'):
    case '\x7f':
        if (cons.edit_idx != cons.write_idx) {
            cons.edit_idx--;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;

    default:
        if (c != 0 && cons.edit_idx - cons.read_idx < IBUF_SIZE) {
            c = (c == '\r') ? '\n' : c;
            uart_put_char(c);
            cons.buf[cons.edit_idx++ % IBUF_SIZE] = c;

            if (c == '\n' || c == C('D') || cons.edit_idx == cons.read_idx + IBUF_SIZE) {
                // Store the buffer into the larger buffer
                static char buf[LINES][IBUF_SIZE];  // LINES * IBUF_SIZE = PAGE_SIZE
                static int buf_idx = 0;

                memcpy(buf[buf_idx], cons.buf + cons.read_idx % IBUF_SIZE, IBUF_SIZE);
                buf_idx = (buf_idx + 1) % LINES;

                cons.write_idx = cons.edit_idx;
                post_sem(&cons.sem);
            }
        }
        break;
    }

    release_spinlock(&cons.lock);
    /* (Final) TODO END */
}