/* Compact decoded x86 IR for the cold execution tier.
 *
 * The IR is produced on demand from guest code bytes, cached in a bounded RAM
 * arena and executed by source/ir_exec.c. It is never written to disk and it is
 * never generated at build time: the build only carries the *addresses* of the
 * functions that are not AOT (IrFnEntry below).
 *
 * Encoding (all words little-endian uint32_t):
 *
 *   core word (always present)
 *     bits  0..7   opcode (IrOp)
 *     bits  8..10  dst register index (or string kind)
 *     bit   11     dst is a memory operand
 *     bits 12..14  src register index
 *     bit   15     src is a memory operand
 *     bits 16..17  operand size: 0 = 8, 1 = 16, 2 = 32
 *     bits 18..19  immediate kind: 0 none, 1 imm8 signed, 2 imm32, 3 imm8 unsigned
 *     bits 20..21  segment: 0 none, 1 fs, 2 gs (gs is rejected by the decoder)
 *     bits 22..23  aux (shift count from cl, string rep kind, ...)
 *     bits 24..31  imm8 payload
 *
 *   address word (present when dst or src is a memory operand)
 *     bits  0..3   base register, 15 = none
 *     bits  4..7   index register, 15 = none
 *     bits  8..9   scale: 0 = 1, 1 = 2, 2 = 4, 3 = 8
 *     bits 10..11  displacement kind: 0 none, 1 disp8 in bits 24..31, 2 disp32 word
 *     bits 24..31  disp8 payload
 *
 *   disp32 word (after the address word, when displacement kind is 2)
 *   imm32 word  (after the address/disp words, when immediate kind is 2)
 *
 * A basic block is a header followed by its instruction stream:
 *   struct IrBlockHeader { start, x86_len, ir_bytes, insn_count }
 * The terminator is always the last instruction in the block; branch
 * displacements are relative to `start + x86_len` (the end of the block).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "x86.h"

/* One function that the interpreter is allowed to enter. `end` is exclusive and
 * must cover every basic block of the function (functions.json uses max(hi)).
 * Extents may overlap when the discovery assigned a shared cold block to more
 * than one function; only exact entry addresses are callable, and the extent
 * stored with that entry is the one used for its frame. */
typedef struct { uint32_t addr, end; } IrFnEntry;

typedef enum {
    IR_OP_NOP = 0,
    IR_OP_MOV,
    IR_OP_MOVZX8, IR_OP_MOVZX16, IR_OP_MOVSX8, IR_OP_MOVSX16,
    IR_OP_LEA,
    IR_OP_ADD, IR_OP_OR, IR_OP_ADC, IR_OP_SBB, IR_OP_AND, IR_OP_SUB,
    IR_OP_XOR, IR_OP_CMP, IR_OP_TEST,
    IR_OP_INC, IR_OP_DEC, IR_OP_NEG, IR_OP_NOT,
    IR_OP_SHL, IR_OP_SHR, IR_OP_SAR, IR_OP_ROL, IR_OP_ROR, IR_OP_RCL,
    IR_OP_RCR, IR_OP_SHLD, IR_OP_SHRD,
    IR_OP_IMUL1, IR_OP_IMUL2, IR_OP_IMUL3, IR_OP_MUL1, IR_OP_DIV, IR_OP_IDIV,
    IR_OP_CDQ, IR_OP_CWDE, IR_OP_CBW, IR_OP_CWD,
    IR_OP_BSWAP, IR_OP_BSF, IR_OP_BSR,
    IR_OP_BT, IR_OP_BTS, IR_OP_BTR, IR_OP_BTC,
    IR_OP_XCHG, IR_OP_XADD, IR_OP_CMPXCHG, IR_OP_CMPXCHG8B,
    IR_OP_PUSH, IR_OP_POP, IR_OP_PUSHA, IR_OP_POPA, IR_OP_PUSHFD,
    IR_OP_POPFD, IR_OP_LEAVE,
    IR_OP_SAHF, IR_OP_LAHF, IR_OP_CLC, IR_OP_STC, IR_OP_CMC,
    IR_OP_CLD, IR_OP_STD,
    IR_OP_JMP, IR_OP_JMP_IND, IR_OP_CALL, IR_OP_CALL_IND,
    IR_OP_RET, IR_OP_LOOP, IR_OP_LOOPE, IR_OP_LOOPNE, IR_OP_JECXZ,
    IR_OP_JCC_BASE,                 /* +0..15: jo .. jg */
    IR_OP_SETCC_BASE = IR_OP_JCC_BASE + 16, /* +0..15 */
    IR_OP_CMOVCC_BASE = IR_OP_SETCC_BASE + 16, /* +0..15 */
    IR_OP_STRING = IR_OP_CMOVCC_BASE + 16,
    IR_OP_X87, IR_OP_SSE,
    IR_OP_CPUID, IR_OP_RDTSC, IR_OP_INT3, IR_OP_INT, IR_OP_XLATB,
    IR_OP_COUNT
} IrOp;

