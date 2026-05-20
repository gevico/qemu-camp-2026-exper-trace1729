#include "qemu/osdep.h"
#include "qemu/log.h"
#include "gpgpu.h"
#include "gpgpu_core.h"
#include "fpu/softfloat.h"

/* Bit extraction macros from NEMU */
#define BITS(x, hi, lo) ((x) >> (lo) & ((1ULL << ((hi) - (lo) + 1)) - 1))
#define SEXT(x, len)    ((int32_t)((x) | ((x) & (1ULL << ((len) - 1)) ? ~((1ULL << (len)) - 1) : 0)))

/* RISC-V opcode constants */
#define OP_LB   0x03
#define OP_LH   0x03
#define OP_LW   0x03
#define OP_LBU  0x03
#define OP_LHU  0x03
#define OP_ADDI 0x13
#define OP_SLLI 0x13
#define OP_SLTI 0x13
#define OP_SLTIU 0x13
#define OP_XORI 0x13
#define OP_ORI  0x13
#define OP_ANDI 0x13
#define OP_SRLI 0x13
#define OP_SRAI 0x13

/* Lane-level VRAM access */
static uint32_t vram_readl(GPGPUState *s, uint32_t addr)
{
    if (addr + 4 > s->vram_size) return 0;
    uint32_t val;
    memcpy(&val, s->vram_ptr + addr, 4);
    return val;
}

static void vram_writel(GPGPUState *s, uint32_t addr, uint32_t val)
{
    if (addr + 4 > s->vram_size) return;
    memcpy(s->vram_ptr + addr, &val, 4);
}

/* Immediate decoding */
static int32_t imm_I(uint32_t inst) { return SEXT(BITS(inst, 31, 20), 12); }
static int32_t imm_U(uint32_t inst) { return (int32_t)SEXT(BITS(inst, 31, 12), 20) << 12; }
static int32_t imm_S(uint32_t inst) { return SEXT((BITS(inst, 31, 25) << 5) | BITS(inst, 11, 7), 12); }
static int32_t imm_B(uint32_t inst) { return SEXT((BITS(inst, 31, 31) << 11 | BITS(inst, 7, 7) << 10 | BITS(inst, 30, 25) << 4 | BITS(inst, 11, 8)), 12) << 1; }
static int32_t imm_J(uint32_t inst) { return SEXT((BITS(inst, 31, 31) << 19 | BITS(inst, 19, 12) << 11 | BITS(inst, 20, 20) << 10 | BITS(inst, 30, 21)), 20) << 1; }

/*
 * ============================================================================
 * CFG Post-Dominator Analysis (kernel 加载时执行一次)
 * ============================================================================
 * 扫描 kernel 二进制，为每个 branch 指令计算 immediate post-dominator (IPOST)。
 * IPOST 是真实汇合点，取代 then_pc < curr_pc ? else_pc : then_pc 启发式。
 *
 * 算法:
 *   1. 从 kernel_addr 出发做可达性扫描, 构建 CFG (前驱/后继)
 *   2. 用迭代数据流法计算 post-dominiator: PD[n] = {n} ∪ (∩ PD[s])
 *   3. 每个 branch 的 IPOST = 在 PD[n] - {n} 中后支配所有其它候选者的节点
 * ============================================================================
 */

#define CFG_MAX_NODES  1024  /* 最大 4KB kernel (1024 条指令) */
#define CFG_NIL        0xFFFF

/* CFG 节点: 一个指令地址 */
typedef struct {
    uint32_t pc;                    /* 指令地址 (相对于 kernel_addr 的偏移) */
    uint16_t succ[2];              /* 后继节点索引 (最多 2 个) */
    uint8_t  num_succ;
    bool     is_branch;            /* 是 branch 指令 (需要计算 IPOST) */
} CFGNode;

/*
 * gpgpu_core_build_cfg - 构建 kernel 的 CFG 并计算 IPOST
 * @s: GPGPU 设备状态
 * @kernel_addr: kernel 在 VRAM 中的地址
 *
 * 结果写入 s->branch_pcs / s->reconv_pcs / s->num_branches
 * 所有分配的数组后续由 gpgpu_core_free_cfg 释放
 */
