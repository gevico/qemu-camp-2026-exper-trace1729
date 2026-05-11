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
                          uint32_t warp_id, uint32_t block_id_linear)
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
    }

    warp->active_mask = (num_threads >= 32) ?
        0xFFFFFFFF : ((1u << num_threads) - 1);
}

int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    for (uint32_t cycle = 0; cycle < max_cycles; cycle++) {
        uint32_t inst = vram_readl(s, warp->lanes[0].pc);

        if (inst == 0x00100073) break; /* ebreak */

        int rd   = BITS(inst, 11, 7);
        int rs1  = BITS(inst, 19, 15);
        int rs2  = BITS(inst, 24, 20);
        uint8_t opcode = BITS(inst, 6, 0);
        uint8_t funct3 = BITS(inst, 14, 12);
        uint8_t funct7 = BITS(inst, 31, 25);

        /* Per-lane execution */
        bool branch_taken = false;
        uint32_t branch_target = warp->lanes[0].pc + 4;

        for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (!warp->lanes[i].active) continue;
            GPGPULane *lane = &warp->lanes[i];

            switch (opcode) {
            /* LUI: rd = imm20 << 12 */
            case 0x37:
                lane->gpr[rd] = imm_U(inst);
                break;

            /* AUIPC: rd = pc + (imm20 << 12) */
            case 0x17:
                lane->gpr[rd] = lane->pc + imm_U(inst);
                break;

            /* OP-IMM: rd = rs1 op imm */
            case 0x13: {
                int32_t imm = imm_I(inst);
                uint32_t v1 = lane->gpr[rs1];
                switch (funct3) {
                case 0: lane->gpr[rd] = v1 + imm; break;          /* ADDI */
                case 1: lane->gpr[rd] = v1 << (imm & 0x1F); break; /* SLLI */
                case 2: lane->gpr[rd] = (int32_t)v1 < imm; break;  /* SLTI */
                case 3: lane->gpr[rd] = v1 < (uint32_t)imm; break; /* SLTIU */
                case 4: lane->gpr[rd] = v1 ^ imm; break;          /* XORI */
                case 5:                                                 /* SRLI/SRAI */
                    if (funct7 & 0x20) lane->gpr[rd] = (int32_t)v1 >> (imm & 0x1F);
                    else              lane->gpr[rd] = v1 >> (imm & 0x1F);
                    break;
                case 6: lane->gpr[rd] = v1 | imm; break;          /* ORI */
                case 7: lane->gpr[rd] = v1 & imm; break;          /* ANDI */
                }
                break;
            }

            /* OP: rd = rs1 op rs2 */
            case 0x33: {
                uint32_t v1 = lane->gpr[rs1];
                uint32_t v2 = lane->gpr[rs2];
                switch (funct3) {
                case 0: /* ADD/SUB/MUL */
                    if (funct7 == 0x00)
                        lane->gpr[rd] = (int32_t)v1 + (int32_t)v2;
                    else if (funct7 == 0x20)
                        lane->gpr[rd] = (int32_t)v1 - (int32_t)v2;
                    break;
                case 1: lane->gpr[rd] = v1 << (v2 & 0x1F); break;   /* SLL */
                case 2: lane->gpr[rd] = (int32_t)v1 < (int32_t)v2; break; /* SLT */
                case 3: lane->gpr[rd] = v1 < v2; break;              /* SLTU */
                case 4: lane->gpr[rd] = v1 ^ v2; break;              /* XOR */
                case 5: /* SRL/SRA */
                    if (funct7 & 0x20) lane->gpr[rd] = (int32_t)v1 >> (v2 & 0x1F);
                    else              lane->gpr[rd] = v1 >> (v2 & 0x1F);
                    break;
                case 6: lane->gpr[rd] = v1 | v2; break;              /* OR */
                case 7: lane->gpr[rd] = v1 & v2; break;              /* AND */
                }
                break;
            }

            /* LOAD: rd = MEM[rs1 + imm] */
            case 0x03: {
                int32_t imm = imm_I(inst);
                uint32_t addr = lane->gpr[rs1] + imm;
                switch (funct3) {
                case 0: /* LB */
                    lane->gpr[rd] = (addr < s->vram_size) ? SEXT(s->vram_ptr[addr], 8) : 0;
                    break;
                case 1: /* LH */
                    lane->gpr[rd] = (addr + 2 <= s->vram_size) ? SEXT(vram_readl(s, addr) & 0xFFFF, 16) : 0;
                    break;
                case 2: /* LW */
                    lane->gpr[rd] = vram_readl(s, addr);
                    break;
                case 4: /* LBU */
                    lane->gpr[rd] = (addr < s->vram_size) ? s->vram_ptr[addr] : 0;
                    break;
                case 5: /* LHU */
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
                case 0: /* SB */
                    if (addr < s->vram_size) s->vram_ptr[addr] = lane->gpr[rs2] & 0xFF;
                    break;
                case 1: /* SH */
                    if (addr + 2 <= s->vram_size) memcpy(s->vram_ptr + addr, &lane->gpr[rs2], 2);
                    break;
                case 2: /* SW */
                    vram_writel(s, addr, lane->gpr[rs2]);
                    break;
                }
                break;
            }

            /* BRANCH */
            case 0x63: {
                int32_t imm = imm_B(inst);
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
                    branch_taken = true;
                    branch_target = lane->pc + imm;
                }
                break;
            }

            /* JALR */
            case 0x67: {
                int32_t imm = imm_I(inst);
                lane->gpr[rd] = lane->pc + 4;
                branch_taken = true;
                branch_target = (lane->gpr[rs1] + imm) & ~1;
                break;
            }

            /* JAL */
            case 0x6F: {
                int32_t imm = imm_J(inst);
                lane->gpr[rd] = lane->pc + 4;
                branch_taken = true;
                branch_target = lane->pc + imm;
                break;
            }

            /* SYSTEM (CSR, EBREAK, ECALL, MRET) */
            case 0x73: {
                uint32_t csr_num = BITS(inst, 31, 20);
                if (funct3 == 0) {
                    /* ECALL (0x00000000) / EBREAK (0x00100073) / MRET (0x30200073) */
                    /* handled above by inst == 0x00100073 */
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
                case 0x00: /* FADD.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_add(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x04: /* FSUB.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_sub(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x08: /* FMUL.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_mul(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x0C: /* FDIV.S */
                    if (funct3 == 0) {
                        float32 vs1 = make_float32(lane->fpr[rs1]);
                        float32 vs2 = make_float32(lane->fpr[rs2]);
                        lane->fpr[rd] = float32_div(vs1, vs2, &lane->fp_status);
                    }
                    break;
                case 0x68: /* FCVT.S.W: funct5=11010, fmt=00 → funct7=0x68 */
                    if ((funct3 & 0x7) == 0) {
                        lane->fpr[rd] = int32_to_float32(
                            lane->gpr[rs1], &lane->fp_status);
                    }
                    break;
                case 0x60: /* FCVT.W.S: funct5=11000, fmt=00 → funct7=0x60 */
                    {
                        float32 fs1 = make_float32(lane->fpr[rs1]);
                        int rm = rs2;
                        if (rm == 1) { /* RTZ */
                            lane->gpr[rd] = float32_to_int32_round_to_zero(
                                fs1, &lane->fp_status);
                        } else {
                            lane->gpr[rd] = float32_to_int32(fs1,
                                &lane->fp_status);
                        }
                    }
                    break;
                case 0x22: /* LP: BF16: funct5=01000, fmt=10 */
                    if (funct3 == 0) {
                        if (rs2 != 0) {
                            lane->fpr[rd] = float32_to_bf16(lane->fpr[rs1]);
                        } else {
                            lane->fpr[rd] = lane->fpr[rs1];
                        }
                    }
                    break;
                case 0x24: /* LP: E4M3/E5M2: funct5=01001, fmt=00 */
                    if (funct3 == 0) {
                        if (rs2 == 1) {
                            lane->fpr[rd] = float32_to_e4m3(lane->fpr[rs1]);
                        } else if (rs2 == 0) {
                            lane->fpr[rd] = lane->fpr[rs1];
                        } else if (rs2 == 3) {
                            lane->fpr[rd] = float32_to_e5m2(lane->fpr[rs1]);
                        } else if (rs2 == 2) {
                            lane->fpr[rd] = lane->fpr[rs1];
                        }
                    }
                    break;
                case 0x26: /* LP: E2M1: funct5=01001, fmt=10 */
                    if (funct3 == 0) {
                        if (rs2 != 0) {
                            lane->fpr[rd] = float32_to_e2m1(lane->fpr[rs1]);
                        } else {
                            lane->fpr[rd] = lane->fpr[rs1];
                        }
                    }
                    break;
                case 0x78: /* FMV.W.X / FMV.X.W */
                    if (funct3 == 0) {
                        lane->fpr[rd] = lane->gpr[rs1]; /* fmv.w.x rd, rs1 */
                    }
                    break;
                }
                break;
            }

            default:
                qemu_log_mask(LOG_GUEST_ERROR,
                    "gpgpu_core: unknown opcode 0x%02x at pc=0x%x\n",
                    opcode, lane->pc);
                return -1;
            }

            lane->gpr[0] = 0; /* x0 is always zero */
        }

        /* Advance PC (lockstep: all active lanes share next PC) */
        uint32_t next_pc = branch_taken ? branch_target :
                           (warp->lanes[0].pc + 4);
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
                        num_active, w, block_linear);

                    gpgpu_core_exec_warp(s, &warp, 4096);
                }

                block_linear++;
            }
        }
    }

    return 0;
}
