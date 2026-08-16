#if defined(__aarch64__) && defined(HAVE_PPC_JIT)

#include "JitArm64.h"
#include "Arm64Emitter.h"

#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#if defined(__APPLE__)
#include <pthread.h>
#endif
#include "../../../../OSD/Logger.h"
#define JIT_LOG(...) DebugLog("[JIT] " __VA_ARGS__)

namespace {

// Apple Silicon enforces write-or-execute permissions for MAP_JIT pages on a
// per-thread basis. This is the same pattern used by established Libretro
// dynarecs such as Flycast and ParaLLEl N64. Other ARM64 platforms keep their
// existing memory behavior.
static inline void set_jit_executable(bool executable)
{
#if defined(__APPLE__)
    if (__builtin_available(macOS 11.0, *))
        pthread_jit_write_protect_np(executable ? 1 : 0);
#else
    (void)executable;
#endif
}

class ScopedJitWrite
{
public:
    ScopedJitWrite() { set_jit_executable(false); }
    ~ScopedJitWrite() { set_jit_executable(true); }

    ScopedJitWrite(const ScopedJitWrite &) = delete;
    ScopedJitWrite &operator=(const ScopedJitWrite &) = delete;
};

} // namespace

// ---------------------------------------------------------------------------
// Function pointer table indices (8 bytes each, packed at m_code_buf[0])
// ---------------------------------------------------------------------------
enum {
    FN_JIT_READ8       = 0,
    FN_JIT_READ16      = 1,
    FN_JIT_READ32      = 2,
    FN_JIT_READ64      = 3,
    FN_JIT_WRITE8      = 4,
    FN_JIT_WRITE16     = 5,
    FN_JIT_WRITE32     = 6,
    FN_JIT_WRITE64     = 7,
    FN_JIT_READ_TBL    = 8,
    FN_JIT_READ_TBU    = 9,
    FN_JIT_FRES        = 10,
    FN_JIT_FRSQRTE     = 11,
    FN_PPC_DISPATCH    = 12,
    FN_JIT_FRSP        = 13,
    // indices 14-15 reserved
};
static_assert(FN_PPC_DISPATCH < JitArm64::FN_TABLE_ENTRIES, "FN table overflow");

// Pointer to the function table base (set when the code buffer is allocated).
// Used by emit_call() to choose ADRP+LDR_X over MOV_X64.
static uint8_t *g_fn_tbl = nullptr;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
JitArm64 &JitArm64::get()
{
    static JitArm64 s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// Code buffer allocation
// ---------------------------------------------------------------------------
bool JitArm64::init()
{
    if (m_code_buf) return true;    // already initialised
    if (m_init_attempted) return false;
    m_init_attempted = true;

#if defined(__APPLE__)
    // RetroArch's macOS executable carries com.apple.security.cs.allow-jit.
    // MAP_JIT plus per-thread write protection is mandatory on Apple Silicon.
    void *p = mmap(nullptr, CODE_BUF_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        ErrorLog("[JIT] MAP_JIT allocation failed (errno=%d); using the PowerPC interpreter.\n", errno);
        return false;
    }
    m_code_buf  = (uint8_t *)p;
    m_write_buf = m_code_buf;
    m_dual_map  = false;
    JIT_LOG("init: MAP_JIT mmap OK, buf=%p size=%zuMB", m_code_buf, CODE_BUF_SIZE >> 20);
    InfoLog("[JIT] Apple Silicon PowerPC recompiler enabled (MAP_JIT, %zu MB code cache).\n",
            CODE_BUF_SIZE >> 20);
#else
    // Try RWX mapping first (works on RPi5/Linux without selinux restrictions)
    void *p = mmap(nullptr, CODE_BUF_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p != MAP_FAILED) {
        m_code_buf  = (uint8_t *)p;
        m_write_buf = m_code_buf;
        m_dual_map  = false;
        JIT_LOG("init: RWX mmap OK, buf=%p size=%zuMB", m_code_buf, CODE_BUF_SIZE >> 20);
    } else {
        // Fallback: allocate RW, compile blocks there, mprotect to RX before execution.
        // On Android 10+ this is the only reliable approach.
        JIT_LOG("init: RWX mmap failed (errno=%d), trying RW+mprotect fallback", errno);
        p = mmap(nullptr, CODE_BUF_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            JIT_LOG("init: RW mmap also failed (errno=%d) — JIT disabled", errno);
            return false;
        }
        m_write_buf = (uint8_t *)p;
        m_code_buf  = m_write_buf;
        m_dual_map  = false;
        JIT_LOG("init: RW mmap OK, buf=%p — will mprotect RX before exec", m_code_buf);
    }
#endif

    ScopedJitWrite write_scope;

    // Fill function pointer table at the very start of the write buffer.
    void **tbl = (void **)m_write_buf;
    tbl[FN_JIT_READ8]    = (void *)&jit_read8;
    tbl[FN_JIT_READ16]   = (void *)&jit_read16;
    tbl[FN_JIT_READ32]   = (void *)&jit_read32;
    tbl[FN_JIT_READ64]   = (void *)&jit_read64;
    tbl[FN_JIT_WRITE8]   = (void *)&jit_write8;
    tbl[FN_JIT_WRITE16]  = (void *)&jit_write16;
    tbl[FN_JIT_WRITE32]  = (void *)&jit_write32;
    tbl[FN_JIT_WRITE64]  = (void *)&jit_write64;
    tbl[FN_JIT_READ_TBL] = (void *)&jit_read_tbl;
    tbl[FN_JIT_READ_TBU] = (void *)&jit_read_tbu;
    tbl[FN_JIT_FRES]     = (void *)&jit_fres;
    tbl[FN_JIT_FRSQRTE]  = (void *)&jit_frsqrte;
    tbl[FN_PPC_DISPATCH] = (void *)&ppc_dispatch_opcode;
    tbl[FN_JIT_FRSP]     = (void *)&jit_frsp;
    g_fn_tbl = m_write_buf;

    // Emit call stubs immediately after the table. Each stub is 3 instructions
    // (12 bytes): ADRP X16, #0 + LDR X16, [X16, #i*8] + BR X16. Since both the
    // stub and the table are within the first 4 KB of the code buffer (which is
    // page-aligned by mmap), ADRP page_off=0 always yields the table base.
    // JIT call sites use BL stub (1 instruction) instead of ADRP+LDR+BLR (3).
    for (size_t i = 0; i < FN_TABLE_ENTRIES; i++) {
        uint32_t *wp = (uint32_t *)(m_write_buf + FN_TABLE_BYTES + i * 12);
        wp[0] = 0x90000010u;                           // ADRP X16, #0
        wp[1] = 0xF9400210u | (uint32_t)(i << 10);    // LDR  X16, [X16, #i*8]
        wp[2] = 0xD61F0200u;                           // BR   X16
    }
    __builtin___clear_cache((char *)m_code_buf,
                            (char *)m_code_buf + BLOCK_START);
    m_code_pos = BLOCK_START;  // blocks start after table + stubs
    return true;
}

void JitArm64::shutdown()
{
    if (m_code_buf) {
        munmap(m_code_buf, CODE_BUF_SIZE);
        m_code_buf  = nullptr;
        m_write_buf = nullptr;
    }
    m_cache.clear();
    m_code_pos = 0;
    m_init_attempted = false;
}

void JitArm64::flush()
{
    m_cache.clear();
    m_fixups.clear();
    // Preserve the function pointer table and call stubs at offset 0; blocks restart after.
    m_code_pos = (g_fn_tbl != nullptr) ? BLOCK_START : 0;
    memset(m_fast_cache, 0, sizeof(m_fast_cache));
    memset(m_code_pages,  0, sizeof(m_code_pages));
}

void JitArm64::invalidate_containing(uint32_t addr)
{
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        const JitBlock &blk = it->second;
        if (addr >= blk.start_pc && addr < blk.end_pc) {
            uint32_t slot = (blk.start_pc >> 2) & FAST_CACHE_MASK;
            if (m_fast_cache[slot] == &blk)
                m_fast_cache[slot] = nullptr;
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
    // Leave m_code_pages bits set — other blocks may still cover this page.
    // Pages are only cleared by flush().
}

void JitArm64::smc_write(uint32_t addr)
{
    if ((addr >> 24) != 0u) return;                          // not work RAM
    if (!m_code_pages[(addr >> 12) & (CODE_PAGE_COUNT - 1)]) return; // page has no JIT blocks
    // Flush the entire cache on SMC to prevent stale chained branches from
    // executing invalidated blocks. Fine-grained invalidation (invalidate_containing)
    // leaves baked B instructions in surviving blocks pointing at evicted code.
    flush();
}

// ---------------------------------------------------------------------------
// Block lookup
// ---------------------------------------------------------------------------
JitBlock *JitArm64::get_or_compile(uint32_t pc)
{
    // Fast path: direct-mapped cache (avoids hash map for hot blocks)
    uint32_t slot = (pc >> 2) & FAST_CACHE_MASK;
    JitBlock *blk = m_fast_cache[slot];
    if (blk && blk->start_pc == pc) {
        m_stats.fast_hits++;
        m_stats.block_executions++;
        return blk;
    }

    // Slow path: hash map lookup or compilation
    auto it = m_cache.find(pc);
    if (it != m_cache.end()) {
        m_fast_cache[slot] = &it->second;
        m_stats.block_executions++;
        return &it->second;
    }
    blk = compile(pc);
    if (blk) {
        m_fast_cache[slot] = blk;
        m_stats.block_executions++;
    } else {
        m_stats.compile_failures++;
    }
    return blk;
}

// ---------------------------------------------------------------------------
// Helpers used during compilation
// ---------------------------------------------------------------------------

// Limits
static constexpr int MAX_BLOCK_INSTS = 128;

// ARM64 register assignments:
//   X19  = pointer to PPC_REGS (callee-saved, set in prologue)
//   W0-W4 = scratch for computations
//   X16  = scratch for function addresses (caller-saved, IP0)
//
// Struct field offsets computed via offsetof()
static int OFF_R;
static int OFF_PC;
static int OFF_NPC;
static int OFF_LR;
static int OFF_CTR;
static int OFF_XER;
static int OFF_XER_CA;
static int OFF_MSR;
static int OFF_CR;
static int OFF_ICOUNT;
static int OFF_FPR;
static int OFF_SPRG;
static int OFF_DEC;
static int OFF_FPSCR;
static int OFF_SRR0;
static int OFF_SRR1;
static int OFF_DEC_TRIGGER;
static int OFF_INT_PENDING;
static int OFF_SR;
static int OFF_DSISR;
static int OFF_DAR;
static int OFF_RAM_PTR;

static bool g_offsets_computed = false;

static void compute_offsets()
{
    if (g_offsets_computed) return;
    PPC_REGS *dummy = nullptr;
#define OFF(f) (int)((uint8_t*)&dummy->f - (uint8_t*)dummy)
    OFF_R      = OFF(r[0]);
    OFF_PC     = OFF(pc);
    OFF_NPC    = OFF(npc);
    OFF_LR     = OFF(lr);
    OFF_CTR    = OFF(ctr);
    OFF_XER    = OFF(xer);
    OFF_XER_CA = OFF(xer_ca);
    OFF_MSR    = OFF(msr);
    OFF_CR     = OFF(cr[0]);
    OFF_ICOUNT = OFF(icount);
    OFF_FPR    = OFF(fpr[0]);
    OFF_SPRG   = OFF(sprg[0]);
    OFF_DEC    = OFF(dec);
    OFF_FPSCR  = OFF(fpscr);
    OFF_SRR0        = OFF(srr0);
    OFF_SRR1        = OFF(srr1);
    OFF_DEC_TRIGGER = OFF(dec_trigger_cycle);
    OFF_INT_PENDING = OFF(interrupt_pending);
    OFF_SR          = OFF(sr[0]);
    OFF_DSISR       = OFF(dsisr);
    OFF_DAR         = OFF(dar);
    OFF_RAM_PTR     = OFF(ram_ptr);
#undef OFF
    g_offsets_computed = true;
}

// X19 register index (holds PPC_REGS*)
static constexpr int PPC_PTR = 19;

// Scratch registers (caller-saved, safe to use freely between C calls)
static constexpr int W0 = 0, W1 = 1, W2 = 2, W3 = 3, W4 = 4;
static constexpr int X16 = 16;   // IP0: scratch for call targets

// FP scratch registers (D0-D7 are caller-saved; we use D0-D2 for JIT ops)
static constexpr int D0 = 0, D1 = 1, D2 = 2;

// ---------------------------------------------------------------------------
// Emit helpers
// ---------------------------------------------------------------------------

// Load a PPC GPR into a W register
static void emit_load_gpr(Arm64Emitter &e, int dst_Wn, int ppc_reg)
{
    int off = OFF_R + ppc_reg * 4;
    if (off < 16384)
        e.LDR_W(dst_Wn, PPC_PTR, off);
    else {
        e.MOV_W32(dst_Wn, off);
        // Use extended register addressing: LDR Wt, [X19, X0] not easily encoded; just use LDUR with offset
        // For large offsets: add base to offset reg, LDR with register offset
        // Since OFF_R + 31*4 = at most ~128+124 = ~252 bytes, we're always under 16384. Safe.
    }
}

// Store a W register into a PPC GPR
static void emit_store_gpr(Arm64Emitter &e, int src_Wn, int ppc_reg)
{
    int off = OFF_R + ppc_reg * 4;
    e.STR_W(src_Wn, PPC_PTR, off);
}

// Load a PPC FPR (stored as 64-bit double) into an ARM FP register
static void emit_load_fpr(Arm64Emitter &e, int dst_Dd, int ppc_fpr)
{
    int off = OFF_FPR + ppc_fpr * 8;
    e.LDR_D(dst_Dd, PPC_PTR, (uint32_t)off);
}

// Store an ARM FP register into a PPC FPR slot
static void emit_store_fpr(Arm64Emitter &e, int src_Dd, int ppc_fpr)
{
    int off = OFF_FPR + ppc_fpr * 8;
    e.STR_D(src_Dd, PPC_PTR, (uint32_t)off);
}

// Check if a 32-bit mask is a single contiguous run of 1-bits (possibly wrapping) and, if so,
// compute the ARM64 bitmask immediate fields immr and imms.
// Returns false for 0 or 0xFFFFFFFF (all-zeros and all-ones are not encodable).
// Formula: immr = (32 - run_start) & 31,  imms = popcount(mask) - 1.
// run_start is the lowest bit of the run for non-wrapping masks, or (gap_end+1) for wrapping ones.
static bool bitmask_imm32(uint32_t mask, int *immr_out, int *imms_out)
{
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    int ones = __builtin_popcount(mask);
    // A valid bitmask has exactly 2 bit-transitions in a circular scan.
    // Count 0→1 transitions (= number of separate runs of 1s) by checking each edge.
    int runs = __builtin_popcount(mask & ~(mask >> 1 | mask << 31));
    if (runs != 1) return false;  // more than one disjoint run

    bool wrapping = (mask & 1u) && (mask >> 31);
    int run_start;
    if (!wrapping) {
        run_start = __builtin_ctz(mask);
    } else {
        int k = __builtin_ctz(~mask);   // start of the 0-gap
        run_start = k + (32 - ones);    // first 1-bit of the upper portion
    }
    *immr_out = (32 - run_start) & 31;
    *imms_out = ones - 1;
    return true;
}

// Emit: Wd = sign_extend_16(simm16)  [1-2 instructions]
static void emit_load_simm16(Arm64Emitter &e, int Wd, int16_t simm)
{
    uint32_t v = (uint32_t)(int32_t)simm;
    e.MOV_W32(Wd, v);
}

// Add simm16 to Wn and store in Wd, keeping Wn unchanged
static void emit_add_simm16(Arm64Emitter &e, int Wd, int Wn, int16_t simm)
{
    if (simm == 0) {
        if (Wd != Wn) e.MOV_W(Wd, Wn);
        return;
    }
    int v = simm;
    if (v > 0 && v <= 4095) {
        e.ADD_W_IMM(Wd, Wn, v);
    } else if (v < 0 && -v <= 4095) {
        e.SUB_W_IMM(Wd, Wn, (uint32_t)(-v));
    } else if (v > 0 && (v & 0xFFF) == 0) {
        e.ADD_W_IMM(Wd, Wn, (uint32_t)v >> 12, 1);   // v is a multiple of 4096
    } else if (v < 0 && ((-v) & 0xFFF) == 0) {
        e.SUB_W_IMM(Wd, Wn, (uint32_t)(-v) >> 12, 1);
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.ADD_W(Wd, Wn, W1);
    }
}

// Update CR field crfD after a signed comparison (flags already set by CMP/SUBS)
// SO (XER bit 31) is always 0 in JIT-compiled code: all OE-form arithmetic ops
// are handled with OE ignored, so SO/OV are never set by arithmetic.  mtspr XER
// with SO=1 is possible but reading CR0[SO] after it is vanishingly rare in game
// code — consistent with the existing OE-ignored compromise.
static void emit_cr_from_flags_signed(Arm64Emitter &e, int crfD)
{
    // nibble = 4 + 4*LT - 2*EQ  (exactly one of LT/GT/EQ is true):
    //   LT=1,EQ=0 → 8; LT=0,EQ=1 → 2; LT=0,EQ=0 → 4 (GT)
    e.CSET_W(W1, A64_LT);               // W1 = LT (N^V)
    e.CSET_W(W2, A64_EQ);               // W2 = EQ (Z)
    e.LSL_W_IMM(W1, W1, 2);             // W1 = 4*LT
    e.ADD_W_IMM(W1, W1, 4);             // W1 = 4 + 4*LT
    e.SUB_W_LSL(W1, W1, W2, 1);         // W1 = (4+4*LT) - 2*EQ = nibble
    e.STRB(W1, PPC_PTR, OFF_CR + crfD);
}

// Like emit_cr_from_flags_signed but reads N (A64_MI) instead of LT (N^V).
// Use after ADDS_W/ADCS_W/SUBS_W/SBCS_W: V may be set by the arithmetic, but
// PPC CR0[LT] is bit31(result), not the signed-overflow-corrected N^V.
static void emit_cr_from_arith_flags(Arm64Emitter &e, int crfD)
{
    e.CSET_W(W1, A64_MI);              // W1 = N (bit31 of result)
    e.CSET_W(W2, A64_EQ);              // W2 = Z
    e.LSL_W_IMM(W1, W1, 2);
    e.ADD_W_IMM(W1, W1, 4);
    e.SUB_W_LSL(W1, W1, W2, 1);
    e.STRB(W1, PPC_PTR, OFF_CR + crfD);
}

// Update CR field crfD after unsigned comparison (flags set by CMP for unsigned).
static void emit_cr_from_flags_unsigned(Arm64Emitter &e, int crfD)
{
    e.CSET_W(W1, A64_CC);               // W1 = 1 if unsigned LT (carry clear)
    e.CSET_W(W2, A64_EQ);               // W2 = 1 if EQ
    e.LSL_W_IMM(W1, W1, 2);
    e.ADD_W_IMM(W1, W1, 4);
    e.SUB_W_LSL(W1, W1, W2, 1);
    e.STRB(W1, PPC_PTR, OFF_CR + crfD);
}

// Update CR0 after an ALU instruction that updates flags (Rc=1 bit)
// Assumes the result is in W0 and flags are NOT set. Sets flags first.
static void emit_set_cr0_from_W0(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0);             // set flags based on W0
    emit_cr_from_flags_signed(e, 0);
}

// Update CR0 when the result is known non-negative (bit31=0 guaranteed) and
// ARM flags already reflect the result (from ANDS or CMP). Only Z matters:
// nibble = 4 (GT) if !Z, 2 (EQ) if Z.
static void emit_cr0_nonneg_from_flags(Arm64Emitter &e)
{
    e.CSET_W(W1, A64_EQ);               // W1 = Z
    e.MOV_W32(W2, 4);                   // W2 = GT nibble
    e.SUB_W_LSL(W2, W2, W1, 1);        // W2 = 4 - 2*Z = 4 (GT) or 2 (EQ)
    e.STRB(W2, PPC_PTR, OFF_CR);
}

// Update CR0 when W0 is known non-negative (bit31==0). Sets flags first via CMP.
static void emit_cr0_nonneg(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0);
    emit_cr0_nonneg_from_flags(e);
}