#define IR_OP(w)      ((unsigned)((w) & 0xffu))
#define IR_DST(w)     ((unsigned)(((w) >> 8) & 7u))
#define IR_DSTM(w)    ((unsigned)(((w) >> 11) & 1u))
#define IR_SRC(w)     ((unsigned)(((w) >> 12) & 7u))
#define IR_SRCM(w)    ((unsigned)(((w) >> 15) & 1u))
#define IR_SIZE(w)    ((unsigned)(((w) >> 16) & 3u))
#define IR_IMMK(w)    ((unsigned)(((w) >> 18) & 3u))
#define IR_SEG(w)     ((unsigned)(((w) >> 20) & 3u))
#define IR_AUX(w)     ((unsigned)(((w) >> 22) & 3u))
#define IR_IMM8(w)    ((int32_t)(int8_t)(uint8_t)((w) >> 24))
#define IR_IMM8U(w)   ((uint32_t)((w) >> 24))
#define IR_HAS_ADDR(w) (IR_DSTM(w) | IR_SRCM(w))

#define IR_A_BASE(a)  ((unsigned)((a) & 0xfu))
#define IR_A_INDEX(a) ((unsigned)(((a) >> 4) & 0xfu))
#define IR_A_SCALE(a) ((unsigned)(((a) >> 8) & 3u))
#define IR_A_DISPK(a) ((unsigned)(((a) >> 10) & 3u))
#define IR_A_DISP8(a) ((int32_t)(int8_t)(uint8_t)((a) >> 24))

#define IR_SIZE_8  0u
#define IR_SIZE_16 1u
#define IR_SIZE_32 2u

#define IR_IMM_NONE 0u
#define IR_IMM_S8   1u
#define IR_IMM_32   2u
#define IR_IMM_U8   3u

#define IR_SEG_NONE 0u
#define IR_SEG_FS   1u
#define IR_SEG_GS   2u

/* Aux for shifts: 0 = immediate/1, 1 = count from cl. */
#define IR_AUX_SHIFT_CL 1u
/* Aux for IR_OP_STRING: rep kind 0 none, 1 rep, 2 repe/repz, 3 repne/repnz. */

/* Relative-branch displacements are measured from start + x86_len. */
typedef struct {
    uint32_t start;      /* guest address of the first x86 instruction */
    uint32_t end_ip;     /* guest address just past the last x86 instruction */
    uint16_t ir_bytes;   /* header + IR stream, 4-byte aligned */
    uint16_t insn_count; /* x86 instructions in the block */
    uint16_t term_op;    /* IR opcode that terminated the block (0 if none) */
    char     error[56];  /* soft-decode failure ("IR: unsupported opcode ...") */
} IrBlockInfo;

/* ---- decoder / cache (source/ir_decode.c) ---- */

/* Decode one basic block at `start`. It must not run past `func_end`.
 * Hard mode (soft == 0) raises rt_unimpl with the exact fault text; soft mode
 * returns 0 and fills info->error instead. On success returns the same
 * info->ir_bytes. `buf` may be NULL when only the size is wanted. */
size_t n3ds_ir_encode_block(CPU *c, uint32_t start, uint32_t func_end,
                            uint32_t *buf, size_t words, IrBlockInfo *info,
                            int soft);

/* Bind the sorted, non-overlapping table of interpreter function ranges. The
 * table must outlive the session. Re-binding replaces the previous table. */
int n3ds_ir_bind(const IrFnEntry *fns, unsigned count);
int n3ds_ir_has(uint32_t addr);
/* Exclusive end of the IR function containing `addr`, or 0. */
uint32_t n3ds_ir_end(uint32_t addr);

/* Bounded decode cache. `bytes` is rounded up to a power of two internally;
 * 0 selects N3DS_IR_DEFAULT_CACHE. Not thread-safe: the runtime pins guest
 * threads to one host thread. */
#define N3DS_IR_DEFAULT_CACHE (1u << 20)
int n3ds_ir_cache_init(size_t bytes);
void n3ds_ir_cache_reset(void);
typedef struct {
    uint64_t blocks, bytes, hits, misses, flushes, evicted;
} IrCacheStats;
void n3ds_ir_cache_stats(IrCacheStats *out);

/* ---- interpreter (source/ir_exec.c) ---- */

/* One cached block handed to the interpreter. `stream` points into the decode
 * arena and is valid until the arena is flushed. */
typedef struct {
    uint32_t start;
    uint32_t x86_len;
    uint32_t insn_count;
    uint32_t stream_words;
    const uint32_t *stream;
} IrExecBlock;
int n3ds_ir_fetch_block(CPU *c, uint32_t start, uint32_t func_end, IrExecBlock *out);

/* Execute one IR function. `func_end` is exclusive. Faults use the same
 * rt_unimpl / n3ds_lift_exit path as AOT code. Returns when the function
 * returns; never returns to normal flow after a guest fault. */
void n3ds_ir_call(CPU *c, uint32_t addr);
void n3ds_ir_run(CPU *c, uint32_t addr, uint32_t func_end);

/* Cooperative time slicing between guest IR blocks.  Both executors (slice IR
 * and the all-functions container) run long guest loops that make no import
 * call; without a tick such a thread owns the CPU forever and starves every
 * other guest thread.  win32_thread_init() installs the scheduler's tick; a
 * NULL hook disables slicing entirely, and the counter only costs one compare
 * per block. */
#define N3DS_IR_YIELD_INTERVAL 4096u
extern void (*n3ds_ir_yield_hook)(void);

/* Runtime dispatch (implemented in n3ds_lift_runtime.c): resolve AOT, IR or
 * import and invoke. Returns 0 when invoked, -1 when the address is neither
 * (the caller then raises the appropriate fault). */
int n3ds_rt_dispatch(CPU *c, uint32_t addr);