static void gpgpu_core_build_cfg(GPGPUState *s, uint32_t kernel_addr)
{
    CFGNode nodes[CFG_MAX_NODES];
    int nnodes = 0;
    int exit_idx = -1;

    /*
     * 第一遍: 用 worklist 做可达性分析
     * 从 kernel_addr 开始，沿着控制流边扩散
     */
    bool visited[CFG_MAX_NODES] = {false};

    /* worklist */
    uint32_t wl_pc[CFG_MAX_NODES];
    int wl_head = 0, wl_tail = 0;
    wl_pc[wl_tail++] = kernel_addr;

    while (wl_head < wl_tail) {
        uint32_t pc = wl_pc[wl_head++];
        int idx = (pc - kernel_addr) / 4;
        if (idx < 0 || idx >= CFG_MAX_NODES) continue;
        if (visited[idx]) continue;

        visited[idx] = true;
        nodes[idx].pc = pc;
        nodes[idx].num_succ = 0;
        nodes[idx].is_branch = false;

        uint32_t inst = vram_readl(s, pc);
        uint8_t opcode = BITS(inst, 6, 0);

        if (inst == 0x00100073) {
            /* ebreak: exit node, no successors */
            nodes[idx].num_succ = 0;
            exit_idx = idx;
            nnodes++;
            continue;
        }

        if (opcode == 0x63) {
            /* B-type branch */
            int32_t imm = imm_B(inst);
            uint32_t then_pc = pc + imm;
            uint32_t else_pc = pc + 4;
            nodes[idx].is_branch = true;
            nodes[idx].succ[0] = (else_pc - kernel_addr) / 4;
            nodes[idx].succ[1] = (then_pc - kernel_addr) / 4;
            nodes[idx].num_succ = 2;
        } else if (opcode == 0x6F) {
            /* JAL: unconditional jump */
            int32_t imm = imm_J(inst);
            uint32_t target = pc + imm;
            nodes[idx].succ[0] = (target - kernel_addr) / 4;
            nodes[idx].num_succ = 1;
        } else {
            /* Fall-through (包括 JALR / CSR, 保守处理) */
            nodes[idx].succ[0] = (pc + 4 - kernel_addr) / 4;
            nodes[idx].num_succ = 1;
        }

        /* 将未访问的后继加入 worklist */
        for (int si = 0; si < nodes[idx].num_succ; si++) {
            int sidx = nodes[idx].succ[si];
            if (sidx >= 0 && sidx < CFG_MAX_NODES && !visited[sidx]) {
                wl_pc[wl_tail++] = kernel_addr + sidx * 4;
                if (wl_tail > CFG_MAX_NODES) wl_tail = CFG_MAX_NODES;
            }
        }
        nnodes++;
    }

    /*
     * 第二遍: 迭代数据流求 post-dominator (PD)
     * PD[n] = {n} ∪ (∩_{s∈succ[n]} PD[s])
     * 用 128-bit 的位图表示 (CFG_MAX_NODES=1024 → 16 个 uint64)
     */
    uint64_t pd[CFG_MAX_NODES][16];   /* PD 位图 */
    uint64_t all_nodes[16] = {0};

    for (int i = 0; i < nnodes; i++) {
        int bit = i / 64;
        int off = i % 64;
        all_nodes[bit] |= (1ULL << off);
    }

    /* 初始化: PD[exit] = {exit}, PD[others] = all nodes */
    for (int i = 0; i < CFG_MAX_NODES; i++) {
        if (!visited[i]) continue;
        for (int b = 0; b < 16; b++) {
            pd[i][b] = all_nodes[b];
        }
    }
    if (exit_idx >= 0) {
        memset(pd[exit_idx], 0, sizeof(pd[0]));
        int bit = exit_idx / 64;
        int off = exit_idx % 64;
        pd[exit_idx][bit] = (1ULL << off);
    }

    /* 迭代直到稳定 */
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 100) {
        changed = false;
        iterations++;
        for (int i = 0; i < CFG_MAX_NODES; i++) {
            if (!visited[i]) continue;
            if (i == exit_idx) continue;

            /* ∩ PD[s] */
            if (nodes[i].num_succ == 0) {
                continue;  /* dead end, keep initialization */
            }

            uint64_t intersect[16];
            memcpy(intersect, pd[nodes[i].succ[0]], sizeof(intersect));
            for (int si = 1; si < nodes[i].num_succ; si++) {
                int sidx = nodes[i].succ[si];
                if (sidx >= 0 && sidx < CFG_MAX_NODES && visited[sidx]) {
                    for (int b = 0; b < 16; b++) {
                        intersect[b] &= pd[sidx][b];
                    }
                }
            }

            /* {i} ∪ intersect */
            uint64_t new_pd[16];
            memcpy(new_pd, intersect, sizeof(new_pd));
            int bit = i / 64;
            int off = i % 64;
            new_pd[bit] |= (1ULL << off);

            /* 比较 */
            for (int b = 0; b < 16; b++) {
                if (new_pd[b] != pd[i][b]) {
                    changed = true;
                    pd[i][b] = new_pd[b];
                }
            }
        }
    }

    /*
     * 第三遍: 对每个 branch 计算 IPOST
     * IPOST[n] = d ∈ PD[n]-{n}, 且对于所有 p ∈ PD[n]-{n,d}: d ∈ PD[p]
     * 即 d 是 PD 集合中最靠近 n 的那个节点
     */
    /* 先统计 branch 数量 */
    int num_branches = 0;
    for (int i = 0; i < CFG_MAX_NODES; i++) {
        if (visited[i] && nodes[i].is_branch) num_branches++;
    }

    s->branch_pcs  = g_new0(uint32_t, num_branches);
    s->reconv_pcs  = g_new0(uint32_t, num_branches);
    s->num_branches = 0;

    for (int i = 0; i < CFG_MAX_NODES; i++) {
        if (!visited[i] || !nodes[i].is_branch) continue;

        /*
         * 从 PD[i] 中找汇合点: 跳过同样是分支指令的节点.
         * 按 PD 深度降序 (最近者优先), 选第一个非分支节点.
         * 避免聚合点和分支指令重合导致的分叉丢失问题.
         */
        /* 先找出所有候选并按深度排序 */
        int candidates[CFG_MAX_NODES];
        int ncand = 0;
        for (int j = 0; j < CFG_MAX_NODES; j++) {
            if (!visited[j] || j == i) continue;
            int bit = j / 64;
            int off = j % 64;
            if (!(pd[i][bit] & (1ULL << off))) continue;
            candidates[ncand++] = j;
        }

        /* 简单选择排序: 按 PD 深度降序 */
        for (int ci = 0; ci < ncand; ci++) {
            /* 计算深度 */
            int depth = 0;
            for (int b = 0; b < 16; b++) {
                depth += __builtin_popcountll(pd[candidates[ci]][b]);
            }
            /* 插入排序降序 */
            int k = ci;
            while (k > 0) {
                int prev_depth = 0;
                for (int b = 0; b < 16; b++) {
                    prev_depth += __builtin_popcountll(pd[candidates[k-1]][b]);
                }
                if (prev_depth >= depth) break;
                int tmp = candidates[k];
                candidates[k] = candidates[k-1];
                candidates[k-1] = tmp;
                k--;
            }
        }

        /* 选第一个非分支候选 */
        int reconv_idx = -1;
        for (int ci = 0; ci < ncand; ci++) {
            if (!nodes[candidates[ci]].is_branch) {
                reconv_idx = candidates[ci];
                break;
            }
        }
        /* 保底: 用最近候选 (即使是分支) */
        if (reconv_idx < 0 && ncand > 0) {
            reconv_idx = candidates[0];
        }

        if (reconv_idx >= 0) {
            s->branch_pcs[s->num_branches]  = kernel_addr + i * 4;
            s->reconv_pcs[s->num_branches++] = kernel_addr + reconv_idx * 4;
        } else {
            /* 无法确定 IPOST (例如无出口). 用启发式保底 */
            uint32_t inst = vram_readl(s, kernel_addr + i * 4);
            int32_t imm = imm_B(inst);
            uint32_t then_pc = kernel_addr + i * 4 + imm;
            uint32_t else_pc = kernel_addr + i * 4 + 4;
            s->branch_pcs[s->num_branches]  = kernel_addr + i * 4;
            s->reconv_pcs[s->num_branches++] = then_pc < kernel_addr + i * 4 ?
                                               else_pc : then_pc;
        }
    }

    /* 调试: 打印 IPOST 表 */
    for (int i = 0; i < s->num_branches; i++) {
        qemu_log("IPOST: branch=0x%x reconv=0x%x\n",
                 s->branch_pcs[i], s->reconv_pcs[i]);
    }
}

