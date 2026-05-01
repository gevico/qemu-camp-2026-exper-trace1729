# QEMU 全景：从 100 行到 100 万行

## 一、一次 GDB 追踪

如果你在 QEMU Camp 仓库里跑过 `build/qemu-system-riscv64`，可能觉得它像一个黑盒子：输入命令行加一个 ELF，输出 `hello from g233`。黑盒子里到底发生了什么？

我们拿 GDB 追踪一次看看。写一个最简单的测试程序：

```c
#include "crt.h"
int main(void) {
    int a = 1, b = 2, c;
    c = a + b;
    printf("hello from g233: %d + %d = %d\n", a, b, c);
    return 0;
}
```

编译后喂给 QEMU，用 GDB 在 `main` 和 `virt_machine_done` 设断点跑一遍。你会看到两条主线：

```
[主线 A — QEMU 自己]
main → qemu_init → qemu_machine_creation_done → virt_machine_done
       ↑                                            ↑
  解析命令行，创建虚拟机                         g233 板子初始化完毕

[主线 B — 替 guest 跑]
tcg_cpu_exec → cpu_exec → cpu_loop_exec_tb → cpu_tb_exec
                                                ↑
                                      第一条 RISC-V 指令在此执行
```

这两条线之间夹着 QEMU 最核心的秘密：几个看起来各自只做一件小事的模块，串在一起就撑起了一台完整的 RISC-V 虚拟机。这篇文章就带你从 100 行的 `test_gdb.elf` 出发，走到 QEMU 的 100 万行源码中去。

## 二、一个类比：QEMU 像一座影城

在学习一个复杂系统之前，最好先在脑子里找一个熟悉的"锚"。我们拿影城来类比：

| 影城角色 | 做什么小事 | QEMU 里的对应模块 |
|---|---|---|
| **售票员** | 检票，告诉你去几号厅 | `vl.c` — 解析命令行，启动虚拟机 |
| **放映机** | 把胶片上的画面投射到银幕上 | **TCG** — 把 RISC-V 机器码翻译成 x86 机器码执行 |
| **舞台布景** | 放映机转动时，幕布、灯光、音响协同动作 | **设备模型** — 模拟 GPIO、SPI、中断控制器 |
| **导演对讲系统** | 机房和舞台之间喊话的通道 | **QOM、IRQ、总线** — 模块间通信 |

你买了票（输入命令行），售票员安排场次（`qemu_init`），放映机开始转动（TCG 翻译并执行），舞台上布景配合演出（外设读写），最后你看到了完整的一场电影（guest 程序输出结果）。

需要注意的是——QEMU 这座影城里没有任何一个人知道全局。售票员不认识放映员、放映员不知道布景怎么动。每个人只做手头那件琐碎的小事，但通过几条固定的通信规则（IRQ、MemoryRegion 读写、QOM 类型系统），它们协作出了一台完整的虚拟机。

## 三、第一层：六大模块全景图

带着影城的印象，我们来看 QEMU 源码目录的树状结构。它不算复杂——整个仓库的核心只有六件事：

```
                          QEMU
                            |
            +---------------+---------------+
            |               |               |
         系统框架          加速层         目标架构
     (system/vl.c)    (accel/tcg)   (target/riscv)
            |               |               |
            +-------+-------+-------+-------+
                    |               |
                 机器模型        设备模型
           (hw/riscv/g233.c) (hw/gpio, hw/spi...)
                    |
              QOM 对象模型 (qom/)
```

| 模块 | 一句话职责 |
|---|---|
| **系统框架** (`system/`) | 总调度：解析命令行、创建虚拟机实例、启动事件循环 |
| **加速层** (`accel/tcg/`) | 把 guest 的 RISC-V 机器码翻译成 host 能直接跑的 x86/arm 机器码 |
| **目标架构** (`target/riscv/`) | 定义 RISC-V CPU 的寄存器、CSR、每条指令的翻译规则 |
| **机器模型** (`hw/riscv/g233.c`) | 定义板子布局：内存从哪开始、外设插在哪些地址、用哪种 CPU |
| **设备模型** (`hw/misc/`, `hw/gpio/` 等) | 模拟每个外设芯片：CPU 读到某个地址时返回什么值 |
| **QOM** (`qom/`) | 所有对象的"身份证系统"：创建、销毁、类型检查、继承 |

这六层摞在一起，就像影城的组织架构图：每层都有自己的"一件小事"，但合起来就是一场完整的放映。

## 四、第二层：每个模块只做一件小事

Minsky 说，人类理解复杂系统的最好方式，是理解每个子模块各自只做什么。下面我们逐个放大这六个模块。

### 系统框架（`system/vl.c`）