// Update CR0 when the result is a JIT compile-time constant. Hardcodes LT/EQ/GT nibble.
static void emit_cr0_const_result(Arm64Emitter &e, int32_t val)
{
    int nibble = (val == 0) ? 2 : (val < 0) ? 8 : 4;
    e.MOV_W32(W1, nibble);
    e.STRB(W1, PPC_PTR, OFF_CR);
}

// Extract CR bit crbit into bit 0 of Wdst (0 or 1). Clobbers only Wdst.
// crbit is 0-31 where bit 0 = CR0[LT], bit 31 = CR7[SO].
static void emit_load_cr_bit(Arm64Emitter &e, int Wdst, int crbit)
{
    int field  = crbit / 4;
    int bitpos = 3 - (crbit % 4);   // nibble bit: 3=MSB(LT), 0=LSB(SO)
    e.LDRB(Wdst, PPC_PTR, OFF_CR + field);
    e.UBFM_W(Wdst, Wdst, bitpos, bitpos);  // extract bit[bitpos] → bit[0], zero-extend
}

// Store bit 0 of Wbit into CR bit crbit. Clobbers Wbit (unused after), Wtmp.
static void emit_store_cr_bit(Arm64Emitter &e, int Wbit, int Wtmp, int crbit)
{
    int field  = crbit / 4;
    int bitpos = 3 - (crbit % 4);
    e.LDRB(Wtmp, PPC_PTR, OFF_CR + field);
    e.BFI_W(Wtmp, Wbit, bitpos, 1);    // insert Wbit[0] into Wtmp[bitpos]
    e.STRB(Wtmp, PPC_PTR, OFF_CR + field);
}

// Load XER.CA (bit 29) into Wdst as 0 or 1. Clobbers only Wdst.
static void emit_load_xer_ca(Arm64Emitter &e, int Wdst)
{
    e.LDRB(Wdst, PPC_PTR, OFF_XER_CA);
}

// Set ARM carry flag from Wca (0 or 1).  CMP Wca,#1 = SUBS WZR,Wca,#1:
// C=0 (borrow) when Wca=0, C=1 (no borrow) when Wca=1.
static void emit_arm_carry_from_W(Arm64Emitter &e, int Wca)
{
    e.CMP_W_IMM(Wca, 1);
}

// After ADDS_W/ADCS_W/SUBS_W/SBCS_W, write ARM carry flag → XER.CA.
// Clobbers W2. Must be called immediately after the flag-setting op.
static void emit_update_xer_ca(Arm64Emitter &e)
{
    e.CSET_W(W2, A64_CS);            // W2 = carry (reads flags before anything else)
    e.STRB(W2, PPC_PTR, OFF_XER_CA);
}

// Call an external C function.  When the function is in the pointer table at
// the start of the code buffer, emit ADRP + LDR_X (2 instructions) instead of
// MOV_X64 (3-4 instructions), saving 1-2 ARM instructions per C call.
static void emit_call(Arm64Emitter &e, uint64_t fn_addr)
{
    if (g_fn_tbl) {
        // Linear scan of the table (compile-time only — not hot)
        void **tbl = (void **)g_fn_tbl;
        for (int i = 0; i < (int)JitArm64::FN_TABLE_ENTRIES; i++) {
            if ((uint64_t)tbl[i] == fn_addr) {
                // BL to pre-emitted stub (ADRP+LDR+BR trampoline): 1 instruction vs 3.
                // Stubs live at g_fn_tbl + FN_TABLE_BYTES + i*12; always within ±128 MB.
                uint64_t stub_addr = (uint64_t)g_fn_tbl + JitArm64::FN_TABLE_BYTES + i * 12;
                e.BL((int64_t)stub_addr - (int64_t)e.ptr());
                return;
            }
        }
    }
    // Fallback: full 64-bit constant load
    e.MOV_X64(X16, fn_addr);
    e.BLR(X16);
}

// Store pc and npc before a fallback instruction call
static void emit_set_pc_npc(Arm64Emitter &e, uint32_t inst_pc)
{
    e.MOV_W32(W0, inst_pc);
    e.ADD_W_IMM(W1, W0, 4);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);
}

// Emit a fallback call to ppc_dispatch_opcode(opcode)
static void emit_fallback(Arm64Emitter &e, uint32_t opcode, uint32_t inst_pc)
{
    emit_set_pc_npc(e, inst_pc);
    e.MOV_W32(W0, opcode);
    emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
}

// ---------------------------------------------------------------------------
// Effective address helpers
// ---------------------------------------------------------------------------

// D-form (immediate): EA = (rA==0 ? 0 : REG(rA)) + simm16  → W0
static void emit_load_ea_imm(Arm64Emitter &e, int rA, int16_t simm)
{
    if (rA == 0) {
        emit_load_simm16(e, W0, simm);
    } else {
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
    }
}

// X-form (register): EA = (rA==0 ? 0 : REG(rA)) + REG(rB)  → Wd  (uses W1 as scratch)
static void emit_load_ea_reg(Arm64Emitter &e, int Wd, int rA, int rB)
{
    if (rA == 0) {
        emit_load_gpr(e, Wd, rB);
    } else {
        emit_load_gpr(e, Wd, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(Wd, Wd, W1);
    }
}

// ---------------------------------------------------------------------------
// Inline RAM fast-path helpers
//
// The Model 3's 8MB PowerPC RAM lives at guest addresses 0x00000000–0x007FFFFF.
// Rather than BL→vtable→CModel3::Read/Write32 for every access, we check the
// address at JIT-compile time and, for the ~90% of accesses that hit RAM, emit
// a direct host LDR/STR using the ram_ptr stored in PPC_REGS.
//
// Byte ordering: RAM is stored word-byte-reversed (little-endian in host memory)
// so that 32-bit host LDR gives the correct big-endian PPC value with no swap.
// 8-bit and 16-bit accesses use XOR addressing (^3 / ^2) to undo the reversal.
//
// On entry to each helper: W0 = guest EA, W1 = store data (stores only).
// X16 is used as scratch (caller-saved IP0 — safe to clobber here).
// ---------------------------------------------------------------------------

// Emit: load 32-bit from RAM fast-path (W0=addr → W0=result), else call jit_read32.
// Only safe for word-aligned addresses: unaligned lwz (addr%4==2) uses XOR-addressed
// Read16 pairs that read different host bytes than a plain LDR would.
static void emit_ram_load32(Arm64Emitter &e)
{
    // Reject unaligned: bits 1 or 0 set → slow path (avoids XOR-addressing mismatch)
    uint32_t *slow_b0 = e.emit_TBNZ_W_placeholder(W0, 0);
    uint32_t *slow_b1 = e.emit_TBNZ_W_placeholder(W0, 1);
    // Reject out-of-range: addr >= 0x800000 → slow path
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow_hi = e.emit_B_COND_placeholder(A64_CS);
    // Fast: ram_ptr[addr]
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);        // X16 = ram_ptr
    e.ADD_X_UXTW(X16, X16, W0);                           // X16 = ram_ptr + addr
    e.LDR_W(W0, X16, 0);                                  // W0 = *(uint32_t*)(ram_ptr+addr)
    uint32_t *done = e.emit_B_placeholder();
    uint32_t *slow = e.ptr();
    e.patch_TBZ(slow_b0, slow);
    e.patch_TBZ(slow_b1, slow);
    e.patch_B_COND(slow_hi, slow);
    emit_call(e, (uint64_t)(void *)&jit_read32);
    e.patch_B(done, e.ptr());
}

// Emit: load 8-bit from RAM fast-path (W0=addr → W0=result), else call jit_read8.
[[maybe_unused]] static void emit_ram_load8(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow = e.emit_B_COND_placeholder(A64_CS);
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);
    e.EOR_W_BITMASK(W4, W0, 0, 1);                        // W4 = addr^3 (XOR byte-swap: bitmask imm 0b11)
    e.ADD_X_UXTW(X16, X16, W4);
    e.LDRB(W0, X16, 0);
    uint32_t *done = e.emit_B_placeholder();
    e.patch_B_COND(slow, e.ptr());
    emit_call(e, (uint64_t)(void *)&jit_read8);
    e.patch_B(done, e.ptr());
}

// Emit: load 16-bit from RAM fast-path (W0=addr → W0=result), else call jit_read16.
[[maybe_unused]] static void emit_ram_load16(Arm64Emitter &e, bool sign_extend)
{
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow = e.emit_B_COND_placeholder(A64_CS);
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);
    e.EOR_W_BITMASK(W4, W0, 31, 0);                       // W4 = addr^2 (XOR halfword-swap: bitmask imm 0b10)
    e.ADD_X_UXTW(X16, X16, W4);
    e.LDRH(W0, X16, 0);
    if (sign_extend) e.SXTH_W(W0, W0);
    uint32_t *done = e.emit_B_placeholder();
    e.patch_B_COND(slow, e.ptr());
    emit_call(e, (uint64_t)(void *)&jit_read16);
    if (sign_extend) e.SXTH_W(W0, W0);                    // also sign-extend on slow path
    e.patch_B(done, e.ptr());
}

// Emit: store 32-bit to RAM fast-path (W0=addr, W1=data), else call jit_write32.
[[maybe_unused]] static void emit_ram_store32(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow = e.emit_B_COND_placeholder(A64_CS);
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);
    e.ADD_X_UXTW(X16, X16, W0);
    e.STR_W(W1, X16, 0);
    uint32_t *done = e.emit_B_placeholder();
    e.patch_B_COND(slow, e.ptr());
    emit_call(e, (uint64_t)(void *)&jit_write32);
    e.patch_B(done, e.ptr());
}

// Emit: store 8-bit to RAM fast-path (W0=addr, W1=data), else call jit_write8.
[[maybe_unused]] static void emit_ram_store8(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow = e.emit_B_COND_placeholder(A64_CS);
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);
    e.EOR_W_BITMASK(W4, W0, 0, 1);                        // W4 = addr^3
    e.ADD_X_UXTW(X16, X16, W4);
    e.STRB(W1, X16, 0);
    uint32_t *done = e.emit_B_placeholder();
    e.patch_B_COND(slow, e.ptr());
    emit_call(e, (uint64_t)(void *)&jit_write8);
    e.patch_B(done, e.ptr());
}

// Emit: store 16-bit to RAM fast-path (W0=addr, W1=data), else call jit_write16.
[[maybe_unused]] static void emit_ram_store16(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0x800u, 1);
    uint32_t *slow = e.emit_B_COND_placeholder(A64_CS);
    e.LDR_X(X16, PPC_PTR, (uint32_t)OFF_RAM_PTR);
    e.EOR_W_BITMASK(W4, W0, 31, 0);                       // W4 = addr^2
    e.ADD_X_UXTW(X16, X16, W4);
    e.STRH(W1, X16, 0);
    uint32_t *done = e.emit_B_placeholder();
    e.patch_B_COND(slow, e.ptr());
    emit_call(e, (uint64_t)(void *)&jit_write16);
    e.patch_B(done, e.ptr());
}

// ---------------------------------------------------------------------------
// Memory load/store translators
// ---------------------------------------------------------------------------

// D-form loads: opcodes 32 (lwz), 33 (lwzu), 34 (lbz), 35 (lbzu),
//               40 (lhz), 41 (lhzu), 42 (lha), 43 (lhau)
static bool translate_load_imm(Arm64Emitter &e, uint32_t op, int opcode)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    bool update = (opcode & 1);   // odd opcodes in each pair are the update form

    if (update) {
        // EA = REG(rA) + simm; REG(rA) = EA before memory access
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
        emit_store_gpr(e, W0, rA);      // rA = EA (W0 still holds EA)
    } else {
        emit_load_ea_imm(e, rA, simm);  // W0 = EA
    }

    switch (opcode) {
    case 32: case 33: emit_ram_load32(e);                                      break;
    case 34: case 35: emit_call(e, (uint64_t)(void *)&jit_read8);             break;
    case 40: case 41: emit_call(e, (uint64_t)(void *)&jit_read16);            break;
    case 42: case 43: emit_call(e, (uint64_t)(void *)&jit_read16);
                      e.SXTH_W(W0, W0);                                        break;
    default: return false;
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// D-form stores: opcodes 36 (stw), 37 (stwu), 38 (stb), 39 (stbu),
//                44 (sth), 45 (sthu)
static bool translate_store_imm(Arm64Emitter &e, uint32_t op, int opcode)
{
    int rS    = (op >> 21) & 0x1F;   // source data register
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    bool update = (opcode & 1);

    if (update) {
        // Compute EA; load rS before writeback to handle rS==rA correctly
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);   // W0 = EA
        emit_load_gpr(e, W1, rS);           // W1 = rS (before rA is overwritten)
        emit_store_gpr(e, W0, rA);          // rA = EA
    } else {
        emit_load_ea_imm(e, rA, simm);      // W0 = EA
        emit_load_gpr(e, W1, rS);           // W1 = data
    }

    switch (opcode) {
    case 36: case 37: emit_call(e, (uint64_t)(void *)&jit_write32);           break;
    case 38: case 39: emit_call(e, (uint64_t)(void *)&jit_write8);            break;
    case 44: case 45: emit_call(e, (uint64_t)(void *)&jit_write16);           break;
    default: return false;
    }
    return true;
}