/*
 * gpgpu_core_free_cfg - 释放 CFG 分析分配的内存
 */
static void gpgpu_core_free_cfg(GPGPUState *s)
{
    g_free(s->branch_pcs);
    g_free(s->reconv_pcs);
    s->branch_pcs  = NULL;
    s->reconv_pcs  = NULL;
    s->num_branches = 0;
}

/*
 * gpgpu_core_lookup_reconv - 查表获取 branch PC 的 IPOST
 */
static uint32_t gpgpu_core_lookup_reconv(GPGPUState *s, uint32_t branch_pc)
{
    for (int i = 0; i < s->num_branches; i++) {
        if (s->branch_pcs[i] == branch_pc) {
            return s->reconv_pcs[i];
        }
    }
    return 0;  /* not found */
}

/* LP conversion: quantize float32 → low-precision; reverse direction is identity */

static uint32_t float32_to_bf16(uint32_t val)
{
    return val & 0xFFFF0000;
}

static uint32_t float32_to_e4m3(uint32_t val)
{
    uint32_t sign = val & 0x80000000;
    uint32_t abs_v = val & 0x7FFFFFFF;
    if (abs_v == 0) return 0;
    if (((abs_v >> 23) & 0xFF) == 0xFF) return sign | 0x43E00000;
    int32_t exp_val = ((abs_v >> 23) & 0xFF) - 127;
    uint32_t m24 = ((abs_v >> 23) & 0xFF) == 0 ?
        ((abs_v & 0x7FFFFF) << 1) : (abs_v & 0x7FFFFF) | 0x800000;
    if (exp_val > 8)  return sign | 0x43E00000;
    if (exp_val < -6) return 0;
    uint32_t m4 = (m24 + (1 << 19)) >> 20;
    if (m4 >= 16) { m4 >>= 1; exp_val++; }
    if (exp_val > 8)  return sign | 0x43E00000;
    if (exp_val < -6) return 0;
    uint32_t out_exp = exp_val + 127;
    return sign | (out_exp << 23) | ((m4 & 0x7) << 20);
}