**一件小事**：把命令行字符串拆成键值对，存进 `QemuOpts`，然后调用 `qemu_init()` 把所有子模块串起来。

你输入 `-M g233 -m 2G -device loader,file=test.elf`，`vl.c` 负责理解这串参数，创建一台 g233 机器，分配 2GB 内存，把 `test.elf` 加载到虚拟内存 `0x80000000`。做完这些，它的工作就结束了——后续的控制权交给事件循环和加速层。

> 🧪 **与你的实验**：你不需要改 `vl.c`，但每次 `make -f Makefile.camp test-cpu`，实际上就是在用 `vl.c` 拼出这条命令。

### 加速层 TCG（`accel/tcg/`）

**一件小事**：把一小段 RISC-V 指令翻译成一小段 x86 指令，然后跳过去执行。循环往复。

这就是 QEMU 作为"模拟器"而非"虚拟机"的核心：它不逐条解释，而是把一个基本块（TB，Translation Block）一次性翻译完，然后直接让物理 CPU 跑翻译后的代码。类比同传译员——不会等到整场演讲结束才开始翻译，而是听到一句就翻一句，观众几乎感觉不到延迟。

关键函数调用链：`cpu_exec → cpu_exec_loop → cpu_loop_exec_tb → cpu_tb_exec`

> 🧪 **与你的实验（CPU 方向）**：你在 `target/riscv/insn_trans/` 里写的翻译函数，就是 TCG 在翻译到你的自定义指令时调用的。你告诉 TCG "这条指令等价于这些 host 操作"，QEMU 就把你写的逻辑编入翻译后的代码中。

### 目标架构（`target/riscv/`）

**一件小事**：定义 RISC-V 的一切——有多少个通用寄存器、CSR 的编号和字段含义、每条指令 translate 成什么样的 TCG 中间表示。

这个目录是 QEMU 里"最像芯片手册"的地方。`cpu.c` 定义 CPU 的结构体（`CPURISCVState`），`translate.c` 负责把 RISC-V 指令转成 TCG ops，`csr.c` 实现所有 CSR 的读写语义。

> 🧪 **与你的实验（CPU 方向）**：你实现自定义指令时，`target/riscv/insn_trans/` 里新增的 `trans_xxx()` 函数，就是在这里被调用的。

### 机器模型（`hw/riscv/g233.c`）

**一件小事**：列一张板子清单——这台机器有几核 CPU、内存从哪到哪、每个外设挂在哪个地址上。

打开 `g233.c` 的 `virt_machine_done` 函数，你能看到整张清单：创建 CPU 对象、初始化设备树、创建 PLIC 中断控制器、挂载 PL011 串口。你加的任何外设，最终都要在这个函数里"登记户口"。

> 🧪 **与你的实验（SoC 方向）**：你写的 GPIO、PWM、WDT、SPI 控制器，都是在这里通过 `sysbus_create_simple("g233-gpio", 0x10012000, ...)` 挂上总线的。

### 设备模型（`hw/misc/`, `hw/gpio/`, `hw/spi/` 等）

**一件小事**：实现一个 `MemoryRegionOps`——当 CPU 访问 `0x10012000` 时，调用 `gpio_read()`；当 CPU 写入 `0x10012004` 时，调用 `gpio_write()`。

每个设备都是一个独立的小文件。它不关心谁来读写它，也不会主动去阻塞 CPU。它只做一件事：把寄存器的读写映射到一段 C 函数的调用上。

> 🧪 **与你的实验（SoC/GPGPU 方向）**：SoC 实验就是实现这些 `read()` / `write()` 回调。GPGPU 实验同理，只是设备挂在 PCI 总线上而非 SysBus。

### 对象模型 QOM（`qom/`）

**一件小事**：让所有设备都能通过名字被创建、让类型能安全地向上向下转型。

QOM 模仿了 QEMU 的 QObject（借鉴了 GLib 的 GObject）——每个设备都是一个 `Object`，有类名、父类、属性。`object_new("g233-gpio")` 能创建一个 GPIO 设备，因为 QOM 维护了一个全局的类注册表。

QOM 对外部你几乎透明，但对 QEMU 内部至关重要：没有它，`vl.c` 无法通过字符串创建机器和设备，每个新外设都要写一堆样板代码。

## 五、第三层：模块间如何交互

每个模块理解之后，下一步是看它们如何串在一起。下面是一条简化但完整的调用链——从 `main` 到 GPIO 寄存器被读取：

