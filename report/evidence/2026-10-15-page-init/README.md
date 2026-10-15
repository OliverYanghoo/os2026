# page_init 验证记录

日期：2026-10-15。实现位置：`code/kern/mm/pmm.c` 的 `//lab2 function1 2412351`。

复现：

```bash
conda activate os2026
cd /home/lhl/os2026/lab2/code
make obj/kern/mm/pmm.o
python3 tools/check_page_init.py
```

- `page-init-check.log`：13 个隔离用例的汇总，全部通过。
- `case-00.log`～`case-12.log`：每个用例在 QEMU 中的实际输出。预期失败输入会打印已匹配的 panic 原因，并标记该用例通过。
- `build.log`：独立测试内核的真实编译和链接命令。
- `lab2-after-page-init-build.log`：完整工程尚未完成 Best-Fit，实际构建失败的记录。

测试包含真正的 `page_init()`，DTB getter、分配器注册回调、启动汇编和输出通路使用独立测试设施。未修改题目提供的测试函数。此结果不表示完整实验或所有分配器通过，测试细节见 `code/tests/README.md`。

日志中的临时内核目录已在测试结束后删除。QEMU/OpenSBI 输出中的 NUL 字节保存为可见的 `\x00`，末尾进程终止信息来自测试完成后的清理。