static uint32_t float32_to_e5m2(uint32_t val)
{
    uint32_t sign = val & 0x80000000;
    uint32_t abs_v = val & 0x7FFFFFFF;
    if (abs_v == 0) return 0;
    if (((abs_v >> 23) & 0xFF) == 0xFF) return sign | 0x46FFC000;
    int32_t exp_val = ((abs_v >> 23) & 0xFF) - 127;
    uint32_t m24 = ((abs_v >> 23) & 0xFF) == 0 ?
        ((abs_v & 0x7FFFFF) << 1) : (abs_v & 0x7FFFFF) | 0x800000;
    if (exp_val > 15)  return sign | 0x46FFC000;
    if (exp_val < -14) return 0;
    uint32_t m3 = (m24 + (1 << 20)) >> 21;
    if (m3 >= 8) { m3 >>= 1; exp_val++; }
    if (exp_val > 15)  return sign | 0x46FFC000;
    if (exp_val < -14) return 0;
    uint32_t out_exp = exp_val + 127;
    return sign | (out_exp << 23) | ((m3 & 0x3) << 21);
}

/* E2M1 tables */
static const uint32_t e2m1_table[8] = {
    0x00000000, 0x3F000000, 0x3F800000, 0x3FC00000,
    0x40000000, 0x40400000, 0x40800000, 0x40C00000,
};
static const uint32_t e2m1_thresh[7] = {
    0x3E800000, 0x3F200000, 0x3F600000, 0x3FA00000,
    0x40200000, 0x40600000, 0x41000000,
};

static uint32_t float32_to_e2m1(uint32_t val)
{
    uint32_t sign = val & 0x80000000;
    uint32_t abs_v = val & 0x7FFFFFFF;
    if (abs_v == 0) return 0;
    if (((abs_v >> 23) & 0xFF) >= 0xFE) return sign | 0x40C00000;
    int idx = 7;
    for (int i = 0; i < 7; i++) {
        if (abs_v < e2m1_thresh[i]) { idx = i; break; }
    }
    return sign | e2m1_table[idx];
}

void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc,
                          uint32_t thread_id_base, const uint32_t block_id[3],
                          uint32_t num_threads,
                          uint32_t warp_id, uint32_t block_id_linear,
                          const uint32_t *kernel_args,
                          uint32_t num_kernel_args)
{
    memset(warp, 0, sizeof(*warp));
    warp->warp_id = warp_id;
    warp->thread_id_base = thread_id_base;
    warp->block_id[0] = block_id[0];
    warp->block_id[1] = block_id[1];
    warp->block_id[2] = block_id[2];

    for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
        GPGPULane *lane = &warp->lanes[i];
        lane->active = (i < num_threads);
        lane->pc = pc;
        lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id, i);

        /* 将内核参数写入 a0-a7 (gpr[10..17]) */
        for (uint32_t k = 0; k < num_kernel_args && k < 8; k++) {
            lane->gpr[10 + k] = kernel_args[k];
        }
    }

    warp->active_mask = (num_threads >= 32) ?
        0xFFFFFFFF : ((1u << num_threads) - 1);
    warp->simt_depth = 0;
}