```
1. main()                                         ← system/main.c
2.   qemu_init()                                  ← system/vl.c: 解析命令行参数
3.     qemu_create_machine()                      ← 根据 "-M g233" 创建机器实例
4.       object_new("riscv-g233-machine")         ← qom/: 按名字查类型，实例化
5.     machine_run_board_init()                   ← 调用 g233 的初始化
6.       object_new("gevico-cpu-v1")              ← 创建 RISC-V CPU
7.       sysbus_create_simple("pl011", 0x10000000)← 串口设备挂到系统总线
8.       sysbus_create_simple("g233-gpio", 0x10012000) ← GPIO 挂到系统总线
9.     virt_machine_done()                        ← g233.c: 板子初始化完成回调
10.  qemu_main_loop()                              ← 进入事件循环，等待/执行
11.    cpu_exec(cpu)                               ← accel/tcg/: 开始执行 guest
12.      tb = tb_gen_code(cpu, pc)                 ← target/riscv/: 翻译 RISC-V 指令
13.        trans_rv32i_add() → tcg_gen_add_i64()   ← 一条 add → 一条 TCG op
14.      cpu_tb_exec(cpu, tb)                      ← 跳到翻译后的 host 代码执行
15.        [guest 代码执行中...]
16.        [guest 访问 0x10012000 读 GPIO]
17.          io_readx(0x10012000)                  ← 穿越 MemoryRegion
18.            gpio_read()                         ← hw/misc: 你的设备回调，返回寄存器值
```

这条链里有三个关键交互接口：

```
guest 指令 ──(translate)──→ TCG ops ──(gen_code)──→ host 机器码 ──(cpu_tb_exec)──→ 物理 CPU
                                                                                      │
                                                                           MemoryRegion 读写
                                                                                      │
                                                                        ┌─────────────┼─────────────┐
                                                                      GPIO           PWM           SPI
```

**每一个接口都极窄**：TCG 不关心接下来要跑的是 arm 还是 riscv，它只管把 TCG ops 编译成 host 代码。设备模型不关心谁在访问它，它只管收到地址后返回或写入数据。这种窄接口设计正是 Minsky 所说的"每个部件只做琐碎小事"的关键。

## 六、回到全貌：QEMU 到底做了什么

Minsky 强调，要真正"知道"一个东西，必须知道它的外部效应——输入什么，输出什么。

| 环节 | 内容 |
|---|---|
| **输入** | `qemu-system-riscv64 -M g233 -m 2G -device loader,file=test.elf` |
| **过程** | 系统框架分配 2GB 内存，机器模型创建 g233 板子和所有外设，TCG 把 `test.elf` 里的 RISC-V 指令翻译成 host 代码，CPU 一条条执行，遇到外设地址就回调设备模型 |
| **输出** | `test.elf` 在虚拟 RISC-V 环境里跑完，`printf` 的结果出现在你的终端上 |

现在你问：**我写的代码到底在哪里？**

- **CPU 实验**：你在 `target/riscv/insn_trans/` 里新增了一个 `trans_xxx()`。QEMU 翻译到你的自定义指令时，会调用你这个函数。你告诉 TCG "这条指令干什么"，QEMU 就多认识了一条指令。

- **SoC 实验**：你在 `hw/misc/` 里写了一个 `gpio_read()` 和 `gpio_write()`。当 guest 程序访问 `0x10012000` 时，QEMU 调用你的函数。你决定了"读到什么""写入后触发什么"，QEMU 就多了一个能正常工作的外设。

- **GPGPU 实验**：你写了一个完整的 PCI 设备模型，包括 VRAM、DMA 引擎、SIMT 执行单元。QEMU 在 PCI 总线上扫描到你的设备，初始化它，然后 guest 驱动可以用 MMIO 和它交互。

- **Rust 实验**：同上，但用 Rust 写设备模型，通过 FFI 挂到 QEMU 的 C 核心上。

## 七、下一步读什么

现在你脑子里有了 QEMU 这棵树的主干。接下来你可以按兴趣挑一根树枝深入：

| 你想知道 | 去看 |
|---|---|
| 从命令行到虚拟机创建的全过程 | `system/vl.c` 的 `qemu_init()` |
| TCG 怎么把 RISC-V 指令变成 host 代码 | `target/riscv/translate.c` + `tcg/tcg.c` |
| 一个 GPIO 设备从注册到读写的完整生命周期 | 「SoC 实验教程」+ `hw/misc/gpio-g233.c` |
| QOM 的对象创建和类型系统是怎么工作的 | `qom/object.c` + `include/qom/object.h` |
| CPU 实验的自定义指令怎么写 | 「CPU 实验教程」+ `target/riscv/insn_trans/` 里已有的例子 |
| GPGPU 的 PCI 设备是怎么挂上去的 | 「GPU 实验教程」+ `hw/gpgpu/` |

整棵树的分叉点你已经知道了，剩下的就是去每一片叶子上往下挖。