// Emit the block epilogue: update icount, set pc/npc, restore and return
// Variant: NPC comes from a register (W_npc) rather than a compile-time constant.
// The caller must ensure W_npc is not W0 or W1.
static void emit_epilogue_npc_reg(Arm64Emitter &e, int inst_count, uint32_t last_pc, int W_npc)
{
    e.LDR_W(W0, PPC_PTR, OFF_ICOUNT);
    e.SUB_W_IMM(W0, W0, inst_count);
    e.STR_W(W0, PPC_PTR, OFF_ICOUNT);
    e.MOV_W32(W1, last_pc);
    e.STP_W(W1, W_npc, PPC_PTR, OFF_PC);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    e.RET();
}

[[maybe_unused]] static void emit_epilogue(Arm64Emitter &e, int inst_count, uint32_t last_pc, uint32_t next_pc)
{
    // ppc.icount -= inst_count
    e.LDR_W(W0, PPC_PTR, OFF_ICOUNT);
    if (inst_count <= 4095)
        e.SUB_W_IMM(W0, W0, inst_count);
    else {
        e.MOV_W32(W1, inst_count);
        e.SUB_W(W0, W0, W1);
    }
    e.STR_W(W0, PPC_PTR, OFF_ICOUNT);

    // ppc.pc = last_pc; ppc.npc = next_pc (store pair)
    e.MOV_W32(W0, last_pc);
    e.MOV_W32(W1, next_pc);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);

    e.LDP_post(PPC_PTR, 30, A64_SP, 16);  // restore X19, X30
    e.RET();
}

// Tail-call epilogue for a statically-known block target already compiled.
// Updates icount, checks for pending interrupts/fatal errors, then — if nothing
// is pending — tail-calls target_fn directly (no dispatch-loop roundtrip).
// On the slow-exit path (icount exhausted / interrupt / fatal), writes pc/npc
// and returns to the C++ dispatch loop which delivers the interrupt.
// Both target_fn and this code must be within the same 16 MB code buffer. ✓
//
// Register usage: W4 used for both icount and interrupt_pending checks.
// fatalError is mirrored into interrupt_pending bit 3 (0x8) by all fatal-setting
// sites, so a single LDR+CBNZ on interrupt_pending covers both.
//
// Fast path ARM instructions (normal case): ~10 (icount+irq check+tail call).
static void emit_epilogue_chained(Arm64Emitter &e, int inst_count, uint32_t last_pc,
                                   uint32_t next_pc, void *target_fn)
{
    // W4 = new_icount.  SUBS sets flags so B.LE below needs no separate CMP.
    e.LDR_W(W4, PPC_PTR, OFF_ICOUNT);
    if (inst_count <= 4095)
        e.SUBS_W_IMM(W4, W4, (uint32_t)inst_count);
    else {
        e.MOV_W32(W0, (uint32_t)inst_count);
        e.SUBS_W(W4, W4, W0);
    }
    e.STR_W(W4, PPC_PTR, OFF_ICOUNT);

    // Early exit when icount <= 0.  Flags from SUBS survive STR/MOV/STP — no CMP needed.
    uint32_t *exit_icount = e.emit_B_COND_placeholder(A64_LE);   // B.LE slow_exit

    // Exit if any interrupt or fatal error is pending (reuse W4; fatalError mirrors into bit 3).
    // Required so tight spin loops (e.g. waiting on a flag set by an IRQ handler) don't starve
    // interrupt delivery by chaining indefinitely without returning to the C++ dispatch loop.
    e.LDR_W(W4, PPC_PTR, OFF_INT_PENDING);
    uint32_t *exit_irq = e.emit_CBNZ_W_placeholder(W4);          // CBNZ slow_exit

    // Fast path: tail call; X0 = PPC_REGS* for callee prologue
    e.MOV_X(0, PPC_PTR);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    int64_t off = (int64_t)target_fn - (int64_t)e.ptr();
    e.B((int)off);

    // Slow exit: write pc/npc so the dispatch loop can restart, then return
    uint32_t *slow_exit = e.ptr();
    e.patch_B_COND(exit_icount, slow_exit);
    e.patch_CBZ(exit_irq, slow_exit);
    e.MOV_W32(W0, last_pc);
    e.MOV_W32(W1, next_pc);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    e.RET();
}

// Deferred-chaining epilogue: same icount check as emit_epilogue_chained,
// but the tail-call target is initially a fallback (writes pc/npc and returns to C++),
// and will be backpatched to the actual target block once that block is compiled.
//
// Returns the address of the patchable B instruction in the fast path.
// The caller must register it with JitArm64::m_fixups[next_pc].
//
// Register usage after LDP (in fallback path): X16 holds ppc ptr (saved before LDP).
static uint32_t* emit_epilogue_deferred(Arm64Emitter &e, int inst_count,
                                         uint32_t last_pc, uint32_t next_pc)
{
    // icount check (same as emit_epilogue_chained)
    e.LDR_W(W4, PPC_PTR, OFF_ICOUNT);
    if (inst_count <= 4095)
        e.SUBS_W_IMM(W4, W4, (uint32_t)inst_count);
    else {
        e.MOV_W32(W0, (uint32_t)inst_count);
        e.SUBS_W(W4, W4, W0);
    }
    e.STR_W(W4, PPC_PTR, OFF_ICOUNT);
    uint32_t *exit_icount = e.emit_B_COND_placeholder(A64_LE);

    // Exit if any interrupt or fatal error is pending (same reason as emit_epilogue_chained).
    e.LDR_W(W4, PPC_PTR, OFF_INT_PENDING);
    uint32_t *exit_irq = e.emit_CBNZ_W_placeholder(W4);

    // Fast path: save ppc ptr in X16, restore callee-saved regs, then fixup B.
    // X16 (IP0) is not preserved by LDP and survives into the fallback path.
    e.MOV_X(0, PPC_PTR);             // X0 = ppc ptr (arg for target block prologue)
    e.MOV_X(X16, PPC_PTR);           // X16 = ppc ptr (for fallback pc/npc write)
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);

    // Fixup B: initially branches to fallback immediately below; patched to target_fn.
    uint32_t *fixup_site = e.emit_B_placeholder();

    // Fallback path (reached while target not yet compiled): write pc/npc via X16, RET.
    uint32_t *fallback = e.ptr();
    e.patch_B(fixup_site, fallback);  // B #0 → B to fallback
    e.MOV_W32(W1, last_pc);
    e.MOV_W32(W2, next_pc);
    e.STR_W(W1, X16, (uint32_t)OFF_PC);
    e.STR_W(W2, X16, (uint32_t)OFF_NPC);
    e.RET();

    // Slow exit (icount exhausted or interrupt pending): write pc/npc via X19, LDP, RET.
    uint32_t *slow_exit = e.ptr();
    e.patch_B_COND(exit_icount, slow_exit);
    e.patch_CBZ(exit_irq, slow_exit);
    e.MOV_W32(W0, last_pc);
    e.MOV_W32(W1, next_pc);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    e.RET();

    return fixup_site;
}

// Apply all pending fixups for a newly-compiled block (retroactive backpatching).
// Patches each registered B instruction to tail-call target_arm directly.
static size_t apply_fixups(std::unordered_map<uint32_t, std::vector<uint32_t*>> &fixup_map,
                            uint32_t ppc_pc, uint8_t *target_arm)
{
    auto it = fixup_map.find(ppc_pc);
    if (it == fixup_map.end()) return 0;
    size_t n = it->second.size();
    for (uint32_t *site : it->second) {
        int off = (int)(((uint32_t*)target_arm - site) * 4);
        *site = 0x14000000u | ((uint32_t)(off / 4) & 0x3FFFFFFu);
        __builtin___clear_cache((char*)site, (char*)(site + 1));
    }
    fixup_map.erase(it);
    return n;
}

// ---------------------------------------------------------------------------
// PPC instruction translators
// ---------------------------------------------------------------------------