int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    for (uint32_t cycle = 0; cycle < max_cycles; cycle++) {
        uint32_t curr_pc = warp->lanes[0].pc;

        /* Fetch instruction */
        uint32_t inst = vram_readl(s, curr_pc);
        if (inst == 0x00100073) {
            /* ebreak: stop this warp */
            for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
                if (warp->lanes[i].active) {
                    warp->lanes[i].pc = curr_pc;
                }
            }
            break;
        }

        int rd   = BITS(inst, 11, 7);
        int rs1  = BITS(inst, 19, 15);
        int rs2  = BITS(inst, 24, 20);
        uint8_t opcode = BITS(inst, 6, 0);
        uint8_t funct3 = BITS(inst, 14, 12);
        uint8_t funct7 = BITS(inst, 31, 25);

        /* Per-lane execution, gated by active_mask */
        bool branch_taken = false;
        uint32_t branch_target = curr_pc + 4;
        uint32_t then_mask = 0;
        uint32_t else_mask = 0;

        for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (!(warp->active_mask & (1u << i))) continue;
            GPGPULane *lane = &warp->lanes[i];

            switch (opcode) {
            /* LUI: rd = imm20 << 12 */
            case 0x37:
                lane->gpr[rd] = imm_U(inst);
                break;

            /* AUIPC: rd = pc + (imm20 << 12) */
            case 0x17:
                lane->gpr[rd] = curr_pc + imm_U(inst);
                break;

            /* OP-IMM: rd = rs1 op imm */
            case 0x13: {
                int32_t imm = imm_I(inst);
                uint32_t v1 = lane->gpr[rs1];
                switch (funct3) {
                case 0: lane->gpr[rd] = v1 + imm; break;           /* ADDI */
                case 1: lane->gpr[rd] = v1 << (imm & 0x1F); break;  /* SLLI */
                case 2: lane->gpr[rd] = (int32_t)v1 < imm; break;   /* SLTI */
                case 3: lane->gpr[rd] = v1 < (uint32_t)imm; break;  /* SLTIU */
                case 4: lane->gpr[rd] = v1 ^ imm; break;           /* XORI */
                case 5:                                                  /* SRLI/SRAI */
                    if (funct7 & 0x20) lane->gpr[rd] = (int32_t)v1 >> (imm & 0x1F);
                    else              lane->gpr[rd] = v1 >> (imm & 0x1F);
                    break;
                case 6: lane->gpr[rd] = v1 | imm; break;           /* ORI */
                case 7: lane->gpr[rd] = v1 & imm; break;           /* ANDI */
                }
                break;
            }

            /* OP: rd = rs1 op rs2 (RV32I + M extension) */
            case 0x33: {
                uint32_t v1 = lane->gpr[rs1];
                uint32_t v2 = lane->gpr[rs2];
                if (funct7 == 0x01) {
                    /* M extension */
                    int64_t prod;
                    switch (funct3) {
                    case 0:                                                 /* MUL */
                        lane->gpr[rd] = (int64_t)(int32_t)v1 * (int64_t)(int32_t)v2;
                        break;
                    case 1:                                                 /* MULH */
                        prod = (int64_t)(int32_t)v1 * (int64_t)(int32_t)v2;
                        lane->gpr[rd] = (uint64_t)prod >> 32;
                        break;
                    case 2:                                                 /* MULHSU */
                        prod = (int64_t)(int32_t)v1 * (uint64_t)v2;
                        lane->gpr[rd] = (uint64_t)prod >> 32;
                        break;
                    case 3:                                                 /* MULHU */
                        prod = (uint64_t)v1 * (uint64_t)v2;
                        lane->gpr[rd] = (uint64_t)prod >> 32;
                        break;
                    case 4:                                                 /* DIV */
                        if (v2 == 0) lane->gpr[rd] = 0xFFFFFFFF;
                        else if ((int32_t)v1 == INT32_MIN && (int32_t)v2 == -1) lane->gpr[rd] = v1;
                        else lane->gpr[rd] = (int32_t)v1 / (int32_t)v2;
                        break;
                    case 5:                                                 /* DIVU */
                        if (v2 == 0) lane->gpr[rd] = 0xFFFFFFFF;
                        else lane->gpr[rd] = v1 / v2;
                        break;
                    case 6:                                                 /* REM */
                        if (v2 == 0) lane->gpr[rd] = v1;
                        else if ((int32_t)v1 == INT32_MIN && (int32_t)v2 == -1) lane->gpr[rd] = 0;
                        else lane->gpr[rd] = (int32_t)v1 % (int32_t)v2;
                        break;
                    case 7:                                                 /* REMU */
                        if (v2 == 0) lane->gpr[rd] = v1;
                        else lane->gpr[rd] = v1 % v2;
                        break;
                    }
                } else {
                    /* RV32I ALU operations */
                    switch (funct3) {
                    case 0:                                                 /* ADD/SUB */
                        if (funct7 == 0x00) lane->gpr[rd] = (int32_t)v1 + (int32_t)v2;
                        else if (funct7 == 0x20) lane->gpr[rd] = (int32_t)v1 - (int32_t)v2;
                        break;
                    case 1: lane->gpr[rd] = v1 << (v2 & 0x1F); break;    /* SLL */
                    case 2: lane->gpr[rd] = (int32_t)v1 < (int32_t)v2; break; /* SLT */
                    case 3: lane->gpr[rd] = v1 < v2; break;               /* SLTU */
                    case 4: lane->gpr[rd] = v1 ^ v2; break;               /* XOR */
                    case 5:                                                   /* SRL/SRA */
                        if (funct7 & 0x20) lane->gpr[rd] = (int32_t)v1 >> (v2 & 0x1F);
                        else              lane->gpr[rd] = v1 >> (v2 & 0x1F);
                        break;
                    case 6: lane->gpr[rd] = v1 | v2; break;               /* OR */
                    case 7: lane->gpr[rd] = v1 & v2; break;               /* AND */
                    }
                }
                break;
            }

            /* LOAD: rd = MEM[rs1 + imm] */
            case 0x03: {
                int32_t imm = imm_I(inst);
                uint32_t addr = lane->gpr[rs1] + imm;
                switch (funct3) {
                case 0:                                                     /* LB */
                    lane->gpr[rd] = (addr < s->vram_size) ? SEXT(s->vram_ptr[addr], 8) : 0;
                    break;
                case 1:                                                     /* LH */
                    lane->gpr[rd] = (addr + 2 <= s->vram_size) ? SEXT(vram_readl(s, addr) & 0xFFFF, 16) : 0;
                    break;
                case 2:                                                     /* LW */
                    lane->gpr[rd] = vram_readl(s, addr);
                    break;
                case 4:                                                     /* LBU */
                    lane->gpr[rd] = (addr < s->vram_size) ? s->vram_ptr[addr] : 0;
                    break;
                case 5:                                                     /* LHU */
                    lane->gpr[rd] = (addr + 2 <= s->vram_size) ? (vram_readl(s, addr) & 0xFFFF) : 0;
                    break;
                }
                break;
            }

            /* STORE: MEM[rs1 + imm] = rs2 */
            case 0x23: {
                int32_t imm = imm_S(inst);
                uint32_t addr = lane->gpr[rs1] + imm;
                switch (funct3) {
                case 0:                                                     /* SB */
                    if (addr < s->vram_size) s->vram_ptr[addr] = lane->gpr[rs2] & 0xFF;
                    break;
                case 1:                                                     /* SH */
                    if (addr + 2 <= s->vram_size) memcpy(s->vram_ptr + addr, &lane->gpr[rs2], 2);
                    break;
                case 2:                                                     /* SW */
                    vram_writel(s, addr, lane->gpr[rs2]);
                    break;
                }
                break;
            }

            /* BRANCH (divergence handled after lane loop) */
            case 0x63: {
                uint32_t v1 = lane->gpr[rs1];
                uint32_t v2 = lane->gpr[rs2];
                bool taken = false;
                switch (funct3) {
                case 0: taken = (v1 == v2); break;
                case 1: taken = (v1 != v2); break;
                case 4: taken = ((int32_t)v1 < (int32_t)v2); break;
                case 5: taken = ((int32_t)v1 >= (int32_t)v2); break;
                case 6: taken = (v1 < v2); break;
                case 7: taken = (v1 >= v2); break;
                }
                if (taken) {
                    then_mask |= (1u << i);
                } else {
                    else_mask |= (1u << i);
                }
                break;
            }

            /* JALR */
            case 0x67: {
                int32_t imm = imm_I(inst);
                lane->gpr[rd] = curr_pc + 4;
                branch_taken = true;
                branch_target = (lane->gpr[rs1] + imm) & ~1;
                break;
            }

            /* JAL */
            case 0x6F: {
                int32_t imm = imm_J(inst);
                lane->gpr[rd] = curr_pc + 4;
                branch_taken = true;
                branch_target = curr_pc + imm;
                break;
            }

            /* SYSTEM (CSR) */
            case 0x73: {
                uint32_t csr_num = BITS(inst, 31, 20);
                if (funct3 == 0) {
                    /* ECALL / EBREAK / MRET - handled above by ebreak check */
                } else if (funct3 == 1) { /* CSRRW */
                    uint64_t old = 0;
                    if (csr_num == CSR_MHARTID) old = lane->mhartid;
                    lane->gpr[rd] = old;
                } else if (funct3 == 2) { /* CSRRS */
                    uint64_t old = 0;
                    if (csr_num == CSR_MHARTID) old = lane->mhartid;
                    lane->gpr[rd] = old;
                } else if (funct3 == 3) { /* CSRRC */
                    uint64_t old = 0;
                    if (csr_num == CSR_MHARTID) old = lane->mhartid;
                    lane->gpr[rd] = old;
                } else if (funct3 == 5) { /* CSRRWI */
                    uint64_t old = 0;
                    if (csr_num == CSR_MHARTID) old = lane->mhartid;
                    lane->gpr[rd] = old;
                } else if (funct3 == 6) { /* CSRRSI */
                    uint64_t old = 0;
                    if (csr_num == CSR_MHARTID) old = lane->mhartid;
                    lane->gpr[rd] = old;
                }
                break;
            }

            /* OP-FP (RV32F) */
            case 0x53: {
                switch (funct7) {
                case 0x00:                                                  /* FADD.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_add(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x04:                                                  /* FSUB.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_sub(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x08:                                                  /* FMUL.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_mul(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x0C:                                                  /* FDIV.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_div(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x68:                                                  /* FCVT.S.W */
                    if ((funct3 & 0x7) == 0) {
                        lane->fpr[rd] = int32_to_float32(lane->gpr[rs1], &lane->fp_status);
                    }
                    break;
                case 0x60:                                                  /* FCVT.W.S */
                    {
                        float32 fs1 = make_float32(lane->fpr[rs1]);
                        int rm = rs2;
                        if (rm == 1) {                                      /* RTZ */
                            lane->gpr[rd] = float32_to_int32_round_to_zero(fs1, &lane->fp_status);
                        } else {
                            lane->gpr[rd] = float32_to_int32(fs1, &lane->fp_status);
                        }
                    }
                    break;
                case 0x22:                                                  /* LP: BF16 */
                    if (funct3 == 0) {
                        if (rs2 != 0) lane->fpr[rd] = float32_to_bf16(lane->fpr[rs1]);
                        else lane->fpr[rd] = lane->fpr[rs1];
                    }
                    break;
                case 0x24:                                                  /* LP: E4M3/E5M2 */
                    if (funct3 == 0) {
                        if (rs2 == 1) lane->fpr[rd] = float32_to_e4m3(lane->fpr[rs1]);
                        else if (rs2 == 0) lane->fpr[rd] = lane->fpr[rs1];
                        else if (rs2 == 3) lane->fpr[rd] = float32_to_e5m2(lane->fpr[rs1]);
                        else if (rs2 == 2) lane->fpr[rd] = lane->fpr[rs1];
                    }
                    break;
                case 0x26:                                                  /* LP: E2M1 */
                    if (funct3 == 0) {
                        if (rs2 != 0) lane->fpr[rd] = float32_to_e2m1(lane->fpr[rs1]);
                        else lane->fpr[rd] = lane->fpr[rs1];
                    }
                    break;
                case 0x78:                                                  /* FMV.W.X / FMV.X.W */
                    if (funct3 == 0) {
                        lane->fpr[rd] = lane->gpr[rs1];                     /* fmv.w.x rd, rs1 */
                    }
                    break;
                }
                break;
            }

            default:
                qemu_log_mask(LOG_GUEST_ERROR,
                    "gpgpu_core: unknown opcode 0x%02x at pc=0x%x\n",
                    opcode, curr_pc);
                return -1;
            }

            lane->gpr[0] = 0;
        }

        /* ------------------------------------------------------------------
         * SIMT Reconvergence Check (after instruction execution)
         * With correct IPOST, both paths naturally flow through reconv_pc.
         * First arrival: switch to the other path.
         * Second arrival: cascade-pop all matching entries, skip to reconv+4.
         * ---------------------------------------------------------------- */
        uint32_t next_pc;
        if (warp->simt_depth > 0) {
            GPGPUSIMTEntry *top = &warp->simt_stack[warp->simt_depth - 1];
            if (curr_pc == top->reconverge_pc) {
                if (!top->then_done) {
                    top->then_done = true;
                    warp->active_mask = top->else_mask;
                    next_pc = top->else_pc;
                } else {
                    /*
                     * Cascade pop: all entries whose reconv_pc == curr_pc
                     * have both paths executed. Pop them at once,
                     * accumulating saved_mask, and skip past reconv_pc
                     * (already executed by the last path).
                     */
                    uint32_t acc_mask = 0;
                    while (warp->simt_depth > 0) {
                        GPGPUSIMTEntry *e =
                            &warp->simt_stack[warp->simt_depth - 1];
                        if (e->reconverge_pc != curr_pc) break;
                        if (!e->then_done) break;
                        acc_mask |= e->saved_mask; /* 渐进恢复掩码 */
                        warp->simt_depth--;
                    }
                    if (acc_mask == 0) {
                        acc_mask = top->saved_mask;  /* 至少原先遮罩 */
                    }
                    warp->active_mask = acc_mask;
                    next_pc = curr_pc + 4;  /* 跳过已执行的聚合点指令 */
                }
                goto pc_update;
            }
        }

        /*
         * Normal PC advance:
         *  - BRANCH divergence: push SIMT stack, enter then path
         *  - Normal branch: taken / not taken
         *  - Otherwise: pc + 4
         */
        if (opcode == 0x63 && then_mask != 0 && else_mask != 0) {
            int32_t imm = imm_B(inst);
            uint32_t then_pc = curr_pc + imm;
            uint32_t else_pc = curr_pc + 4;
            if (then_pc != else_pc) {
                /* Divergent branch: push SIMT stack */
                if (warp->simt_depth >= GPGPU_SIMT_STACK_DEPTH) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                        "gpgpu_core: SIMT stack overflow\n");
                    return -1;
                }

                /* Look up correct reconv point from CFG analysis */
                uint32_t reconv = gpgpu_core_lookup_reconv(s, curr_pc);
                if (reconv == 0) {
                    reconv = then_pc < curr_pc ? else_pc : then_pc;
                }

                GPGPUSIMTEntry *entry = &warp->simt_stack[warp->simt_depth++];
                entry->saved_mask    = warp->active_mask;
                entry->then_mask     = then_mask;
                entry->else_mask     = else_mask;
                entry->then_pc       = then_pc;
                entry->else_pc       = else_pc;
                entry->reconverge_pc = reconv;
                entry->then_done     = false;

                /* Enter then path */
                warp->active_mask = then_mask;
                next_pc = then_pc;
                qemu_log("SIMT: divergence push then=%08x else=%08x "
                         "reconv=0x%x pc=0x%x\n",
                         entry->then_mask, entry->else_mask,
                         entry->reconverge_pc, next_pc);
                goto pc_update;
            }
            /* then_pc == else_pc: uniform branch, fall through to normal handling */
        }

        /* Normal branch or no branch */
        if (opcode == 0x63 && (then_mask || else_mask)) {
            branch_taken = (then_mask != 0);
            branch_target = (then_mask != 0) ? (curr_pc + imm_B(inst)) : (curr_pc + 4);
        }
        next_pc = branch_taken ? branch_target : (curr_pc + 4);

