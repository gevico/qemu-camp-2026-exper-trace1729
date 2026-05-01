# QEMU 全局架构介绍文章 — 设计文档

## 基本信息

- **目标读者**：QEMU Camp 2026 学员（有 C 语言基础，正在做此仓库的 CPU/SoC/GPGPU/Rust 实验）
- **主题**：QEMU 全局架构（模块划分、启动流程、模块间交互、与各实验方向的关系）
- **篇幅**：~2150 字核心速览（15 分钟阅读）
- **方法**：Marvin Minsky "The Society of Mind" 的认知原则
- **标题（暂定）**：「QEMU 全景：从 100 行到 100 万行」

## 方法论

遵循 Minsky 的 5 个原则：

1. **大型系统由各自只做琐碎小事的部件组成** → 以 6 大模块分解 QEMU
2. **理解 = 子模块功能 → 模块间交互 → 外部效应** → 段 4 → 段 5 → 段 6 的递进结构
3. **新知识挂载到已有记忆** → 用影城类比，用 g233.c / test-insn-vadd.c 等学员已接触过的代码做锚点
4. **记忆是树状的** → 段 3 先给全景图（根部），段 4 逐模块展开（枝叶），段 5 展示调用链（叶脉）
5. **类比复用已有记忆结构** → 段 2 的影城类比

## 文章结构

### 段 1 — 引子：一次 GDB 追踪（~200 字）

以仓库中实际可运行的 GDB 调试结果为引子，展示两条主线：

```
main → ... → virt_machine_done（板子初始化）
tcg_cpu_exec → cpu_tb_exec（执行第一行 guest 代码）
```

引出核心问题：这两条线之间藏着多少个模块？它们各自只做一件小事，但合起来就跑起了整个 RISC-V 系统。

**数据来源**：仓库中 `build/qemu-system-riscv64` 对 `test_gdb.elf` 的实际 GDB 跟踪结果。

### 段 2 — 类比：QEMU 像一个「影城」（~250 字）

| 影城角色 | QEMU 对应 |
|---|---|
| 售票员（检票、安排场次） | vl.c — 解析命令行、调度启动 |
| 放映机（把硬盘上的文件变成影画面） | TCG — 把 RISC-V 机器码翻译成 x86 执行 |
| 舞台布景（观众看不到的机械装置） | 设备模型 — 模拟 GPIO、SPI、中断控制器 |
| 场务对讲系统 | QOM、IRQ、总线 — 模块间的通信机制 |

### 段 3 — 第一层：六大模块全景图（~400 字）

树状根部的层次图：

```
                     QEMU
                       |
       +---------------+---------------+
       |               |               |
    系统框架        加速层         目标架构
  (system/vl.c)   (accel/tcg)   (target/riscv)
       |               |               |
       +-------+-------+-------+-------+
               |               |
            机器模型        设备模型
         (hw/riscv/g233.c) (hw/gpio, hw/i2c, hw/spi...)
               |
          QOM 对象模型 (qom/)
```

每层一句话职责说明。

### 段 4 — 第二层：各模块只做一件小事（~500 字）

6 个模块，每个 50-80 字，说明职责 + 代码位置 + 与 Camp 实验的关系：

| 模块 | 一件小事 | 代码位置 | 实验关联 |
|---|---|---|---|
| 系统框架 (vl.c) | 把命令行变成 `QemuOpts`，调用 `qemu_init` | `system/vl.c:qemu_init` | — |
| TCG (加速层) | 把一段 RISC-V 指令翻译成 x86 指令，跳过去执行 | `accel/tcg/cpu-exec.c:cpu_tb_exec` | CPU 实验：自定义指令在此翻译 |
| target/riscv | 定义 RISC-V 的寄存器、CSR、指令翻译规则 | `target/riscv/translate.c` | CPU 实验 |
| 机器模型 (g233.c) | 列清单：内存布局、外设地址、CPU 型号 | `hw/riscv/g233.c:virt_machine_done` | SoC 实验：外设在此注册 |
| 设备模型 (hw/) | 模拟外设芯片——当 CPU 读写某个地址时，返回对应的寄存器值 | `hw/misc/gpio-g233.c` 等 | SoC/GPGPU 实验 |
| QOM (对象模型) | 让每个设备都能被 `object_new("g233-gpio")` 创建 | `qom/object.c` | — |

### 段 5 — 第三层：模块间如何交互（~400 字）

简化的调用链（来源：仓库实际 GDB 跟踪）：

```
1. main()
2.   qemu_init()                    ← 系统框架：解析参数
3.     machine_run_board_init()     ← 创建 g233 机器
4.       object_new("gevico-cpu")   ← QOM 创建 CPU
5.       sysbus_create("g233-gpio", 0x10012000) ← 设备挂到总线
6.     virt_machine_done()          ← 板子初始化完成
7.   qemu_main_loop()               ← 进入事件循环
8.     cpu_exec(cpu)                ← 开始执行 guest
9.       tb = tb_gen_code()         ← TCG 翻译 RISC-V 指令
10.        cpu_tb_exec(tb)          ← 执行翻译后的代码
11.          io_readx(0x10012000)    ← guest 访问 GPIO 寄存器
12.            gpio_read()          ← 设备模型返回寄存器的值
```

用箭头图说明 guest 代码 → TCG → host 执行 → MemoryRegion 读写 → 设备回调的数据流。

### 段 6 — 回到全貌：QEMU 到底「做了什么」（~250 字）

Minsky 的"外部效应"视角：

- **输入**：`qemu-system-riscv64 -M g233 -device loader,file=test.elf`
- **输出**：test.elf 在虚拟 RISC-V 环境里跑完，printf 结果出现在终端
- **中间过程**：6 个模块各自完成自己的小事
- **学员代码的位置**：
  - CPU 实验 → 改了 TCG 翻译 → QEMU 多认识一条指令
  - SoC 实验 → 写了设备回调 → QEMU 多了一个正常工作的外设
  - GPGPU 实验 → 写了 PCI 设备模型 + SIMT → QEMU 能模拟 GPU 计算

### 段 7 — 收尾：下一步读什么（~150 字）

引导学员按兴趣深入：
- 启动细节 → `system/vl.c` 的 `qemu_init`
- TCG 翻译 → `target/riscv/translate.c`
- 设备模型 → SoC 实验教程 + `hw/misc/gpio-g233.c`
- 对象模型 → `qom/object.c` + `include/qom/object.h`

## 与仓库实验的关联

文章在每个关键模块处标注了与 CPU/SoC/GPGPU/Rust 四个实验方向的对应关系（🧪 标记），确保学员能一眼看到"我写的代码在 QEMU 的哪个位置"。

## 关键代码引用

文章中引用的实际代码位置（来自仓库已验证的文件）：

- `system/vl.c` — 启动函数 `qemu_init`
- `accel/tcg/cpu-exec.c` — `cpu_tb_exec`
- `target/riscv/translate.c` — 指令翻译
- `hw/riscv/g233.c` — 机器模型定义
- `qom/object.c` — QOM 对象系统
- `tests/gevico/tcg/riscv64/Makefile.softmmu-target` — 测试编译和 QEMU 调用