// Primary opcode 14: addi rD, rA, SIMM  (includes li = addi rD, r0, SIMM)
static bool translate_addi(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (rA == 0) {
        // li rD, simm
        emit_load_simm16(e, W0, simm);
    } else {
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 15: addis rD, rA, SIMM  (includes lis = addis rD, r0, SIMM)
static bool translate_addis(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    uint32_t imm = (uint32_t)simm << 16;

    if (rA == 0) {
        e.MOV_W32(W0, imm);
    } else {
        emit_load_gpr(e, W0, rA);
        // For |SIMM| in [1,255]: SIMM<<16 fits in ADD/SUB imm12<<12 (sh=1) → 1 instruction
        if (simm > 0 && simm <= 255) {
            e.ADD_W_IMM(W0, W0, (uint32_t)simm << 4, 1);
        } else if (simm < 0 && simm >= -255) {
            e.SUB_W_IMM(W0, W0, (uint32_t)(-simm) << 4, 1);
        } else {
            e.MOV_W32(W1, imm);
            e.ADD_W(W0, W0, W1);
        }
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 24: ori rA, rS, UIMM
static bool translate_ori(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    if (uimm == 0) {
        // nop if rA == rS, else mr
        if (rA != rS) { emit_load_gpr(e, W0, rS); emit_store_gpr(e, W0, rA); }
        return true;
    }
    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.ORR_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.ORR_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 25: oris rA, rS, UIMM
static bool translate_oris(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    if (uimm == 0) {
        if (rA != rS) { emit_load_gpr(e, W0, rS); emit_store_gpr(e, W0, rA); }
        return true;
    }
    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.ORR_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.ORR_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 26: xori rA, rS, UIMM
static bool translate_xori(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    if (uimm == 0) {
        if (rA != rS) { emit_load_gpr(e, W0, rS); emit_store_gpr(e, W0, rA); }
        return true;
    }
    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.EOR_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.EOR_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 27: xoris rA, rS, UIMM
static bool translate_xoris(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    if (uimm == 0) {
        if (rA != rS) { emit_load_gpr(e, W0, rS); emit_store_gpr(e, W0, rA); }
        return true;
    }
    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.EOR_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.EOR_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 28: andi. rA, rS, UIMM  (always updates CR0)
static bool translate_andi_dot(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.ANDS_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.ANDS_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    emit_cr0_nonneg_from_flags(e);  // UIMM is 16-bit zero-extended: result[31..16]=0 always
    return true;
}

// Primary opcode 29: andis. rA, rS, UIMM
static bool translate_andis_dot(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    emit_load_gpr(e, W0, rS);
    int immr, imms;
    if (bitmask_imm32(uimm, &immr, &imms)) {
        e.ANDS_W_BITMASK(W0, W0, immr, imms);
    } else {
        e.MOV_W32(W1, uimm);
        e.ANDS_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    // If UIMM bit 15 = 0, mask has no bit-31 → result non-negative
    if ((uimm & 0x80000000) == 0)
        emit_cr0_nonneg_from_flags(e);
    else
        emit_cr_from_flags_signed(e, 0);
    return true;
}

// Primary opcode 11: cmpi crfD, rA, SIMM  (cmpwi)
static bool translate_cmpi(Arm64Emitter &e, uint32_t op)
{
    int crfD = (op >> 23) & 0x7;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    if (simm >= 0 && (uint32_t)simm <= 4095) {
        e.CMP_W_IMM(W0, (uint32_t)simm);
    } else if (simm < 0 && (uint32_t)(-simm) <= 4095) {
        e.CMN_W_IMM(W0, (uint32_t)(-simm));
    } else if (simm > 0 && ((uint32_t)simm & 0xFFF) == 0) {
        e.CMP_W_IMM(W0, (uint32_t)simm >> 12, 1);   // multiple of 4096
    } else if (simm < 0 && ((uint32_t)(-simm) & 0xFFF) == 0) {
        e.CMN_W_IMM(W0, (uint32_t)(-simm) >> 12, 1);
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.CMP_W(W0, W1);
    }
    emit_cr_from_flags_signed(e, crfD);
    return true;
}

// Primary opcode 10: cmpli crfD, rA, UIMM  (cmplwi, unsigned)
static bool translate_cmpli(Arm64Emitter &e, uint32_t op)
{
    int crfD = (op >> 23) & 0x7;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    emit_load_gpr(e, W0, rA);
    if (uimm <= 4095)
        e.CMP_W_IMM(W0, uimm);
    else if ((uimm & 0xFFF) == 0)
        e.CMP_W_IMM(W0, uimm >> 12, 1);  // multiple of 4096
    else {
        e.MOV_W32(W1, uimm);
        e.CMP_W(W0, W1);
    }
    emit_cr_from_flags_unsigned(e, crfD);
    return true;
}

// Primary opcode 7: mulli rD, rA, SIMM
static bool translate_mulli(Arm64Emitter &e, uint32_t op)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int32_t sv = (int32_t)(int16_t)(op & 0xFFFF);

    if (sv == 0) {
        e.MOV_W32(W0, 0);
        emit_store_gpr(e, W0, rD);
        return true;
    }
    uint32_t abs_sv = sv < 0 ? (uint32_t)(-sv) : (uint32_t)sv;
    if ((abs_sv & (abs_sv - 1)) == 0) {
        // |simm| is a power of two: use LSL [+ NEG] instead of MUL
        int shift = __builtin_ctz(abs_sv);
        emit_load_gpr(e, W0, rA);
        if (shift > 0) e.LSL_W_IMM(W0, W0, shift);
        if (sv < 0)    e.NEG_W(W0, W0);
        emit_store_gpr(e, W0, rD);
        return true;
    }
    // sv = -(2^n - 1): SUB W0, W0, W0, LSL#n  (e.g. -3x, -7x, -15x, -31x)
    // W0*(1-2^n) computes sv*rA in one instruction; saves 1 vs ADD_W_LSL+NEG or MOV+MUL.
    // Positive 2^n-1 cases (sv=7,15,...) need SUB+NEG = 2 insns, same as MOV+MUL — not worth it.
    if (sv < 0) {
        uint32_t abs_sv_p1 = abs_sv + 1;
        if (abs_sv_p1 && (abs_sv_p1 & abs_sv) == 0) {
            int shift = __builtin_ctz(abs_sv_p1);
            emit_load_gpr(e, W0, rA);
            e.SUB_W_LSL(W0, W0, W0, shift);    // W0 = rA*(1-2^n) = sv*rA
            emit_store_gpr(e, W0, rD);
            return true;
        }
    }
    // sv = 1 + 2^n: ADD W0, W0, W0, LSL#n  (e.g. 3x, 5x, 9x, 17x)
    uint32_t abs_sv1 = abs_sv - 1;
    if (abs_sv1 && (abs_sv1 & (abs_sv1 - 1)) == 0) {
        int shift = __builtin_ctz(abs_sv1);
        emit_load_gpr(e, W0, rA);
        e.ADD_W_LSL(W0, W0, W0, shift);
        if (sv < 0) e.NEG_W(W0, W0);
        emit_store_gpr(e, W0, rD);
        return true;
    }
    emit_load_gpr(e, W0, rA);
    e.MOV_W32(W1, (uint32_t)sv);
    e.MUL_W(W0, W0, W1);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 21: rlwinm rA, rS, SH, MB, ME
static bool translate_rlwinm(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int sh = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    // Peephole: identity (sh=0, mb=0, me=31 → mask=all, no rotate)
    if (sh == 0 && mb == 0 && me == 31) {
        if (rc == 0 && rA == rS) return true;
        emit_load_gpr(e, W0, rS);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // Peephole: logical shift left — ROTL(rS,sh) & ~((1<<sh)-1) = rS << sh
    // Condition: mb==0, me==31-sh  (mask covers [sh..31], lower sh bits zeroed)
    if (sh > 0 && mb == 0 && me == 31 - sh) {
        emit_load_gpr(e, W0, rS);
        e.LSL_W_IMM(W0, W0, sh);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // Peephole: UBFX — me==31 means result starts at bit 0; when mb>=32-sh the source
    // field [32-sh .. 63-sh-mb] of rS doesn't wrap, encodable as UBFM(immr=32-sh, imms=63-sh-mb).
    // Covers LSR (mb==32-sh) and narrower extracts.
    if (sh > 0 && me == 31 && mb >= 32 - sh) {
        emit_load_gpr(e, W0, rS);
        e.UBFM_W(W0, W0, 32 - sh, 63 - sh - mb);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_cr0_nonneg(e);   // UBFX zero-extends: bit31==0 always
        return true;
    }

    // Peephole: sh=0, me==31, mb>0 — zero the upper mb bits via UBFX from bit 0
    if (sh == 0 && me == 31 && mb > 0) {
        emit_load_gpr(e, W0, rS);
        if (rc) {
            e.ANDS_W_BITMASK(W0, W0, 0, 31 - mb);  // mask = lower (32-mb) bits; sets N/Z
            emit_store_gpr(e, W0, rA);
            emit_cr0_nonneg_from_flags(e);           // mb>0: bit31 not in mask, N=0 always
        } else {
            e.UBFM_W(W0, W0, 0, 31 - mb);
            emit_store_gpr(e, W0, rA);
        }
        return true;
    }

    // General path: rotate then mask.  The mask is always a bitmask immediate (single
    // contiguous run of 1-bits, possibly wrapping).  For mb<=me: immr=(me+1)&31, imms=me-mb.
    // For mb>me (wrapping): same immr, imms=32+me-mb.  All-ones case (mb==me+1 for wrapping,
    // mb=0&&me=31 for non-wrapping) is filtered by the identity peephole or the mb==me+1 check.
    int immr_bm = (me + 1) & 31;
    int imms_bm = (mb <= me) ? (me - mb) : (32 + me - mb);
    bool need_and = (mb > me) ? (mb != me + 1) : (mb != 0 || me != 31);

    emit_load_gpr(e, W0, rS);

    if (sh > 0)
        e.ROR_W_IMM(W0, W0, 32 - sh);   // ROTL32(rS, sh) = ROR32(rS, 32-sh)

    if (need_and) {
        if (rc) {
            e.ANDS_W_BITMASK(W0, W0, immr_bm, imms_bm);  // sets N/Z, V=C=0
            emit_store_gpr(e, W0, rA);
            // Non-wrapping mask with mb>0: bit31 not in mask, N=0 always
            if (mb <= me && mb > 0)
                emit_cr0_nonneg_from_flags(e);
            else
                emit_cr_from_flags_signed(e, 0);
            return true;
        }
        e.AND_W_BITMASK(W0, W0, immr_bm, imms_bm);
    }

    emit_store_gpr(e, W0, rA);
    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 20: rlwimi rA, rS, SH, MB, ME  (rotate left word immediate then mask insert)
static bool translate_rlwimi(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int sh = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    if (mb <= me) {
        int lsb   = 31 - me;
        int width = me - mb + 1;
        emit_load_gpr(e, W0, rS);
        if (sh > 0) e.ROR_W_IMM(W0, W0, 32 - sh);
        if (width == 32) {
            // Full-word replace: just rotate and store
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        if (lsb > 0) e.LSR_W_IMM(W0, W0, lsb);   // shift field to bit 0
        emit_load_gpr(e, W1, rA);
        e.BFI_W(W1, W0, lsb, width);              // insert field into rA
        emit_store_gpr(e, W1, rA);
        if (rc) { e.CMP_W_IMM(W1, 0); emit_cr_from_flags_signed(e, 0); }
        return true;
    }

    // mb > me: wrapping mask — bitmask immediates for both mask and ~mask.
    // mask (wrapping): immr=(me+1)&31, imms=32+me-mb.
    // ~mask (gap, non-wrapping): immr=mb&31, imms=mb-me-2.
    // When mb==me+1 the gap is empty and mask=all-ones: result = ROTL(rS, sh) entirely.
    bool wrap_full = (mb == me + 1);
    int immr_m  = (me + 1) & 31;
    int imms_m  = 32 + me - mb;    // mask
    int immr_nm = mb & 31;
    int imms_nm = mb - me - 2;     // ~mask (only valid when mb > me+1)

    emit_load_gpr(e, W0, rS);
    if (sh > 0) e.ROR_W_IMM(W0, W0, 32 - sh);
    if (!wrap_full) {
        emit_load_gpr(e, W2, rA);
        e.AND_W_BITMASK(W0, W0, immr_m,  imms_m);   // W0 = rotated_rS & mask
        e.AND_W_BITMASK(W2, W2, immr_nm, imms_nm);  // W2 = rA & ~mask
        e.ORR_W(W0, W0, W2);
    }
    emit_store_gpr(e, W0, rA);
    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 23: rlwnm rA, rS, rB, MB, ME  (rotate left word then AND with mask)
static bool translate_rlwnm(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int rB = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    int immr_bm = (me + 1) & 31;
    int imms_bm = (mb <= me) ? (me - mb) : (32 + me - mb);
    bool need_and = (mb > me) ? (mb != me + 1) : (mb != 0 || me != 31);

    emit_load_gpr(e, W0, rS);
    emit_load_gpr(e, W1, rB);
    e.NEG_W(W2, W1);                // W2 = -rB; RORV uses W2[4:0] = (32-rB)[4:0]
    e.ROR_W(W0, W0, W2);            // W0 = ROTL(rS, rB[4:0])

    if (need_and) {
        if (rc) {
            e.ANDS_W_BITMASK(W0, W0, immr_bm, imms_bm);
            emit_store_gpr(e, W0, rA);
            // Non-wrapping mask with mb>0: bit31 not in mask, N=0 always
            if (mb <= me && mb > 0)
                emit_cr0_nonneg_from_flags(e);
            else
                emit_cr_from_flags_signed(e, 0);
            return true;
        }
        e.AND_W_BITMASK(W0, W0, immr_bm, imms_bm);
    }
    emit_store_gpr(e, W0, rA);
    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// ---------------------------------------------------------------------------
// Opcode-31 sub-operations
// ---------------------------------------------------------------------------

// Forward declarations for X-form load/store helpers (defined after translate_op31)
static bool translate_op31_load_x(Arm64Emitter &e, int rD, int rA, int rB,
                                   void *reader, bool sign_ext);
static bool translate_op31_load_xu(Arm64Emitter &e, int rD, int rA, int rB,
                                    void *reader, bool sign_ext);
static bool translate_op31_store_x(Arm64Emitter &e, int rS, int rA, int rB, void *writer);
static bool translate_op31_store_xu(Arm64Emitter &e, int rS, int rA, int rB, void *writer);

static bool translate_op31(Arm64Emitter &e, uint32_t op)
{
    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rc  = op & 1;
    int subop = (op >> 1) & 0x3FF;

    switch (subop) {
    // cmp crfD, rA, rB  (signed compare)
    case 0: {
        int crfD = (op >> 23) & 0x7;
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.CMP_W(W0, W1);
        emit_cr_from_flags_signed(e, crfD);
        return true;
    }
    // cmpl crfD, rA, rB  (unsigned compare)
    case 32: {
        int crfD = (op >> 23) & 0x7;
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.CMP_W(W0, W1);    // ARM unsigned compare same instruction; flags different
        emit_cr_from_flags_unsigned(e, crfD);
        return true;
    }
    // add rD, rA, rB  (addo = 778: OE ignored, same result)
    case 778:
    case 266:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        if (rc) {
            e.ADDS_W(W0, W0, W1);
            emit_store_gpr(e, W0, rD);
            emit_cr_from_arith_flags(e, 0);
        } else {
            e.ADD_W(W0, W0, W1);
            emit_store_gpr(e, W0, rD);
        }
        return true;

    // addc rD, rA, rB  (rD = rA + rB, XER.CA = carry out; addco = 522)
    case 522:
    case 10:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADDS_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);          // clobbers W2; W0 unchanged
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // adde rD, rA, rB  (rD = rA + rB + XER.CA; addeo = 650)
    case 650:
    case 138:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        emit_load_xer_ca(e, W2);        // W2 = XER.CA
        emit_arm_carry_from_W(e, W2);   // ARM C = XER.CA
        e.ADCS_W(W0, W0, W1);           // W0 = rA + rB + C; new ARM C = carry
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // addze rD, rA  (rD = rA + 0 + XER.CA; addzeo = 714)
    case 714:
    case 202:
        emit_load_gpr(e, W0, rA);
        emit_load_xer_ca(e, W1);           // W1 = CA (0 or 1)
        e.ADDS_W(W0, W0, W1);              // W0 = rA + CA; ARM C = carry = new XER.CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // addme rD, rA  (rD = rA + 0xFFFFFFFF + XER.CA; addmeo = 746)
    case 746:
    case 234:
        emit_load_gpr(e, W0, rA);
        emit_load_xer_ca(e, W1);
        emit_arm_carry_from_W(e, W1);
        e.MVN_W(W1, A64_WZR);           // W1 = 0xFFFFFFFF
        e.ADCS_W(W0, W0, W1);           // rA + 0xFFFFFFFF + CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // subf rD, rA, rB  (rD = rB - rA; subfo = 552)
    case 552:
    case 40:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        if (rc) {
            e.SUBS_W(W0, W0, W1);
            emit_store_gpr(e, W0, rD);
            emit_cr_from_arith_flags(e, 0);
        } else {
            e.SUB_W(W0, W0, W1);
            emit_store_gpr(e, W0, rD);
        }
        return true;

    // subfc rD, rA, rB  (rD = rB - rA, XER.CA = ~borrow = (rB>=rA); subfco = 520)
    case 520:
    case 8:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        e.SUBS_W(W0, W0, W1);           // ARM C = 1 if rB >= rA (no borrow) = PPC CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // subfe rD, rA, rB  (rD = ~rA + rB + XER.CA; subfeo = 648)
    // = rB - rA - NOT(XER.CA) in ARM terms (SBC with C=XER.CA)
    case 648:
    case 136:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2);
        e.SBCS_W(W0, W0, W1);           // W0 = rB - rA - NOT(C) = ~rA + rB + CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // subfze rD, rA  (rD = ~rA + 0 + XER.CA = 0 - rA - NOT(CA); subfzeo = 712)
    case 712:
    case 200:
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2);
        e.SBCS_W(W0, A64_WZR, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // subfme rD, rA  (rD = ~rA + 0xFFFFFFFF + XER.CA; subfmeo = 744)
    // = 0xFFFFFFFF - rA - NOT(CA) via SBCS
    case 744:
    case 232:
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2);
        e.MVN_W(W0, A64_WZR);           // W0 = 0xFFFFFFFF
        e.SBCS_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_cr_from_arith_flags(e, 0);
        return true;

    // neg rD, rA  (nego = 616: OE ignored)
    case 616:
    case 104:
        emit_load_gpr(e, W0, rA);
        if (rc) {
            e.NEGS_W(W0, W0);
            emit_store_gpr(e, W0, rD);
            emit_cr_from_arith_flags(e, 0);
        } else {
            e.NEG_W(W0, W0);
            emit_store_gpr(e, W0, rD);
        }
        return true;

    // or/mr rA, rS, rB  (note: PPC uses rD/rA/rB encoding, but or uses rS in rD field)
    case 444:
        if (rD == rB) {  // mr rA, rS: rS | rS = rS
            if (rc == 0 && rA == rD) return true;
            emit_load_gpr(e, W0, rD);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORR_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // xor rA, rS, rB
    case 316:
        if (rD == rB) {  // xor with self = 0
            e.MOV_W32(W0, 0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_cr0_const_result(e, 0);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.EOR_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // and rA, rS, rB
    case 28:
        if (rD == rB) {  // and with self = identity (mr)
            if (rc == 0 && rA == rD) return true;
            emit_load_gpr(e, W0, rD);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        if (rc) {
            e.ANDS_W(W0, W0, W1);          // sets Z/N flags → skip CMP in CR update
            emit_store_gpr(e, W0, rA);
            emit_cr_from_flags_signed(e, 0);
        } else {
            e.AND_W(W0, W0, W1);
            emit_store_gpr(e, W0, rA);
        }
        return true;

    // nor rA, rS, rB
    case 124:
        if (rD == rB) {  // nor rA, rS, rS = ~rS
            emit_load_gpr(e, W0, rD);
            e.MVN_W(W0, W0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORR_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // andc rA, rS, rB  (rA = rS & ~rB)
    case 60:
        if (rD == rB) {  // rS & ~rS = 0
            e.MOV_W32(W0, 0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_cr0_const_result(e, 0);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        if (rc) {
            e.BICS_W(W0, W0, W1);          // rS & ~rB, sets Z/N flags → skip CMP
            emit_store_gpr(e, W0, rA);
            emit_cr_from_flags_signed(e, 0);
        } else {
            e.BIC_W(W0, W0, W1);
            emit_store_gpr(e, W0, rA);
        }
        return true;

    // orc rA, rS, rB  (rA = rS | ~rB)
    case 412:
        if (rD == rB) {  // rS | ~rS = 0xFFFFFFFF
            e.MOV_W32(W0, 0xFFFFFFFFu);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_cr0_const_result(e, -1);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORN_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // nand rA, rS, rB
    case 476:
        if (rD == rB) {  // ~(rS & rS) = ~rS
            emit_load_gpr(e, W0, rD);
            e.MVN_W(W0, W0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.AND_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // eqv rA, rS, rB  (XNOR: rA = ~(rS ^ rB))
    case 284:
        if (rD == rB) {  // ~(rS ^ rS) = ~0 = 0xFFFFFFFF
            e.MOV_W32(W0, 0xFFFFFFFFu);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_cr0_const_result(e, -1);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.EOR_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // slw rA, rS, rB
    // PPC spec: if rB[26] (bit5) is set (shift >= 32), result = 0.
    // ARM LSLV uses rB[4:0] (mod 32), so shifts of 32-63 would give wrong result.
    case 24:
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.LSL_W(W0, W0, W1);                   // W0 = rS << (rB & 31)
        e.TST_W_BITMASK(W1, 27, 0);            // Z=0 if bit5 of rB set (shift >= 32)
        e.CSEL_W(W0, A64_WZR, W0, A64_NE);    // result = 0 if shift >= 32
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // srw rA, rS, rB
    // Same shift>=32 issue as slw.
    case 536:
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.LSR_W(W0, W0, W1);                   // W0 = rS >> (rB & 31)
        e.TST_W_BITMASK(W1, 27, 0);            // Z=0 if bit5 of rB set (shift >= 32)
        e.CSEL_W(W0, A64_WZR, W0, A64_NE);    // result = 0 if shift >= 32
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // sraw rA, rS, rB  (arithmetic shift right; also updates XER.CA)
    case 792: {
        emit_load_gpr(e, W0, rD);       // W0 = rS
        emit_load_gpr(e, W1, rB);       // W1 = rB
        e.AND_W_BITMASK(W1, W1, 0, 5); // W1 = sh = rB & 63  (immr=0, imms=5 → mask=0x3F)

        // Result: for sh>=32, sign_ext(rS); for sh<32, ASRV(rS, sh)
        // ARM64 ASRV_W uses W1[4:0] (mod 32) — correct only for sh < 32
        e.ASR_W_IMM(W2, W0, 31);        // W2 = sign_ext(rS) [used when sh >= 32]
        e.ASR_W(W3, W0, W1);            // W3 = rS >> (sh & 31)
        e.CMP_W_IMM(W1, 32);            // compare sh vs 32 — flags reused below
        e.CSEL_W(W2, W2, W3, A64_CS);  // result = (sh >= 32) ? sign_ext : shifted

        // XER.CA: lost = rS & mask; CA = (rS < 0) AND (lost != 0)
        // mask = (1<<sh)-1 for sh<32; all-ones for sh>=32
        // CMP flags from above are still valid across the non-flag-setting MOV/LSL/SUB/MVN.
        e.MOV_W32(W3, 1);
        e.LSL_W(W3, W3, W1);            // W3 = 1 << (sh & 31) [mask helper]
        e.SUB_W_IMM(W3, W3, 1);         // W3 = (1<<sh)-1 [small-shift mask]
        e.MVN_W(W4, A64_WZR);           // W4 = 0xFFFFFFFF [large-shift mask]
        e.CSEL_W(W3, W4, W3, A64_CS);  // W3 = (sh >= 32) ? 0xFFFFFFFF : mask  [no CMP needed]

        e.ANDS_W(W3, W0, W3);           // W3 = rS & mask; Z = (lost bits == 0)
        e.CSET_W(W3, A64_NE);           // W3 = 1 if lost bits != 0
        e.AND_W_ASR(W3, W3, W0, 31);   // W3 = CA (W3 & sign_replicated(rS))

        e.STRB(W3, PPC_PTR, OFF_XER_CA);

        emit_store_gpr(e, W2, rA);
        if (rc) {
            e.CMP_W_IMM(W2, 0);
            emit_cr_from_flags_signed(e, 0);
        }
        return true;
    }

    // srawi rA, rS, SH  (arithmetic shift right immediate; also updates XER.CA)
    case 824: {
        int sh = (op >> 11) & 0x1F;
        emit_load_gpr(e, W0, rD);   // W0 = rS (in rD field)
        if (sh == 0) {
            // No shift: result = rS, XER.CA = 0
            emit_store_gpr(e, W0, rA);
            e.STRB(A64_WZR, PPC_PTR, OFF_XER_CA);  // clear XER.CA
        } else {
            // XER.CA = (rS < 0) && ((rS & ((1<<sh)-1)) != 0)
            e.ANDS_W_BITMASK(W1, W0, 0, sh - 1); // W1 = lost bits; Z=1 if none lost
            e.CSET_W(W3, A64_NE);                 // W3 = 1 if lost != 0
            e.ASR_W_IMM(W0, W0, sh);              // W0 = result
            e.AND_W_ASR(W3, W3, W0, 31);         // W3 = CA (W3 & sign_replicated(result))
            emit_store_gpr(e, W0, rA);
            e.STRB(W3, PPC_PTR, OFF_XER_CA);
        }
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // extsh rA, rS  (sign extend halfword)
    case 922:
        emit_load_gpr(e, W0, rD);
        e.SXTH_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // extsb rA, rS  (sign extend byte)
    case 954:
        emit_load_gpr(e, W0, rD);
        e.SXTB_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // cntlzw rA, rS
    case 26:
        emit_load_gpr(e, W0, rD);
        e.CLZ_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_cr0_nonneg(e);   // CLZ result is 0-32, always non-negative
        return true;

    // mulhwu rD, rA, rB  (unsigned multiply high: upper 32 bits of 64-bit product)
    case 11:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.UMULL_X(0, W0, W1);        // X0 = (uint64)rA * (uint64)rB
        e.LSR_X_IMM(0, 0, 32);       // X0 >>= 32 (upper 32 bits now in W0)
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mulhw rD, rA, rB  (signed multiply high)
    case 75:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.SMULL_X(0, W0, W1);        // X0 = (int64)rA * (int64)rB
        e.LSR_X_IMM(0, 0, 32);       // X0 >>= 32
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mullw rD, rA, rB  (mullwo = 747: OE ignored)
    case 747:
    case 235:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.MUL_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // divwu rD, rA, rB  (unsigned; divwuo = 971: OE ignored)
    case 971:
    case 459:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.UDIV_W(W0, W0, W1);          // ARM64: divide-by-zero gives 0 (PPC: undefined)
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // divw rD, rA, rB  (signed; divwo = 1003: OE ignored)
    case 1003:
    case 491:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.SDIV_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mfcr rD: pack all 8 CR fields into a 32-bit value
    case 19: {
        e.LDRB(W0, PPC_PTR, OFF_CR + 0);
        for (int i = 1; i < 8; i++) {
            e.LDRB(W1, PPC_PTR, OFF_CR + i);
            e.ORR_W_LSL(W0, W1, W0, 4);   // W0 = W1 | (W0 << 4)
        }
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // mtcrf CRM, rS: unpack 32-bit value into CR fields selected by CRM
    case 144: {
        int crm = (op >> 12) & 0xFF;
        emit_load_gpr(e, W0, rD);          // rS is in rD field
        for (int i = 0; i < 8; i++) {
            if (!(crm & (0x80 >> i))) continue;  // bit not set: skip this field
            // Extract nibble i from W0: bits [31-4i : 28-4i]
            e.UBFM_W(W1, W0, 28 - 4 * i, 31 - 4 * i);
            e.STRB(W1, PPC_PTR, OFF_CR + i);
        }
        return true;
    }

    // mfmsr rD  (move from machine state register)
    case 83:
        e.LDR_W(W0, PPC_PTR, OFF_MSR);
        emit_store_gpr(e, W0, rD);
        return true;

    // mfspr rD, SPR: inline common SPRs; fall back for others
    case 339: {
        int spr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        switch (spr) {
        case 1: {
            e.LDR_W(W0, PPC_PTR, OFF_XER);
            e.LDRB(W1, PPC_PTR, OFF_XER_CA);
            e.BFI_W(W0, W1, 29, 1);              // reconstruct CA from xer_ca cache
            emit_store_gpr(e, W0, rD); return true;
        }
        case 8:   e.LDR_W(W0, PPC_PTR, OFF_LR);            emit_store_gpr(e, W0, rD); return true;
        case 9:   e.LDR_W(W0, PPC_PTR, OFF_CTR);           emit_store_gpr(e, W0, rD); return true;
        case 18:  e.LDR_W(W0, PPC_PTR, (uint32_t)OFF_DSISR); emit_store_gpr(e, W0, rD); return true;
        case 19:  e.LDR_W(W0, PPC_PTR, (uint32_t)OFF_DAR);   emit_store_gpr(e, W0, rD); return true;
        case 22:  return false;  // mfspr DEC: fall back so read_decrementer() returns cycle-adjusted value
        case 26:  e.LDR_W(W0, PPC_PTR, OFF_SRR0);          emit_store_gpr(e, W0, rD); return true;
        case 27:  e.LDR_W(W0, PPC_PTR, OFF_SRR1);          emit_store_gpr(e, W0, rD); return true;
        case 272: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 0);      emit_store_gpr(e, W0, rD); return true;
        case 273: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 4);      emit_store_gpr(e, W0, rD); return true;
        case 274: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 8);      emit_store_gpr(e, W0, rD); return true;
        case 275: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 12);     emit_store_gpr(e, W0, rD); return true;
        case 268: emit_call(e, (uint64_t)(void *)&jit_read_tbl); emit_store_gpr(e, W0, rD); return true;
        case 269: emit_call(e, (uint64_t)(void *)&jit_read_tbu); emit_store_gpr(e, W0, rD); return true;
        default:  return false;
        }
    }

    // mftb rD, TBR  (read timebase: TBR=268→TBL, TBR=269→TBU)
    case 371: {
        int tbr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        if (tbr == 268) {
            emit_call(e, (uint64_t)(void *)&jit_read_tbl);
            emit_store_gpr(e, W0, rD);
            return true;
        } else if (tbr == 269) {
            emit_call(e, (uint64_t)(void *)&jit_read_tbu);
            emit_store_gpr(e, W0, rD);
            return true;
        }
        return false;
    }

    // mtspr SPR, rS: inline common SPRs
    case 467: {
        int spr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        switch (spr) {
        case 1: {
            emit_load_gpr(e, W0, rD);
            e.STR_W(W0, PPC_PTR, OFF_XER);
            e.UBFM_W(W1, W0, 29, 29);            // extract new CA bit
            e.STRB(W1, PPC_PTR, OFF_XER_CA);     // sync xer_ca cache
            return true;
        }
        case 8:   emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_LR);        return true;
        case 9:   emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_CTR);       return true;
        case 18:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, (uint32_t)OFF_DSISR); return true;
        case 19:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, (uint32_t)OFF_DAR);   return true;
        case 22:  return false;  // mtspr DEC: fall back so write_decrementer() recalculates dec_trigger_cycle
        case 26:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SRR0);      return true;
        case 27:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SRR1);      return true;
        case 272: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 0);  return true;
        case 273: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 4);  return true;
        case 274: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 8);  return true;
        case 275: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 12); return true;
        default:  return false;
        }
    }

    // mtmsr rS  (write machine state register)
    // Falls back so ppc_set_msr() runs: checks LE mode, calls ppc603_check_interrupts()
    // to deliver any interrupt newly enabled by the MSR write. Block terminates so the
    // (possibly modified) NPC takes effect before the next block executes.
    case 146:
        return false;

    // mcrxr crfD — copy XER[SO,OV,CA,0] to CR field crfD, then clear XER[31:28]
    case 512: {
        int crfD = (op >> 23) & 0x7;
        e.LDR_W(W0, PPC_PTR, OFF_XER);
        e.LDRB(W1, PPC_PTR, OFF_XER_CA);
        e.BFI_W(W0, W1, 29, 1);                    // reconstruct CA from xer_ca cache
        e.LSR_W_IMM(W1, W0, 28);                   // W1 = SO:OV:CA:0 (bits 3:0)
        e.STRB(W1, PPC_PTR, OFF_CR + crfD);
        e.AND_W_BITMASK(W0, W0, 0, 27);            // clear top nibble (mask=0x0FFFFFFF)
        e.STR_W(W0, PPC_PTR, OFF_XER);
        e.STRB(A64_WZR, PPC_PTR, OFF_XER_CA);      // clear xer_ca cache
        return true;
    }

    // mfsr rD, SR  — move from segment register (SR index in bits 19:16)
    case 595: {
        int sr = (op >> 16) & 0xF;
        e.LDR_W(W0, PPC_PTR, (uint32_t)(OFF_SR + sr * 4));
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // mtsr SR, rS  — move to segment register (rS in rD field)
    case 210: {
        int sr = (op >> 16) & 0xF;
        emit_load_gpr(e, W0, rD);
        e.STR_W(W0, PPC_PTR, (uint32_t)(OFF_SR + sr * 4));
        return true;
    }

    // mfsrin rD, rB  — move from segment register indirect (sr index = rB[31:28])
    case 659: {
        emit_load_gpr(e, W1, rB);
        e.LSR_W_IMM(W1, W1, 28);              // W1 = sr_index (0-15)
        e.LSL_W_IMM(W1, W1, 2);               // W1 = sr_index * 4
        e.ADD_W_IMM(W1, W1, (uint32_t)OFF_SR); // W1 = byte offset in struct
        e.ADD_X_UXTW(1, PPC_PTR, W1);         // X1 = &ppc.sr[sr_index]
        e.LDR_W(W0, 1, 0);
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // mtsrin rS, rB  — move to segment register indirect (rS in rD field)
    case 242: {
        emit_load_gpr(e, W1, rB);
        e.LSR_W_IMM(W1, W1, 28);
        e.LSL_W_IMM(W1, W1, 2);
        e.ADD_W_IMM(W1, W1, (uint32_t)OFF_SR);
        e.ADD_X_UXTW(1, PPC_PTR, W1);         // X1 = &ppc.sr[sr_index]
        emit_load_gpr(e, W0, rD);             // rS in rD field
        e.STR_W(W0, 1, 0);
        return true;
    }

    // tw rA, rB / twi — trap: NOP (traps don't occur in normal game code)
    case 4:
        return true;

    // Cache and TLB management — all NOPs in emulator (no real cache/TLB to manage)
    case 54:    // dcbst
    case 86:    // dcbf
    case 246:   // dcbtst
    case 278:   // dcbt
    case 470:   // dcbi
    case 982:   // icbi
    case 306:   // tlbie
    case 370:   // tlbia
    case 566:   // tlbsync
        return true;

    // sync / lwsync: memory barrier — NOP in emulator
    case 598:
        return true;

    // eieio: enforce in-order execution of I/O — NOP in emulator
    case 854:
        return true;

    // dcbz: data cache block zero — NOP (no cache to zero in emulator)
    case 1014:
        return true;

    // lwarx rD, rA, rB — load and reserve (single-core: treat as plain lwzx; no reservation tracking)
    case 20:  return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read32, false);

    // stwcx. rS, rA, rB — store conditional (single-core: always succeeds; set CR0[EQ]=1)
    case 150: {
        translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write32);
        e.MOV_W32(W0, 2);                     // EQ nibble (LT=GT=SO=0, EQ=1)
        e.STRB(W0, PPC_PTR, OFF_CR + 0);
        // Set ARM Z=1 so the Rc=1 peephole (which fuses a following bc on CR0[EQ]
        // with live ARM flags) sees the correct outcome. Without this, a stale Z=0
        // from a preceding andi./cmp causes the CAS retry branch to always re-fire,
        // preventing the post-CAS kick code from ever executing.
        e.CMP_W_IMM(W0, 2);                   // W0==2 → Z=1 (EQ success)
        return true;
    }

    // ---- Indexed loads (X-form) ----
    // lwzx rD, rA, rB
    case 23:  return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read32, false);
    // lwzux rD, rA, rB
    case 55:  return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read32, false);
    // lbzx rD, rA, rB
    case 87:  return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read8, false);
    // lbzux rD, rA, rB
    case 119: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read8, false);
    // lhzx rD, rA, rB
    case 279: return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read16, false);
    // lhzux rD, rA, rB
    case 311: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read16, false);
    // lhax rD, rA, rB  (sign-extend)
    case 343: return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read16, true);
    // lhaux rD, rA, rB
    case 375: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read16, true);

    // ---- Indexed stores (X-form, rS is in rD field) ----
    // stwx rS, rA, rB
    case 151: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write32);
    // stwux rS, rA, rB
    case 183: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write32);
    // stbx rS, rA, rB
    case 215: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write8);
    // stbux rS, rA, rB
    case 247: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write8);
    // sthx rS, rA, rB
    case 407: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write16);
    // sthux rS, rA, rB
    case 439: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write16);

    // ---- Indexed FP loads (X-form) ----
    // lfsx rD, rA, rB
    case 535: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_ram_load32(e);                              // lfsx — fastmem
        e.FMOV_S_W(D0, W0);
        e.FCVT_D_S(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;
    }
    // lfsux rD, rA, rB
    case 567: {
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        emit_ram_load32(e);                              // lfsux — fastmem
        e.FMOV_S_W(D0, W0);
        e.FCVT_D_S(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;
    }
    // lfdx rD, rA, rB
    case 599: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read64);
        e.STR_X(W0, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        return true;
    }
    // lfdux rD, rA, rB
    case 631: {
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        emit_call(e, (uint64_t)(void *)&jit_read64);
        e.STR_X(W0, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        return true;
    }

    // ---- Indexed FP stores (X-form, rS in rD field) ----
    // stfsx rS, rA, rB
    case 663: {
        emit_load_fpr(e, D0, rD);
        e.FCVT_S_D(D0, D0);
        emit_load_ea_reg(e, W0, rA, rB);  // may clobber W1; D0 is safe
        e.FMOV_W_S(W1, D0);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // stfsux rS, rA, rB
    case 695: {
        emit_load_fpr(e, D0, rD);
        e.FCVT_S_D(D0, D0);
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        e.FMOV_W_S(W1, D0);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // stfdx rS, rA, rB
    case 727: {
        // emit_load_ea_reg may use W1 as scratch; load FPR into X1 after EA is ready.
        emit_load_ea_reg(e, W0, rA, rB);  // W0 = EA (may clobber W1)
        e.LDR_X(W1, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));  // X1 = raw double bits
        emit_call(e, (uint64_t)(void *)&jit_write64);
        return true;
    }
    // stfdux rS, rA, rB
    case 759: {
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);             // W0 = EA
        emit_store_gpr(e, W0, rA);
        e.LDR_X(W1, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));  // X1 = raw double bits
        emit_call(e, (uint64_t)(void *)&jit_write64);
        return true;
    }

    // stfiwx frS, rA, rB — store lower 32 bits of FPR as integer word (from fctiwz result)
    case 983: {
        // EA first (may clobber W1), then load data into W1
        emit_load_ea_reg(e, W0, rA, rB);
        e.LDR_W(W1, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }

    // ---- Byte-reverse indexed loads ----
    // lwbrx rD, rA, rB — load word byte-reversed
    case 534: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_ram_load32(e);                              // lwbrx — fastmem
        e.REV_W(W0, W0);
        emit_store_gpr(e, W0, rD);
        return true;
    }
    // lhbrx rD, rA, rB — load halfword byte-reversed (zero-extend)
    case 790: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read16);
        e.REV16_W(W0, W0);                  // 0x0000AABB → 0x0000BBAA
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // ---- Byte-reverse indexed stores ----
    // stwbrx rS, rA, rB — store word byte-reversed
    case 662: {
        emit_load_ea_reg(e, W0, rA, rB);    // EA (may clobber W1)
        emit_load_gpr(e, W1, rD);           // rD field = rS for stores
        e.REV_W(W1, W1);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // sthbrx rS, rA, rB — store halfword byte-reversed
    case 918: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_load_gpr(e, W1, rD);
        e.REV16_W(W1, W1);
        emit_call(e, (uint64_t)(void *)&jit_write16);
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Opcode-31 indexed load/store sub-operations (X-form)
// These are added inline to translate_op31; declared here as forward refs aren't needed.
// ---------------------------------------------------------------------------

// Helper for X-form indexed load: emit EA→W0, call reader, sign-ext if lha, store to rD
static bool translate_op31_load_x(Arm64Emitter &e, int rD, int rA, int rB,
                                   void *reader, bool sign_ext)
{
    emit_load_ea_reg(e, W0, rA, rB);
    if (reader == (void *)&jit_read32)       emit_ram_load32(e);
    else                                     emit_call(e, (uint64_t)reader);
    if (sign_ext) e.SXTH_W(W0, W0);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Helper for X-form indexed load with update: ea=rA+rB, rA=ea, load, store to rD
static bool translate_op31_load_xu(Arm64Emitter &e, int rD, int rA, int rB,
                                    void *reader, bool sign_ext)
{
    emit_load_gpr(e, W0, rA);
    emit_load_gpr(e, W1, rB);
    e.ADD_W(W0, W0, W1);                // W0 = EA
    emit_store_gpr(e, W0, rA);          // rA = EA
    if (reader == (void *)&jit_read32)       emit_ram_load32(e);
    else                                     emit_call(e, (uint64_t)reader);
    if (sign_ext) e.SXTH_W(W0, W0);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Helper for X-form indexed store: EA→W0, data→W1, call writer
static bool translate_op31_store_x(Arm64Emitter &e, int rS, int rA, int rB, void *writer)
{
    emit_load_ea_reg(e, W0, rA, rB);    // W0 = EA (uses W1 as scratch)
    emit_load_gpr(e, W1, rS);           // W1 = data (after scratch use is done)
    emit_call(e, (uint64_t)writer);
    return true;
}

// Helper for X-form indexed store with update: rS loaded first to handle rS==rA
static bool translate_op31_store_xu(Arm64Emitter &e, int rS, int rA, int rB, void *writer)
{
    emit_load_gpr(e, W2, rS);           // W2 = data (save before potential rA overwrite)
    emit_load_gpr(e, W0, rA);
    emit_load_gpr(e, W1, rB);
    e.ADD_W(W0, W0, W1);                // W0 = EA
    emit_store_gpr(e, W0, rA);          // rA = EA
    e.MOV_W(W1, W2);                    // W1 = data
    emit_call(e, (uint64_t)writer);
    return true;
}

// ---------------------------------------------------------------------------
// Branch translation helpers
// These always set npc and return true (they terminate the block).
// ---------------------------------------------------------------------------

// Primary opcode 16: bc BO, BI, BD [, AA] [, LK]  — fully inlined
// Returns true if block is terminated (always for bc).
// taken_fn / not_taken_fn: if non-null, use chained epilogue for that path.
static bool translate_bc(Arm64Emitter &e, uint32_t op, uint32_t pc, int inst_count,
                          std::vector<std::pair<uint32_t, uint32_t*>> &pending_fixups,
                          void *taken_fn = nullptr, void *not_taken_fn = nullptr)
{
    int bo = (op >> 21) & 0x1F;
    int bi = (op >> 16) & 0x1F;
    int aa = (op >>  1) & 1;
    int lk = op & 1;
    int32_t bd = (int32_t)((op & 0xFFFC) << 16) >> 16;  // sign-extend 14-bit field
    uint32_t taken_target = aa ? (uint32_t)bd : (pc + (uint32_t)bd);

    bool ctr_relevant  = !(bo & 0x04);
    bool cond_relevant = !(bo & 0x10);
    bool ctr_zero      = (bo & 0x02) != 0;  // true → branch if CTR==0 after decrement (bdz); false → bdnz
    bool cond_on_clear = !(bo & 0x08);   // true → branch if CR bit CLEAR; false → SET

    if (!ctr_relevant && !cond_relevant) {
        // Unconditional bc (bo=0b10100): always taken, no branch needed
        if (lk) { e.MOV_W32(W0, pc + 4); e.STR_W(W0, PPC_PTR, OFF_LR); }
        if (taken_fn)
            emit_epilogue_chained(e, inst_count + 1, pc, taken_target, taken_fn);
        else {
            uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, taken_target);
            pending_fixups.push_back({taken_target, site});
        }
        return true;
    }

    // Helper lambda to emit taken and not-taken epilogues after the branch instruction
    // not_taken_patch always points to a B.cond instruction for the two fast-path cases,
    // or a CBZ for the combined case — handled by separate code paths below.

    if (ctr_relevant && !cond_relevant) {
        // CTR-only (bdnz / bdz): branch directly from SUBS flags — saves 3 instructions
        e.LDR_W(W1, PPC_PTR, OFF_CTR);
        e.SUBS_W_IMM(W1, W1, 1);                           // decrement; Z=1 if now 0
        e.STR_W(W1, PPC_PTR, OFF_CTR);
        if (lk) { e.MOV_W32(W0, pc + 4); e.STR_W(W0, PPC_PTR, OFF_LR); }
        // bdz→taken when EQ; bdnz→taken when NE; not_taken → opposite
        uint32_t *nt = e.emit_B_COND_placeholder(ctr_zero ? A64_NE : A64_EQ);
        if (taken_fn)  emit_epilogue_chained(e, inst_count + 1, pc, taken_target, taken_fn);
        else {
            uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, taken_target);
            pending_fixups.push_back({taken_target, site});
        }
        e.patch_B_COND(nt, e.ptr());
        if (not_taken_fn) emit_epilogue_chained(e, inst_count + 1, pc, pc + 4, not_taken_fn);
        else {
            uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, pc + 4);
            pending_fixups.push_back({pc + 4, site});
        }
        return true;
    }

    if (!ctr_relevant && cond_relevant) {
        // Cond-only (beq/bne/blt/etc.): use TBZ/TBNZ on the raw CR byte — saves UBFM + CMP
        int crfD  = bi / 4;
        int crbit = bi % 4;                                 // 0=LT,1=GT,2=EQ,3=SO
        int bitpos = 3 - crbit;                             // bit position in CR byte (3=LT,2=GT,1=EQ,0=SO)
        e.LDRB(W1, PPC_PTR, OFF_CR + crfD);
        if (lk) { e.MOV_W32(W0, pc + 4); e.STR_W(W0, PPC_PTR, OFF_LR); }
        // cond_on_clear: taken when bit==0 → jump to not_taken when bit!=0 (TBNZ)
        // cond_on_set:   taken when bit!=0 → jump to not_taken when bit==0 (TBZ)
        uint32_t *nt = cond_on_clear ? e.emit_TBNZ_W_placeholder(W1, bitpos)
                                     : e.emit_TBZ_W_placeholder(W1, bitpos);
        if (taken_fn)  emit_epilogue_chained(e, inst_count + 1, pc, taken_target, taken_fn);
        else {
            uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, taken_target);
            pending_fixups.push_back({taken_target, site});
        }
        e.patch_TBZ(nt, e.ptr());
        if (not_taken_fn) emit_epilogue_chained(e, inst_count + 1, pc, pc + 4, not_taken_fn);
        else {
            uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, pc + 4);
            pending_fixups.push_back({pc + 4, site});
        }
        return true;
    }

    // Both CTR and COND must be satisfied: CCMP fuses the two checks into flags
    e.LDR_W(W1, PPC_PTR, OFF_CTR);
    e.SUBS_W_IMM(W1, W1, 1);                // NZCV = ctr condition
    e.STR_W(W1, PPC_PTR, OFF_CTR);

    {
        int crfD  = bi / 4;
        int crbit = bi % 4;
        int bitpos = 3 - crbit;
        e.LDRB(W1, PPC_PTR, OFF_CR + crfD);
        e.UBFM_W(W1, W1, bitpos, bitpos);    // W1 = CR bit (0 or 1)
        // CCMP: if ctr_cond, compare W1 with 1 (Z=1 iff bit set); else force flags.
        // For cond_on_set (taken when bit=1): nzcv_fail=0 → Z=0 when ctr fails (not taken).
        // For cond_on_clear (taken when bit=0): nzcv_fail=4 → Z=1 when ctr fails (not taken).
        int nzcv_fail = cond_on_clear ? 4 : 0;
        e.CCMP_W_IMM(W1, 1, nzcv_fail, ctr_zero ? A64_EQ : A64_NE);
        // After CCMP: Z=1 iff (bit set AND ctr_cond) for cond_on_set;
        //             Z=0 iff (bit clear AND ctr_cond) for cond_on_clear.
    }

    if (lk) { e.MOV_W32(W1, pc + 4); e.STR_W(W1, PPC_PTR, OFF_LR); }
    // Branch to not-taken path if condition fails:
    // cond_on_set: taken when Z=1, skip on Z=0 → B.NE
    // cond_on_clear: taken when Z=0, skip on Z=1 → B.EQ
    uint32_t *nt = e.emit_B_COND_placeholder(cond_on_clear ? A64_EQ : A64_NE);
    if (taken_fn)  emit_epilogue_chained(e, inst_count + 1, pc, taken_target, taken_fn);
    else {
        uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, taken_target);
        pending_fixups.push_back({taken_target, site});
    }
    e.patch_B_COND(nt, e.ptr());
    if (not_taken_fn) emit_epilogue_chained(e, inst_count + 1, pc, pc + 4, not_taken_fn);
    else {
        uint32_t *site = emit_epilogue_deferred(e, inst_count + 1, pc, pc + 4);
        pending_fixups.push_back({pc + 4, site});
    }
    return true;
}

