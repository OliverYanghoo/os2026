# Best-Fit 实现与验证记录

日期：2026-10-15。负责人：李华濂，2412351。

## 状态和不变量

`free_area` 只包含框架约定的 `free_list`、`nr_free`。链表按 `Page` 地址递增排列；在连续的描述符数组中，这与物理页地址递增一致。链表只链接空闲块首个描述符；块头设置 `PageProperty`，`property` 为该块连续页数。其余空闲页及已分配页的 `PageProperty` 清零，当前实现也将其 `property` 清零。

空闲块不重叠；物理相邻的空闲块在注册或释放时立即合并。`nr_free` 等于所有空闲块长度之和，统计页数。分割不重复计数，合并不增加计数，释放只增加本次释放的页数。分配时扫描全部块，选择最小足够大块；大小相同则保留先遍历到的低地址块。

没有额外索引，因此兼容题目自测临时清空、保存和恢复链表头与页计数的做法。接口沿用框架约定：申请和注册/释放的数量应为正，释放范围应为调用者持有的合法页面。本模块不提供通用的非法指针或所有重复释放检测。

## 修改

- `code/kern/mm/best_fit_pmm.c`：标记 `//lab2 function2-2 2412351`；补全全部管理器接口，以同一个辅助函数完成有序插入及相邻合并；定义管理器并绑定现有 `best_fit_check`。
- `code/kern/mm/pmm.c`：`init_pmm_manager()` 选择 `best_fit_pmm_manager`。
- `code/tests/best_fit_check.c`、`code/tools/check_best_fit.py`：新增独立验证；复用既有测试入口。
- `code/tests/README.md`：补充复现方法与测试边界。
- `report/prompt.md`：按约定记录本轮实际提问及结果。

`preservation-check.log` 确认现有两个管理器自测的完整文本 SHA-256 与修改前一致；`default_pmm.c`、正式入口汇编、SBI 和题目测试/注入/评分脚本保持不变。基准哈希见 `tests-before.sha256`、`unchanged-before.json`。

## 复杂度

设空闲块数为 k，本次注册或释放页数为 n：初始化 O(1)，空闲计数 O(1)，分配 O(k)，注册和释放 O(n+k)。插入位置确定后，最多合并前后两个相邻块，合并本身为 O(1)。除框架已有 Page 描述符和链表外，管理器只需要 O(1) 额外空间。

## 实际验证

在 `lab2/code/` 下执行：

```bash
conda activate os2026
make clean
make
python3 tools/check_best_fit.py
python3 tools/check_page_init.py
```

- 完整代码清理后构建成功，见 `build.log`。
- Best-Fit 隔离检查在 `-O0`、`-O2` 下通过。原有自测各得 `grading: 6 / 6 points`；这只代表其六项内部检查，不代表整个实验评分。
- 每种构建还验证选择策略、同大小规则、状态清理、分割、整块分配、外部碎片失败、三种位置插入、相邻合并和 512 次独立位图模型操作。见 `console-O0.log`、`console-O2.log`；独立构建命令见 `isolated-build.log`。
- 此前的 13 个 `page_init` 隔离用例全部回归通过，见 `page-init-regression.log`。

隔离启动输出包括：

```text
memory management: best_fit_pmm_manager
check_alloc_page() succeeded!
[Test 1] Page Init: PASS
[Test 2] Best-Fit: PASS
BEST_FIT_PASS
```

## 正式启动阻塞与验证范围

正式 `bin/ucore.img` 另行在 QEMU 启动，并用 GDB 连接专用端口 15325 取样，随后结束本次创建的进程，未占用正常调试端口 1234。记录见 `production-boot.log`、`production-boot-gdb.log`。

实际读到 `satp=0x8000000000080205`、`scause=0xc`、`stval=0x80200000`。现有入口根页表仅设置高地址映射项 511，没有当前低地址所需的项 2，启用分页后发生取指页缺页，尚未进入 `pmm_init()`。这说明正式启动仍受入口分页切换依赖阻塞；本轮没有修改该模块或 SBI、DTB、Buddy，也没有声称完整启动或 `make grade` 通过。

Best-Fit 检查用独立入口、UART 和 DTB getter 替身排除上述依赖，但执行实际分配器、`pmm_init()`、`page_init()` 和未改动的题目检查。隔离通过与正式启动结果分别记录，不能互相替代。本轮没有 Git 提交或推送。
