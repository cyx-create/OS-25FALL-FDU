import subprocess
import time
import os

# 测试参数
BUILD_DIR = "build"  # QEMU 在该目录下 make
MAX_TIMEOUT = 3      # 秒
LOOP_TIMES =700       # 循环测试次数
LOG_DIR = "qemu_logs"
os.makedirs(LOG_DIR, exist_ok=True)

for run_idx in range(1, LOOP_TIMES + 1):
    print(f"[RUN {run_idx}] Starting test...")

    # 启动 QEMU
    proc = subprocess.Popen(
        ["make", "qemu"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.PIPE,
        cwd=BUILD_DIR,
        bufsize=1,
        universal_newlines=True
    )

    start_time = time.time()
    success = False
    output_lines = []

    try:
        while True:
            # 读取一行输出
            line = proc.stdout.readline()
            if line:
                print(line, end="")
                output_lines.append(line)
                if "proc_test PASS" in line:
                    success = True
                    break

            # 超时检测
            if time.time() - start_time > MAX_TIMEOUT:
                print("[TIMEOUT] Test exceeded max time.")
                break

            # QEMU 已退出
            if proc.poll() is not None:
                break

    except Exception as e:
        print("[ERROR] Exception while reading stdout:", e)

    # 尝试退出 QEMU
    if proc.poll() is None:
        try:
            # Ctrl+A + x
            proc.stdin.write('\x01x\n')
            proc.stdin.flush()
            time.sleep(0.5)
        except Exception as e:
            print("[ERROR] Failed to send Ctrl+A x:", e)

        # 再兜底杀掉
        proc.kill()
        proc.wait()

    # 保存失败日志
    if not success:
        log_file = os.path.join(LOG_DIR, f"run_{run_idx}.log")
        with open(log_file, "w") as f:
            f.writelines(output_lines)
        print(f"[FAILED] Output saved to {log_file}")
    else:
        print("[PASS] Test passed")

    # 等待一小段时间再开始下一轮
    time.sleep(0.5)