// Primary opcode 18: b BD [, AA] [, LK]
// Returns the branch target; emits LR update if LK.
static uint32_t translate_b(Arm64Emitter &e, uint32_t op, uint32_t pc)
{
    int aa = (op >> 1) & 1;    // absolute address
    int lk = op & 1;            // link (bl)
    int32_t bd = (int32_t)((op & 0x03FFFFFC) << 6) >> 6;  // sign-extend 26-bit

    uint32_t target = aa ? (uint32_t)bd : (pc + (uint32_t)bd);

    if (lk) {
        e.MOV_W32(W0, pc + 4);
        e.STR_W(W0, PPC_PTR, OFF_LR);
    }
    return target;
}

// bclr: primary 19, subop 16 (branch to LR)
// bcctr: primary 19, subop 528 (branch to CTR)
// These always terminate the block.

// ---------------------------------------------------------------------------
// Additional primary-opcode translators
// ---------------------------------------------------------------------------

// Shared helper: compute EA = (rA==0 ? 0 : REG(rA)) + offset32  → W0
static void emit_load_ea_off32(Arm64Emitter &e, int rA, int32_t off)
{
    if (rA == 0) {
        e.MOV_W32(W0, (uint32_t)off);
    } else {
        emit_load_gpr(e, W0, rA);
        if      (off == 0)                          { /* nothing */ }
        else if (off > 0 && off <= 4095)            e.ADD_W_IMM(W0, W0, (uint32_t)off);
        else if (off < 0 && -off <= 4095)           e.SUB_W_IMM(W0, W0, (uint32_t)(-off));
        else if (off > 0 && (off & 0xFFF) == 0 && (off >> 12) <= 4095)
            e.ADD_W_IMM(W0, W0, (uint32_t)off >> 12, 1);
        else if (off < 0 && ((-off) & 0xFFF) == 0 && ((-off) >> 12) <= 4095)
            e.SUB_W_IMM(W0, W0, (uint32_t)(-off) >> 12, 1);
        else { e.MOV_W32(W1, (uint32_t)off); e.ADD_W(W0, W0, W1); }
    }
}

