#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>

// 显示帮助信息
static void display_usage_info(void) {
    printf("Usage: mkdir [OPTION]... DIRECTORY...\n");
    printf("  -m, --mode=MODE       set file mode (only 0 supported)\n");
    printf("  -p, --parents         create parent directories as needed\n");
    printf("  -v, --verbose         print a message for each created directory\n");
    printf("  -Z, --context=CTX     (NOT implemented) set the SELinux security context of each created directory to CTX\n");
    printf("      --help            display this help and exit\n");
    printf("      --version         output version information and exit\n");
    exit(0);
}

// 显示版本信息
static void display_version_info(void) {
    printf("mkdir version 1.0\n");
    exit(0);
}

// 未实现的 SELinux 上下文设置功能
static void configure_security_context(const char *context_path) {
    (void)context_path; // 防止未使用参数警告
    printf("setfilecon: not implemented\n");
    exit(1);
}

// 创建目录及其所有父目录
static int create_directory_with_parents(const char *directory_path, mode_t permission_mode, bool show_message) {
    char path_buffer[1024];
    char *current_position = NULL;
    size_t path_length;
    
    // 复制路径到缓冲区
    strncpy(path_buffer, directory_path, sizeof(path_buffer));
    path_buffer[sizeof(path_buffer) - 1] = '\0';
    
    path_length = strlen(path_buffer);
    // 移除末尾的斜杠
    if (path_buffer[path_length - 1] == '/') {
        path_buffer[path_length - 1] = '\0';
    }
    
    // 逐级创建父目录
    for (current_position = path_buffer + 1; *current_position; current_position++) {
        if (*current_position == '/') {
            *current_position = '\0';
            mkdir(path_buffer, permission_mode);
            *current_position = '/';
        }
    }
    
    // 创建目标目录
    if (mkdir(path_buffer, permission_mode) == 0 && show_message) {
        printf("mkdir: created directory '%s'\n", path_buffer);
    }
    
    return 0;
}

int main(int argc, char *argv[]) 
{
    /* (Final) TODO BEGIN */
    static struct option command_options[] = {
        {"mode",    required_argument, 0, 'm'},
        {"parents", no_argument,       0, 'p'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'V'},
        {"context", required_argument, 0, 'Z'},
        {0, 0, 0, 0}
    };

    bool create_parents = false;
    bool verbose_output = false;
    mode_t directory_mode = 0; // 仅支持模式 0
    int option_value;

    // 解析命令行参数
    while ((option_value = getopt_long(argc, argv, "m:pvhVZ:", command_options, NULL)) != -1) {
        switch (option_value) {
            case 'm':
                directory_mode = strtol(optarg, NULL, 8);
                break;
            case 'p':
                create_parents = true;
                break;
            case 'v':
                verbose_output = true;
                break;
            case 'h':
                display_usage_info();
                break;
            case 'V':
                display_version_info();
                break;
            case 'Z':
                configure_security_context(optarg);
                break;
            default:
                fprintf(stderr, "Try --help for more information.\n");
                exit(1);
        }
    }

    // 检查是否提供了目录参数
    if (optind >= argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        fprintf(stderr, "Try 'mkdir --help' for more information.\n");
        exit(1);
    }

    // 为每个指定的目录执行创建操作
    for (int argument_index = optind; argument_index < argc; argument_index++) {
        int operation_result;
        
        if (create_parents) {
            operation_result = create_directory_with_parents(argv[argument_index], 
                                                           directory_mode, 
                                                           verbose_output);
        } else {
            operation_result = mkdir(argv[argument_index], directory_mode);
            if (operation_result == 0 && verbose_output) {
                printf("mkdir: created directory '%s'\n", argv[argument_index]);
            }
        }
        
        if (operation_result < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[argument_index], strerror(errno));
        }
    }
    
    exit(0);
    /* (Final) TODO END */
}