pc_update:
        /* Update warp PC for all lanes */
        for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (warp->lanes[i].active) {
                warp->lanes[i].pc = next_pc;
            }
        }
    }
    return 0;
}

int gpgpu_core_exec_kernel(GPGPUState *s)
{
    int threads_per_block = s->kernel.block_dim[0]
                          * s->kernel.block_dim[1]
                          * s->kernel.block_dim[2];
    int num_warps = (threads_per_block + 31) / 32;
    int block_linear = 0;

    /* 从 VRAM 读取内核参数 (最多 8 个 u32) */
    uint32_t kernel_args[8] = {0};
    uint32_t num_args = 0;
    if (s->kernel.kernel_args != 0) {
        for (int i = 0; i < 8; i++) {
            uint32_t addr = (uint32_t)s->kernel.kernel_args + i * 4;
            kernel_args[i] = vram_readl(s, addr);
            num_args++;
        }
    }

    /* build CFG and compute correct reconv points */
    gpgpu_core_build_cfg(s, (uint32_t)s->kernel.kernel_addr);

    for (int bx = 0; bx < s->kernel.grid_dim[0]; bx++) {
        for (int by = 0; by < s->kernel.grid_dim[1]; by++) {
            for (int bz = 0; bz < s->kernel.grid_dim[2]; bz++) {
                uint32_t block_id[3] = {bx, by, bz};

                for (int w = 0; w < num_warps; w++) {
                    int thread_id_base = w * 32;
                    int num_active = MIN(32,
                        threads_per_block - thread_id_base);

                    GPGPUWarp warp;
                    gpgpu_core_init_warp(&warp,
                        (uint32_t)s->kernel.kernel_addr,
                        thread_id_base, block_id,
                        num_active, w, block_linear,
                        kernel_args, num_args);

                    gpgpu_core_exec_warp(s, &warp, 4096);
                }

                block_linear++;
            }
        }
    }

    gpgpu_core_free_cfg(s);
    return 0;
}