// Primary opcode 8: subfic rD, rA, SIMM  (rD = SIMM - rA, XER.CA = (SIMM >= rA unsigned))
static bool translate_subfic(Arm64Emitter &e, uint32_t op)
{
    int rD = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    e.MOV_W32(W1, (uint32_t)(int32_t)simm);
    e.SUBS_W(W0, W1, W0);              // W0 = SIMM - rA; ARM C = 1 if SIMM >= rA (unsigned) = CA
    emit_store_gpr(e, W0, rD);
    emit_update_xer_ca(e);
    return true;
}

// Primary opcodes 12/13: addic[.] rD, rA, SIMM  (rD = rA+SIMM, XER.CA = carry out)
static bool translate_addic(Arm64Emitter &e, uint32_t op, bool update_cr)
{
    int rD = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    if (simm >= 0 && (uint32_t)simm <= 4095) {
        e.ADDS_W_IMM(W0, W0, (uint32_t)simm);
    } else if (simm < 0 && (uint32_t)(-simm) <= 4095) {
        e.SUBS_W_IMM(W0, W0, (uint32_t)(-simm));   // rA + simm = rA - |simm|; C = NOT borrow = CA
    } else if (simm > 0 && ((uint32_t)simm & 0xFFF) == 0) {
        e.ADDS_W_IMM(W0, W0, (uint32_t)simm >> 12, 1);  // simm is multiple of 4096
    } else if (simm < 0 && ((uint32_t)(-simm) & 0xFFF) == 0) {
        e.SUBS_W_IMM(W0, W0, (uint32_t)(-simm) >> 12, 1);
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.ADDS_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rD);
    emit_update_xer_ca(e);
    if (update_cr) emit_cr_from_arith_flags(e, 0);
    return true;
}

// Primary opcode 46: lmw rD, d(rA)  — load registers rD..r31 from consecutive memory
static bool translate_lmw(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    int n    = 32 - rD;

    for (int i = 0; i < n; i++) {
        emit_load_ea_off32(e, rA, (int32_t)simm + i * 4);  // W0 = EA
        emit_ram_load32(e);                                  // lmw — fastmem
        emit_store_gpr(e, W0, rD + i);
    }
    return true;
}

// Primary opcode 47: stmw rS, d(rA)  — store registers rS..r31 to consecutive memory
static bool translate_stmw(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    int n    = 32 - rS;

    for (int i = 0; i < n; i++) {
        emit_load_ea_off32(e, rA, (int32_t)simm + i * 4);  // W0 = EA
        emit_load_gpr(e, W1, rS + i);                       // W1 = data
        emit_call(e, (uint64_t)(void *)&jit_write32);
    }
    return true;
}

// ---------------------------------------------------------------------------
// FP load/store helpers: EA in W0 on entry; may clobber W0-W4, D0-D3
// ---------------------------------------------------------------------------

// Compute EA (D-form) into W0.  rA==0 → EA = simm16 only.
// (Reuses emit_load_ea_imm which is already defined above)

// lfs  rD, simm(rA): load float, convert to double, store in FPR
static bool translate_lfs(Arm64Emitter &e, uint32_t op, bool update)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;  // lfsu rA=0 is invalid

    emit_load_ea_imm(e, rA, simm);   // W0 = EA

    if (update) {
        emit_store_gpr(e, W0, rA);   // rA = EA (write-back)
    }
    emit_ram_load32(e);               // lfs/lfsu — fastmem; W0 = float bits
    e.FMOV_S_W(D0, W0);              // D0(S0) = reinterpret as float
    e.FCVT_D_S(D0, D0);              // D0 = (double)float
    emit_store_fpr(e, D0, rD);
    return true;
}

// lfd  rD, simm(rA): load 64-bit double directly into FPR
static bool translate_lfd(Arm64Emitter &e, uint32_t op, bool update)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }
    emit_call(e, (uint64_t)(void *)&jit_read64);   // X0 = 64-bit data
    // Store raw bits directly into the FPR slot — no FMOV needed.
    e.STR_X(W0, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
    return true;
}

// stfs rS, simm(rA): convert FPR double to float, store 32-bit float
static bool translate_stfs(Arm64Emitter &e, uint32_t op, bool update)
{
    int rS    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    emit_load_fpr(e, D0, rS);        // D0 = FPR[rS] (double)
    e.FCVT_S_D(D0, D0);              // S0/D0 = (float)D0
    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }
    e.FMOV_W_S(W1, D0);
    emit_call(e, (uint64_t)(void *)&jit_write32);
    return true;
}

// stfd rS, simm(rA): store 64-bit double
static bool translate_stfd(Arm64Emitter &e, uint32_t op, bool update)
{
    int rS    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    // emit_load_ea_imm uses only W0; load EA first, then read FPR as integer into X1.
    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }
    e.LDR_X(W1, PPC_PTR, (uint32_t)(OFF_FPR + rS * 8));  // X1 = raw double bits
    emit_call(e, (uint64_t)(void *)&jit_write64);
    return true;
}

// Emit a call to jit_set_fprf(rD), replicating the interpreter's set_fprf() side-effect after
// an FP *arithmetic* result is stored. Every interpreter FP arithmetic op updates FPSCR[FPRF];
// the JIT computed the right value but omitted the flag, which broke Daytona 2's opponent AI
// (it reads FPRF back via mffs/mcrfs). Call ONLY after ops the interpreter runs set_fprf for —
// never after moves (fmr/fneg/fabs), compares (fcmp) or fsel, which leave FPRF untouched.
static void emit_set_fprf(Arm64Emitter &e, int rD)
{
    e.MOV_W32(W0, rD);
    emit_call(e, (uint64_t)(void *)&jit_set_fprf);
}

