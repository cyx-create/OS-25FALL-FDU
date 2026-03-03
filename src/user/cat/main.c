#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>

// 选项标志结构体
typedef struct {
    int line_numbers;        // 显示行号
    int skip_empty_lines;    // 跳过空行编号
    int mark_line_end;       // 标记行尾
    int show_tab_chars;      // 显示制表符
    int compress_empty;      // 压缩空行
    int display_nonprint;    // 显示非打印字符
} CommandOptions;

CommandOptions opts = {0};

// 辅助函数：检查字符是否需要特殊处理
static int needs_special_handling(char ch) {
    if (opts.display_nonprint && ch < 32 && ch != '\n' && ch != '\t') return 1;
    if (opts.show_tab_chars && ch == '\t') return 1;
    if (opts.mark_line_end && ch == '\n') return 1;
    return 0;
}

// 显示字符（带格式化）
static void display_character(char ch) {
    // 处理不可打印字符
    if (opts.display_nonprint) {
        if (ch < 32 && ch != '\n' && ch != '\t') {
            printf("^%c", ch + 64);
            return;
        }
        if (ch >= 127) {
            printf("M-%c", ch - 128);
            return;
        }
    }
    
    // 处理制表符
    if (opts.show_tab_chars && ch == '\t') {
        printf("^I");
        return;
    }
    
    // 处理行尾
    if (opts.mark_line_end && ch == '\n') {
        putchar('$');
    }
    
    putchar(ch);
}

// 处理单个文件
static int process_file(const char* filename) {
    int file_descriptor;
    
    // 打开文件
    if (strcmp(filename, "-") == 0) {
        file_descriptor = STDIN_FILENO;
    } else {
        file_descriptor = open(filename, O_RDONLY);
        if (file_descriptor < 0) {
            fprintf(stderr, "cat: cannot access '%s': %s\n", 
                    filename, strerror(errno));
            return -1;
        }
    }
    
    char buffer[4096];
    int bytes_read;
    int line_counter = 1;
    int consecutive_empty = 0;
    int at_line_start = 1;
    
    // 读取并处理文件内容
    while ((bytes_read = read(file_descriptor, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            char current = buffer[i];
            
            // 压缩空行处理
            if (opts.compress_empty) {
                if (current == '\n') {
                    if (consecutive_empty > 0) {
                        continue;
                    }
                    consecutive_empty++;
                } else {
                    consecutive_empty = 0;
                }
            }
            
            // 行号处理
            if (at_line_start) {
                if (opts.line_numbers || 
                    (opts.skip_empty_lines && current != '\n')) {
                    printf("%6d\t", line_counter);
                }
                at_line_start = 0;
            }
            
            // 显示字符
            display_character(current);
            
            // 行结束处理
            if (current == '\n') {
                line_counter++;
                at_line_start = 1;
            }
        }
    }
    
    // 错误检查
    if (bytes_read < 0) {
        fprintf(stderr, "cat: error reading '%s': %s\n", 
                filename, strerror(errno));
        if (file_descriptor != STDIN_FILENO) close(file_descriptor);
        return -1;
    }
    
    // 清理
    if (file_descriptor != STDIN_FILENO) {
        if (close(file_descriptor) < 0) {
            fprintf(stderr, "cat: error closing '%s': %s\n", 
                    filename, strerror(errno));
        }
    }
    
    return 0;
}

// 显示使用帮助
static void show_usage() {
    printf("Usage: cat [OPTION]... [FILE]...\n");
    printf("Display FILE contents to standard output.\n\n");
    printf("Options:\n");
    printf("  -A, --show-all       equivalent to -vET\n");
    printf("  -b, --number-nonblank number nonempty lines\n");
    printf("  -e                   equivalent to -vE\n");
    printf("  -E, --show-ends      display $ at line ends\n");
    printf("  -n, --number         number all lines\n");
    printf("  -s, --squeeze-blank  suppress repeated empty lines\n");
    printf("  -t                   equivalent to -vT\n");
    printf("  -T, --show-tabs      display TAB as ^I\n");
    printf("  -v, --show-nonprint  show non-printing characters\n");
    printf("      --help           show this help\n");
    printf("      --version        show version info\n\n");
    printf("With no FILE, or when FILE is -, read standard input.\n");
    exit(0);
}

// 显示版本信息
static void show_version() {
    printf("cat version 1.0\n");
    exit(0);
}

int main(int argc, char *argv[]) {
    /* (Final) TODO BEGIN */
    static struct option long_opts[] = {
        {"show-all", no_argument, 0, 'A'},
        {"number-nonblank", no_argument, 0, 'b'},
        {"show-ends", no_argument, 0, 'E'},
        {"number", no_argument, 0, 'n'},
        {"squeeze-blank", no_argument, 0, 's'},
        {"show-tabs", no_argument, 0, 'T'},
        {"show-nonprinting", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "AbEnsTtvhV", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'A':
                opts.display_nonprint = opts.mark_line_end = opts.show_tab_chars = 1;
                break;
            case 'b':
                opts.skip_empty_lines = 1;
                break;
            case 'e':
                opts.display_nonprint = opts.mark_line_end = 1;
                break;
            case 'E':
                opts.mark_line_end = 1;
                break;
            case 'n':
                opts.line_numbers = 1;
                break;
            case 's':
                opts.compress_empty = 1;
                break;
            case 'T':
                opts.show_tab_chars = 1;
                break;
            case 't':
                opts.display_nonprint = opts.show_tab_chars = 1;
                break;
            case 'v':
                opts.display_nonprint = 1;
                break;
            case 'h':
                show_usage();
                break;
            case 'V':
                show_version();
                break;
            default:
                fprintf(stderr, "Try 'cat --help' for more information.\n");
                return 1;
        }
    }
    
    // 处理文件
    if (optind >= argc) {
        // 没有文件名，读取标准输入
        process_file("-");
    } else {
        // 处理所有指定的文件
        for (int i = optind; i < argc; i++) {
            if (process_file(argv[i]) < 0) {
                return 1;
            }
        }
    }
    /* (Final) TODO END */
    return 0;
}