// ---------------------------------------------------------------------------
// Opcode 63: floating-point double-precision arithmetic
// ---------------------------------------------------------------------------
static bool translate_op63(Arm64Emitter &e, uint32_t op)
{
    // Native FP translation is not yet fully equivalent to the interpreter.
    // Harley-Davidson's attract-mode AI visibly diverges with it enabled, while
    // keeping the integer/control/memory JIT and interpreting FP restores the
    // reference behaviour with ample performance headroom.
    constexpr bool native_fp_enabled = false;
    if (!native_fp_enabled) { (void)e; (void)op; return false; }

    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rC  = (op >> 6)  & 0x1F;  // used by fmadd/fmsub family
    int sub = (op >> 1) & 0x3FF;
    (void)rA;  // some sub-ops don't use rA

    // The A-form ops fsel/fmul/fmsub/fmadd/fnmsub/fnmadd encode frC in bits 6-10, which overlap
    // the 10-bit `sub`. Without collapsing it, only their frC==0 forms matched and every real
    // fmul/fmadd fell back to the interpreter. Reduce these to their 5-bit opcode so they match
    // for any frC; the codes {23,25,28..31} collide with no X-form op in the switch below (the
    // interpreter fills all 32 frC slots for exactly these — see optable63 in ppc.cpp).
    {
        int lo = sub & 0x1F;
        if (lo == 23 || lo == 25 || (lo >= 28 && lo <= 31))
            sub = lo;
    }

    switch (sub) {
    case 72:  // fmr rD, rB  (copy FPR)
        emit_load_fpr(e, D0, rB);
        emit_store_fpr(e, D0, rD);
        return true;

    case 40:  // fneg rD, rB
        emit_load_fpr(e, D0, rB);
        e.FNEG_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 264: // fabs rD, rB
        emit_load_fpr(e, D0, rB);
        e.FABS_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 136: // fnabs rD, rB  (force-negative abs)
        emit_load_fpr(e, D0, rB);
        e.FABS_D(D0, D0);
        e.FNEG_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 12:  // frsp rD, rB  — fall back to interpreter (also updates FPSCR[FPRF])
        return false;

    case 14:  // fctiw rD, rB  (round to integer using FPSCR rounding mode)
    // We ignore FPSCR rounding mode and truncate (same as fctiwz) — acceptable for game code
    case 15:  // fctiwz rD, rB  (convert double to int32, sign-extend to 64 bits in FPR)
    {
        emit_load_fpr(e, D0, rB);
        e.FCVTZS_X_D(W0, D0);                        // X0 = sign-extended (int32)D0
        e.STR_X(W0, PPC_PTR, OFF_FPR + rD * 8);
        return true;
    }

    case 21:  // fadd rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FADD_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 20:  // fsub rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FSUB_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 25:  // fmul rD, rA, rC  (note: uses rC not rB!)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        e.FMUL_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 18:  // fdiv rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FDIV_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 22:  // fsqrt rD, rB
        emit_load_fpr(e, D0, rB);
        e.FSQRT_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 29:  // fmadd rD, rA, rC, rB  (rD = rA*rC + rB)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMADD_D(D0, D0, D1, D2);   // D0 = D2 + D0*D1 = rB + rA*rC
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 28:  // fmsub rD, rA, rC, rB  (rD = rA*rC - rB)
        // ARM FNMSUB: -(Da - Dn*Dm) = Dn*Dm - Da = rA*rC - rB ✓
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMSUB_D(D0, D0, D1, D2);  // D0*D1 - D2 = rA*rC - rB
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 31:  // fnmadd rD, rA, rC, rB  (rD = -(rA*rC + rB))
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMADD_D(D0, D0, D1, D2);  // -(D2 + D0*D1) = -(rB + rA*rC) ✓
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 30:  // fnmsub rD, rA, rC, rB  (rD = -(rA*rC - rB) = rB - rA*rC)
        // ARM FMSUB: Da - Dn*Dm = rB - rA*rC ✓
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMSUB_D(D0, D0, D1, D2);   // D2 - D0*D1 = rB - rA*rC
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;

    case 23: {// fsel frD, frA, frC, frB  (frD = frA >= 0.0 ? frC : frB; NaN → frB)
        // Load frA first; FCMP_D_ZERO sets GE=true when frA >= 0 and frA is not NaN
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FCMP_D_ZERO(D0);
        e.FCSEL_D(D0, D1, D2, A64_GE);   // D0 = (GE) ? frC : frB
        emit_store_fpr(e, D0, rD);
        return true;
    }

    case 26: {  // frsqrte frD, frB — reciprocal square root estimate
        emit_load_fpr(e, D0, rB);
        emit_call(e, (uint64_t)(void *)&jit_frsqrte);
        emit_store_fpr(e, D0, rD);
        emit_set_fprf(e, rD);
        return true;
    }

    case 583: {  // mffs frD — move FPSCR to FPR (lower 32 bits)
        // LDR_W zero-extends W0 to 64 bits (ARM64 rule), so STR_X stores
        // FPSCR in the lower 4 bytes and 0 in the upper 4 bytes.
        e.LDR_W(W0, PPC_PTR, OFF_FPSCR);
        e.STR_X(W0, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        return true;
    }

    case 711:   // mtfsf FLM, frB — update FPSCR fields from FPR (NOP: JIT doesn't model FPSCR)
    case 134:   // mtfsfi crfD, IMM — set FPSCR field from immediate (NOP)
    case 70:    // mtfsb0 crbD — clear FPSCR bit (NOP)
    case 38:    // mtfsb1 crbD — set FPSCR bit (NOP)
        return true;

    case 64: {  // mcrfs crfD, crfS — copy FPSCR field to CR field
        int crfD = (op >> 23) & 0x7;
        int crfS = (op >> 18) & 0x7;
        e.LDR_W(W0, PPC_PTR, OFF_FPSCR);
        e.UBFM_W(W1, W0, 28 - crfS * 4, 31 - crfS * 4);  // extract 4-bit field to [3:0]
        e.STRB(W1, PPC_PTR, OFF_CR + crfD);
        return true;
    }

    case 32:  // fcmpo crD, rA, rB  (ordered FP compare)
    case 0:   // fcmpu crD, rA, rB  (unordered FP compare)
    {
        int crfD = (op >> 23) & 0x7;
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FCMP_D(D0, D1);            // sets NZCV: N=LT, Z=EQ, C=!LT (ordered), V=unordered
        // Build PPC CR field: bit3=LT, bit2=GT, bit1=EQ, bit0=FU (unordered)
        e.CSET_W(W0, A64_MI);        // W0 = LT
        e.CSET_W(W1, A64_GT);        // W1 = GT
        e.CSET_W(W2, A64_EQ);        // W2 = EQ
        e.CSET_W(W3, A64_VS);        // W3 = FU (unordered)
        e.ORR_W_LSL(W1, W3, W1, 2);  // W1 = FU | (GT<<2)
        e.ORR_W_LSL(W0, W1, W0, 3);  // W0 = (FU|GT<<2) | (LT<<3)
        e.ORR_W_LSL(W0, W0, W2, 1);  // W0 |= EQ<<1
        e.STRB(W0, PPC_PTR, OFF_CR + crfD);
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Opcode 59: floating-point single-precision arithmetic
// Same as op63 but results are rounded to single precision after each op.
// ---------------------------------------------------------------------------
static bool translate_op59(Arm64Emitter &e, uint32_t op)
{
    constexpr bool native_fp_enabled = false;
    if (!native_fp_enabled) { (void)e; (void)op; return false; }

    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rC  = (op >> 6)  & 0x1F;
    // Opcode 59 is entirely A-form (single-precision arithmetic); every XO fits in 5 bits and
    // there are no X-form ops here, so mask to 5 bits. A 10-bit mask would fold the frC field
    // into the opcode and make fmuls (and the fmadds family) fall back for any frC != 0.
    int sub = (op >> 1) & 0x1F;

// Round to single precision, store, then update FPSCR[FPRF] — matches the interpreter, which
// runs set_fprf after every single-precision arithmetic op. (op59 uses STORE_SP only for
// arithmetic ops, so baking the FPRF update in here is correct for all of them.)
#define STORE_SP(Dd, ppc_fpr) do { e.FCVT_S_D(Dd, Dd); e.FCVT_D_S(Dd, Dd); emit_store_fpr(e, Dd, ppc_fpr); emit_set_fprf(e, ppc_fpr); } while(0)

    switch (sub) {
    case 20:  // fsubs rD, rA, rB — interpreter: block-extension causes visual corruption
        return false;

    case 21:  // fadds rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FADD_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 25:  // fmuls rD, rA, rC
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        e.FMUL_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 18:  // fdivs rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FDIV_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 22:  // fsqrts rD, rB
        emit_load_fpr(e, D0, rB);
        e.FSQRT_D(D0, D0);
        STORE_SP(D0, rD);
        return true;

    // fmadds/fmsubs/fnmadds/fnmsubs/fres — interpreter (FMA ops, not yet JIT'd)
    case 29: case 28: case 31: case 30: case 24:
        return false;

    default:
        return false;
    }
#undef STORE_SP
}

// ---------------------------------------------------------------------------
// Main compilation function
// ---------------------------------------------------------------------------

JitBlock *JitArm64::compile(uint32_t start_pc)
{
    jit_sync_fetch(start_pc);   // ensure ppc.cur_fetch is valid for ppc_read_opcode_at
    compute_offsets();

    // Check available buffer space (rough estimate: 128 instructions * 32 ARM ops each)
    if (m_code_pos + 128 * 32 * 4 > CODE_BUF_SIZE) {
        // Buffer nearly full: evict all blocks and reuse from start
        flush();
    }

    ScopedJitWrite write_scope;

    uint32_t *write_base = (uint32_t *)(m_write_buf + m_code_pos);
    size_t cap = (CODE_BUF_SIZE - m_code_pos) / 4;
    Arm64Emitter e(write_base, cap);

    // -----------------------------------------------------------------------
    // Prologue — X0 = PPC_REGS* passed by caller (saves 2-4 instructions vs MOV_X64)
    // -----------------------------------------------------------------------
    e.STP_pre(PPC_PTR, 30, A64_SP, -16);   // save X19 (ppc ptr) and X30 (LR)
    e.MOV_X(PPC_PTR, 0);                    // X19 = X0 (PPC_REGS* argument)

    // -----------------------------------------------------------------------
    // Block body
    // -----------------------------------------------------------------------
    uint32_t pc = start_pc;
    int inst_count = 0;
    bool terminated = false;
    uint32_t exit_npc = start_pc;   // fallthrough target

    // Deferred fixup sites collected during compilation of this block.
    // Each entry is {target_ppc_pc, arm_B_instruction_address}.
    std::vector<std::pair<uint32_t, uint32_t*>> pending_fixups;
    auto dep_ep = [&](int ic, uint32_t lpc, uint32_t npc) {
        uint32_t *site = emit_epilogue_deferred(e, ic, lpc, npc);
        pending_fixups.push_back({npc, site});
    };

    while (inst_count < MAX_BLOCK_INSTS && !terminated && !e.full()) {
        uint32_t op = ppc_read_opcode_at(pc);
        if (op == 0) {
            // Opcode 0 = illegal / unreadable; bail
            break;
        }

        int primary = op >> 26;
        bool handled = false;

        // Peephole: cmp/cmpi/cmpli/cmpl immediately followed by bc on the same CR field.
        // emit_cr_from_flags_* uses only CSET/LSL/ORR/LDR/STRB — none touch NZCV — so
        // ARM flags from the CMP are still live after the CR write.  Branch directly from
        // those flags instead of reloading the CR bit (saves 3 instructions per pair).
        {
            bool is_cmp = false, is_signed = false;
            int  cmp_crfD = 0;
            if (primary == 11) {
                is_cmp = true; is_signed = true; cmp_crfD = (op >> 23) & 0x7;
            } else if (primary == 10) {
                is_cmp = true; is_signed = false; cmp_crfD = (op >> 23) & 0x7;
            } else if (primary == 31) {
                int xo = (op >> 1) & 0x3FF;
                if (xo == 0 || xo == 32) {
                    is_cmp = true; is_signed = (xo == 0); cmp_crfD = (op >> 23) & 0x7;
                }
            }
            if (is_cmp) {
                uint32_t next_op = ppc_read_opcode_at(pc + 4);
                int  nbo         = (next_op >> 21) & 0x1F;
                int  nbi         = (next_op >> 16) & 0x1F;
                int  n_crfD      = nbi / 4;
                int  n_crbit     = nbi % 4;
                bool nctr_rel    = !(nbo & 0x04);
                bool ncond_rel   = !(nbo & 0x10);
                bool ncond_clr   = !(nbo & 0x08);
                if ((next_op >> 26) == 16 && !nctr_rel && ncond_rel
                    && !(next_op & 1) && !((next_op >> 1) & 1)
                    && n_crfD == cmp_crfD && n_crbit <= 2)
                {
                    bool ok;
                    if (primary == 11)      ok = translate_cmpi(e, op);
                    else if (primary == 10) ok = translate_cmpli(e, op);
                    else                    ok = translate_op31(e, op);
                    if (ok) {
                        // ARM NZCV still live from the CMP inside translate_cmp*
                        static const int s_cond[3] = { A64_LT, A64_GT, A64_EQ };
                        static const int u_cond[3] = { A64_CC, A64_HI, A64_EQ };
                        int a64_cond = (is_signed ? s_cond : u_cond)[n_crbit];
                        if (ncond_clr) a64_cond ^= 1;

                        int32_t  bd               = (int32_t)((next_op & 0xFFFC) << 16) >> 16;
                        uint32_t bc_pc            = pc + 4;
                        uint32_t taken_target     = bc_pc + (uint32_t)bd;
                        uint32_t not_taken_target = bc_pc + 4;

                        void *taken_fn = nullptr, *not_taken_fn = nullptr;
                        auto  it = m_cache.find(taken_target);
                        if (it != m_cache.end()) taken_fn = (void *)it->second.fn;
                        it = m_cache.find(not_taken_target);
                        if (it != m_cache.end()) not_taken_fn = (void *)it->second.fn;

                        // B.cond_inv skips the taken path; fall through to not-taken
                        uint32_t *nt = e.emit_B_COND_placeholder(a64_cond ^ 1);
                        if (taken_fn) emit_epilogue_chained(e, inst_count + 2, bc_pc, taken_target, taken_fn);
                        else          dep_ep(inst_count + 2, bc_pc, taken_target);
                        e.patch_B_COND(nt, e.ptr());
                        if (not_taken_fn) emit_epilogue_chained(e, inst_count + 2, bc_pc, not_taken_target, not_taken_fn);
                        else              dep_ep(inst_count + 2, bc_pc, not_taken_target);

                        inst_count++;    // bc; loop bottom adds +1 for cmp
                        pc       += 4;   // skip bc; loop bottom advances past cmp
                        handled   = true;
                        terminated = true;
                    }
                }
            }
        }

        // Peephole: lis rD, HI immediately followed by addi rD, rD, LO  or
        //           ori  rD, rD, UIMM — fold into a single MOV_W32 + STR.
        // Saves the LDR+ADD+STR that the second instruction would otherwise cost
        // (typically 3 instructions → eliminated entirely).
        if (!handled && primary == 15) {
            int  rD_lis  = (op >> 21) & 0x1F;
            int  rA_lis  = (op >> 16) & 0x1F;
            if (rA_lis == 0) {
                uint32_t hi_val   = (uint32_t)(int16_t)(op & 0xFFFF) << 16;
                uint32_t next_op  = ppc_read_opcode_at(pc + 4);
                int      next_pri = next_op >> 26;
                int      next_rD  = (next_op >> 21) & 0x1F;
                int      next_rA  = (next_op >> 16) & 0x1F;
                uint32_t full_val = 0;
                bool     fuse     = false;

                if (next_pri == 14 && next_rD == rD_lis && next_rA == rD_lis) {
                    // addi rD, rD, LO: signed add
                    full_val = hi_val + (uint32_t)(int32_t)(int16_t)(next_op & 0xFFFF);
                    fuse = true;
                } else if (next_pri == 24 && next_rD == rD_lis && next_rA == rD_lis) {
                    // ori rD, rD, UIMM: unsigned OR into lower 16 bits
                    full_val = hi_val | (next_op & 0xFFFF);
                    fuse = true;
                }

                if (fuse) {
                    e.MOV_W32(W0, full_val);
                    emit_store_gpr(e, W0, rD_lis);
                    inst_count++;
                    pc      += 4;
                    handled  = true;
                }
            }
        }

        if (!handled) switch (primary) {
        case  3: handled = true; break;  // twi — trap word immediate, NOP in emulator
        case  7: handled = translate_mulli(e, op);      break;
        case  8: handled = translate_subfic(e, op);     break;
        case 10: handled = translate_cmpli(e, op);      break;
        case 11: handled = translate_cmpi(e, op);       break;
        case 12: handled = translate_addic(e, op, false); break;
        case 13: handled = translate_addic(e, op, true);  break;
        case 14: handled = translate_addi(e, op);       break;
        case 15: handled = translate_addis(e, op);      break;
        case 20: handled = translate_rlwimi(e, op);     break;
        case 21: handled = translate_rlwinm(e, op);     break;
        case 23: handled = translate_rlwnm(e, op);      break;
        case 24: handled = translate_ori(e, op);        break;
        case 25: handled = translate_oris(e, op);       break;
        case 26: handled = translate_xori(e, op);       break;
        case 27: handled = translate_xoris(e, op);      break;
        case 28: handled = translate_andi_dot(e, op);   break;
        case 29: handled = translate_andis_dot(e, op);  break;
        case 31: handled = translate_op31(e, op); break;

        // D-form loads
        case 32: case 33: case 34: case 35:
        case 40: case 41: case 42: case 43:
            handled = translate_load_imm(e, op, primary);
            break;

        // D-form stores
        case 36: case 37: case 38: case 39:
        case 44: case 45:
            handled = translate_store_imm(e, op, primary);
            break;

        // Load/store multiple
        case 46: handled = translate_lmw(e, op);  break;
        case 47: handled = translate_stmw(e, op); break;

        // FP D-form loads
        case 48: handled = translate_lfs(e, op, false);  break;  // lfs
        case 49: handled = translate_lfs(e, op, true);   break;  // lfsu
        case 50: handled = translate_lfd(e, op, false);  break;  // lfd
        case 51: handled = translate_lfd(e, op, true);   break;  // lfdu

        // FP D-form stores
        case 52: handled = translate_stfs(e, op, false); break;  // stfs
        case 53: handled = translate_stfs(e, op, true);  break;  // stfsu
        case 54: handled = translate_stfd(e, op, false); break;  // stfd
        case 55: handled = translate_stfd(e, op, true);  break;  // stfdu

        // FP arithmetic
        case 59: handled = translate_op59(e, op); break;
        case 63: handled = translate_op63(e, op); break;

        case 17: {
            // sc — system call: interpreter sets ppc.npc to exception handler
            emit_set_pc_npc(e, pc);
            e.MOV_W32(W0, op);
            emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
            // NPC is now whatever the sc handler set; read it and return
            e.LDR_W(W3, PPC_PTR, OFF_NPC);
            emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
            terminated = true;
            handled    = true;
            break;
        }

        case 16: {
            // bc: fully inlined conditional branch — always terminates the block.
            // Look up taken/not-taken targets for block chaining.
            {
                int32_t bd = (int32_t)((op & 0xFFFC) << 16) >> 16;
                int aa16   = (op >> 1) & 1;
                uint32_t taken_target   = aa16 ? (uint32_t)bd : (pc + (uint32_t)bd);
                uint32_t not_taken_target = pc + 4;
                void *taken_fn     = nullptr;
                void *not_taken_fn = nullptr;
                auto it = m_cache.find(taken_target);
                if (it != m_cache.end()) taken_fn = (void *)it->second.fn;
                it = m_cache.find(not_taken_target);
                if (it != m_cache.end()) not_taken_fn = (void *)it->second.fn;
                handled = translate_bc(e, op, pc, inst_count, pending_fixups, taken_fn, not_taken_fn);
            }
            terminated = true;
            break;
        }

        case 18: {
            // b / bl / ba / bla.  Chain non-linking branches to already-compiled targets.
            uint32_t target = translate_b(e, op, pc);
            int lk18 = op & 1;
            void *target_fn = nullptr;
            if (!lk18) {
                auto it = m_cache.find(target);
                if (it != m_cache.end()) target_fn = (void *)it->second.fn;
            }
            if (target_fn)
                emit_epilogue_chained(e, inst_count + 1, pc, target, target_fn);
            else
                dep_ep(inst_count + 1, pc, target);
            terminated = true;
            handled = true;
            break;
        }

        case 19: {
            // CR logical ops, bclr, bcctr, isync
            int subop = (op >> 1) & 0x3FF;
            int crBD = (op >> 21) & 0x1F;
            int crBA = (op >> 16) & 0x1F;
            int crBB = (op >> 11) & 0x1F;
            switch (subop) {
            case 33:   // crnor   crBD = !(crBA | crBB)
            case 129:  // crandc  crBD = crBA & ~crBB
            case 193:  // crxor   crBD = crBA ^ crBB
            case 225:  // crnand  crBD = !(crBA & crBB)
            case 257:  // crand   crBD = crBA & crBB
            case 289:  // creqv   crBD = !(crBA ^ crBB)
            case 417:  // crorc   crBD = crBA | ~crBB
            case 449: {// cror    crBD = crBA | crBB
                emit_load_cr_bit(e, W0, crBA);
                emit_load_cr_bit(e, W1, crBB);
                // After emit_load_cr_bit, W0/W1 are 0 or 1 (bits [31:1] = 0).
                // EOR_W_FLIP_BIT(Wx, Wx, 0) toggles bit 0 without touching [31:1].
                switch (subop) {
                case  33: e.ORR_W(W0, W0, W1); e.EOR_W_FLIP_BIT(W0, W0, 0); break; // NOR:  !(crBA|crBB)
                case 129: e.EOR_W_FLIP_BIT(W1, W1, 0); e.AND_W(W0, W0, W1); break; // ANDC: crBA & ~crBB
                case 193: e.EOR_W(W0, W0, W1); break;                                // XOR:  crBA ^ crBB
                case 225: e.AND_W(W0, W0, W1); e.EOR_W_FLIP_BIT(W0, W0, 0); break; // NAND: !(crBA&crBB)
                case 257: e.AND_W(W0, W0, W1); break;                                // AND:  crBA & crBB
                case 289: e.EOR_W(W0, W0, W1); e.EOR_W_FLIP_BIT(W0, W0, 0); break; // EQV:  !(crBA^crBB)
                case 417: e.EOR_W_FLIP_BIT(W1, W1, 0); e.ORR_W(W0, W0, W1); break; // ORC:  crBA | ~crBB
                case 449: e.ORR_W(W0, W0, W1); break;                                // OR:   crBA | crBB
                default: break;
                }
                emit_store_cr_bit(e, W0, W2, crBD);
                handled = true;
                break;
            }
            case 150:
                // isync — NOP
                handled = true;
                break;
            case 50: {  // rfi — return from interrupt (sets PC/MSR from SRR0/SRR1)
                emit_set_pc_npc(e, pc);
                e.MOV_W32(W0, op);
                emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
                e.LDR_W(W3, PPC_PTR, OFF_NPC);
                emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
                terminated = true;
                handled    = true;
                break;
            }
            case 0: {   // mcrf crfD, crfS — copy one CR field to another
                int crfD = (op >> 23) & 0x7;
                int crfS = (op >> 18) & 0x7;
                e.LDRB(W0, PPC_PTR, OFF_CR + crfS);
                e.STRB(W0, PPC_PTR, OFF_CR + crfD);
                handled = true;
                break;
            }
            default:
                break;
            }
            if (!handled && (subop == 16 || subop == 528)) {
                // bclr (16) / bcctr (528): branch to LR / CTR, optionally conditional
                int bo = (op >> 21) & 0x1F;
                int bi = (op >> 16) & 0x1F;
                int lk = op & 1;
                bool is_ctr       = (subop == 528);
                bool ctr_relevant = !(bo & 0x04);
                bool cond_relevant= !(bo & 0x10);
                bool ctr_zero     = (bo & 0x02) != 0;
                bool cond_on_clear= !(bo & 0x08);

                // W3 = branch target; load before LK might overwrite LR
                e.LDR_W(W3, PPC_PTR, is_ctr ? OFF_CTR : OFF_LR);

                if (!ctr_relevant && !cond_relevant) {
                    // Unconditional blr/bctr: always branch, no condition
                    if (lk) { e.MOV_W32(W1, pc+4); e.STR_W(W1, PPC_PTR, OFF_LR); }
                    e.AND_W_ALIGN4(W3, W3);

                } else if (ctr_relevant && !cond_relevant) {
                    // CTR-only (bdnzlr / bdzlr / bdnzctr / bdzctr)
                    e.LDR_W(W1, PPC_PTR, OFF_CTR);
                    e.SUBS_W_IMM(W1, W1, 1);          // flags set: Z=1 if CTR becomes 0
                    e.STR_W(W1, PPC_PTR, OFF_CTR);
                    if (is_ctr) e.MOV_W(W3, W1);      // bcctr: target is post-dec CTR
                    if (lk) { e.MOV_W32(W2, pc+4); e.STR_W(W2, PPC_PTR, OFF_LR); }
                    e.AND_W_ALIGN4(W3, W3);
                    e.MOV_W32(W4, pc+4);
                    e.CSEL_W(W3, W3, W4, ctr_zero ? A64_EQ : A64_NE);

                } else if (!ctr_relevant && cond_relevant) {
                    // Cond-only (beqlr / bnelr / etc.): branch directly from CR bit
                    int crfD2  = bi / 4;
                    int crbit2 = bi % 4;
                    int bitpos2 = 3 - crbit2;
                    e.LDRB(W1, PPC_PTR, OFF_CR + crfD2);
                    e.TST_W_BITMASK(W1, (32 - bitpos2) & 31, 0); // Z=1 if bit clear
                    if (lk) { e.MOV_W32(W1, pc+4); e.STR_W(W1, PPC_PTR, OFF_LR); }
                    e.AND_W_ALIGN4(W3, W3);
                    if (lk) {
                        // W1 = pc+4 from LK write; reuse as fallthrough
                        e.CSEL_W(W3, W3, W1, cond_on_clear ? A64_EQ : A64_NE);
                    } else {
                        e.MOV_W32(W4, pc+4);
                        e.CSEL_W(W3, W3, W4, cond_on_clear ? A64_EQ : A64_NE);
                    }

                } else {
                    // Both CTR and COND: CCMP fuses CTR and CR conditions into flags
                    e.LDR_W(W1, PPC_PTR, OFF_CTR);
                    e.SUBS_W_IMM(W1, W1, 1);
                    e.STR_W(W1, PPC_PTR, OFF_CTR);
                    if (is_ctr) e.MOV_W(W3, W1);

                    int crfD2  = bi / 4;
                    int crbit2 = bi % 4;
                    int bitpos2 = 3 - crbit2;
                    e.LDRB(W1, PPC_PTR, OFF_CR + crfD2);
                    e.UBFM_W(W1, W1, bitpos2, bitpos2);
                    int nzcv_fail = cond_on_clear ? 4 : 0;
                    e.CCMP_W_IMM(W1, 1, nzcv_fail, ctr_zero ? A64_EQ : A64_NE);

                    if (lk) { e.MOV_W32(W1, pc+4); e.STR_W(W1, PPC_PTR, OFF_LR); }
                    e.AND_W_ALIGN4(W3, W3);
                    e.MOV_W32(W4, pc+4);
                    e.CSEL_W(W3, W3, W4, cond_on_clear ? A64_NE : A64_EQ);
                }

                emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
                terminated = true;
                handled    = true;
            }
            break;
        }  // case 19

        default:
            break;
        }  // if (!handled) switch

        // Post-switch peephole: rc=1 op writing CR0, immediately followed by a bc
        // Peephole: fuse an Rc=1 instruction that leaves NZCV valid with a following bc
        // on CR0.  NZCV survive all CR-store helpers (CSET/ADD/SUB/LDR/ORR_LSR/STRB set
        // no flags).  The compare+bc case is handled by the look-ahead peephole above.
        if (handled && !terminated) {
            bool has_rc1 = false;
            if (primary == 28 || primary == 29) {
                has_rc1 = true;   // andi./andis.: inherently Rc=1
            } else if (primary == 13) {
                has_rc1 = true;   // addic.: always updates CR0
            } else if ((op & 1) &&
                       (primary == 20 || primary == 21 || primary == 23 || primary == 31)) {
                has_rc1 = true;   // rlwimi./rlwinm./rlwnm./op31 with Rc bit
            }
            if (has_rc1) {
                uint32_t next_op = ppc_read_opcode_at(pc + 4);
                int  nbo         = (next_op >> 21) & 0x1F;
                int  nbi         = (next_op >> 16) & 0x1F;
                int  n_crfD      = nbi / 4;
                int  n_crbit     = nbi % 4;
                bool nctr_rel    = !(nbo & 0x04);
                bool ncond_rel   = !(nbo & 0x10);
                bool ncond_clr   = !(nbo & 0x08);
                // GT condition (n_crbit=1): A64_GT = N^V==0 && Z==0, but PPC GT = N==0 && Z==0.
                // These differ when V=1 (signed overflow), which can occur for ADDS/SUBS/NEGS in
                // primary-31 arithmetic ops and primary-13 (addic.). Skip GT peephole for these.
                bool gt_safe = !(n_crbit == 1 && (primary == 13 || primary == 31));
                if ((next_op >> 26) == 16 && !nctr_rel && ncond_rel
                    && !(next_op & 1) && !((next_op >> 1) & 1)
                    && n_crfD == 0 && n_crbit <= 2 && gt_safe)
                {
                    // CR0[LT] = N (bit31) for all Rc=1 ops (arithmetic ops set N directly;
                    // logical/shift ops use CMP or ANDS where V=0 so A64_MI = A64_LT).
                    // Using A64_LT (N^V) is wrong for neg./add./subf. etc. when V=1 (signed overflow).
                    static const int s_cond[3] = { A64_MI, A64_GT, A64_EQ };
                    int a64_cond = s_cond[n_crbit];
                    if (ncond_clr) a64_cond ^= 1;

                    int32_t  bd               = (int32_t)((next_op & 0xFFFC) << 16) >> 16;
                    uint32_t bc_pc            = pc + 4;
                    uint32_t taken_target     = bc_pc + (uint32_t)bd;
                    uint32_t not_taken_target = bc_pc + 4;

                    void *taken_fn = nullptr, *not_taken_fn = nullptr;
                    auto  it = m_cache.find(taken_target);
                    if (it != m_cache.end()) taken_fn = (void *)it->second.fn;
                    it = m_cache.find(not_taken_target);
                    if (it != m_cache.end()) not_taken_fn = (void *)it->second.fn;

                    uint32_t *nt = e.emit_B_COND_placeholder(a64_cond ^ 1);
                    if (taken_fn) emit_epilogue_chained(e, inst_count + 2, bc_pc, taken_target, taken_fn);
                    else          dep_ep(inst_count + 2, bc_pc, taken_target);
                    e.patch_B_COND(nt, e.ptr());
                    if (not_taken_fn) emit_epilogue_chained(e, inst_count + 2, bc_pc, not_taken_target, not_taken_fn);
                    else              dep_ep(inst_count + 2, bc_pc, not_taken_target);

                    inst_count++;
                    pc       += 4;
                    terminated = true;
                }
            }
        }

        if (!handled) {
            // Fallback: call interpreter for this instruction
            emit_fallback(e, op, pc);
        }

        inst_count++;
        exit_npc = pc + 4;
        pc += 4;
    }

    if (inst_count == 0) return nullptr;    // couldn't compile anything

    // -----------------------------------------------------------------------
    // Epilogue (fall-through case or instruction limit hit)
    // -----------------------------------------------------------------------
    if (!terminated) {
        dep_ep(inst_count, pc - 4, exit_npc);
    }

    // -----------------------------------------------------------------------
    // Flush I-cache
    // -----------------------------------------------------------------------
    void *block_start = write_base;
    void *block_end   = e.ptr();

    if (!m_dual_map) {
        // If RWX, just flush I-cache
        __builtin___clear_cache((char *)block_start, (char *)block_end);
    } else {
        // RW → protect as RX, then flush
        size_t sz = (uint8_t *)block_end - (uint8_t *)block_start;
        sz = (sz + 4095) & ~(size_t)4095;    // round up to page
        mprotect(block_start, sz, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)m_code_buf + m_code_pos,
                                (char *)m_code_buf + m_code_pos + sz);
    }

    m_code_pos += e.size();

    // -----------------------------------------------------------------------
    // Register block in cache
    // -----------------------------------------------------------------------
    JitBlock blk;
    blk.start_pc   = start_pc;
    blk.end_pc     = pc;
    blk.inst_count = inst_count;
    blk.fn         = (void (*)(PPC_REGS *))block_start;

    m_stats.blocks_compiled++;
    m_cache[start_pc] = blk;


    // Mark every 4 KB page covered by this block so smc_write() can skip non-code pages.
    if ((start_pc >> 24) == 0u) {
        uint32_t p0 = start_pc >> 12;
        uint32_t p1 = (blk.end_pc > 0 ? blk.end_pc - 1 : start_pc) >> 12;
        for (uint32_t p = p0; p <= p1; ++p)
            m_code_pages[p & (CODE_PAGE_COUNT - 1)] = 1;
    }

    // Apply retroactive fixups: patch any sites that were waiting for this block
    m_stats.fixups_applied += apply_fixups(m_fixups, start_pc, (uint8_t *)block_start);
    // Register deferred fixups from this block for future backpatching
    for (auto &p : pending_fixups) {
        m_fixups[p.first].push_back(p.second);
        m_stats.fixups_registered++;
    }

    return &m_cache[start_pc];
}

void JitArm64::log_stats() const
{
    uint64_t total = m_stats.block_executions;
    uint64_t fast  = m_stats.fast_hits;
    uint64_t slow  = total > fast ? total - fast : 0;
    size_t code_kb = m_code_pos / 1024;
    JIT_LOG("compiled=%llu execs=%llu (fast=%llu slow=%llu) failures=%llu cache=%zu code=%zuKB",
        (unsigned long long)m_stats.blocks_compiled,
        (unsigned long long)total,
        (unsigned long long)fast,
        (unsigned long long)slow,
        (unsigned long long)m_stats.compile_failures,
        m_cache.size(),
        code_kb);
}

#ifdef ANDROID
#include <algorithm>
#include <android/log.h>
void JitArm64::dump_compiled_pcs(uint32_t lo, uint32_t hi) const
{
    std::vector<uint32_t> pcs;
    pcs.reserve(256);
    for (auto &kv : m_cache)
        if (kv.first >= lo && kv.first < hi)
            pcs.push_back(kv.first);
    std::sort(pcs.begin(), pcs.end());
    __android_log_print(3, "SupermodelDBG",
        "JIT cache: %zu total blocks, %zu in [%08X-%08X]",
        m_cache.size(), pcs.size(), lo, hi);
    for (size_t i = 0; i < pcs.size(); i++) {
        auto it = m_cache.find(pcs[i]);
        __android_log_print(3, "SupermodelDBG",
            "  [%03zu] pc=%08X end=%08X insts=%d",
            i, pcs[i], it->second.end_pc, it->second.inst_count);
    }
}
#endif

#endif // __aarch64__ && HAVE_PPC_JIT
