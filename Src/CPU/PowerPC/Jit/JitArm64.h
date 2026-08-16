#pragma once
#if defined(__aarch64__) && defined(HAVE_PPC_JIT)

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include "../ppc_regs.h"

// Forward declarations from the interpreter (defined in ppc.cpp/ppc603.c)
#ifdef __cplusplus
extern "C" {
#endif
extern void      ppc_dispatch_opcode(UINT32 opcode);
extern void      ppc_check_interrupts_jit(void);
extern PPC_REGS *ppc_get_state(void);
extern UINT32    ppc_read_opcode_at(UINT32 pc);
extern void      jit_sync_fetch(UINT32 pc);     // sync ppc.cur_fetch before compilation
// Memory bridges: all PPC memory accesses from JIT go through these
extern UINT32    jit_read8(UINT32 addr);
extern UINT32    jit_read16(UINT32 addr);
extern UINT32    jit_read32(UINT32 addr);
extern UINT64    jit_read64(UINT32 addr);
extern UINT32    jit_read_tbl(void);
extern UINT32    jit_read_tbu(void);
extern void      jit_write8(UINT32 addr, UINT32 data);
extern void      jit_write16(UINT32 addr, UINT32 data);
extern void      jit_write32(UINT32 addr, UINT32 data);
extern void      jit_write64(UINT32 addr, UINT64 data);
// FP helpers: argument and return value in D0 (AAPCS64 FP convention)
extern double    jit_fres(double x);
extern double    jit_frsqrte(double x);
extern double    jit_frsp(double x);    // round double to single precision and back
extern void      jit_set_fprf(UINT32 rD); // update FPSCR[FPRF] from FPR[rD] after a JIT FP op
#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// JIT block descriptor
// ---------------------------------------------------------------------------
struct JitBlock {
    uint32_t start_pc;
    uint32_t end_pc;          // first PC NOT in this block
    int      inst_count;
    void   (*fn)(PPC_REGS *); // compiled native function; X19 = ppc on entry
};

// ---------------------------------------------------------------------------
// PowerPC → ARM64 JIT
//
// Usage: JitArm64::get().init(); then JitArm64::get().run(cycles);
// JitArm64::get().flush() invalidates all compiled blocks (e.g. on reset).
// ---------------------------------------------------------------------------
class JitArm64
{
public:
    static JitArm64 &get();

    // Initialise the code buffer (mmap). Returns false if JIT is unavailable.
    bool init();
    void shutdown();

    // Invalidate all compiled blocks (must be called on reset / ROM change)
    void flush();

    // SMC: invalidate any cached block whose PC range covers addr.
    void invalidate_containing(uint32_t addr);

    // SMC fast path: page-bitmap check then invalidate_containing.
    // O(1) for non-code pages (the common case); only scans the cache on a code-page hit.
    // Call from every JIT write bridge that targets 0x00000000–0x00FFFFFF.
    void smc_write(uint32_t addr);

    // Find or compile a block for the given PC. Returns nullptr on failure.
    JitBlock *get_or_compile(uint32_t pc);

    bool is_available() const { return m_code_buf != nullptr; }

    // Telemetry
    struct Stats {
        uint64_t blocks_compiled;   // total compile() calls that succeeded
        uint64_t block_executions;  // total get_or_compile() calls that returned a block
        uint64_t fast_hits;         // direct-mapped cache hits (hot path)
        uint64_t compile_failures;  // compile() returned nullptr
        uint64_t fixups_registered; // deferred fixup sites registered for backpatching
        uint64_t fixups_applied;    // fixup sites actually patched when target compiled
    };
    const Stats &get_stats() const { return m_stats; }
    void log_stats() const;
    size_t cache_size() const { return m_cache.size(); }
    size_t code_kb()    const { return m_code_pos / 1024; }

#ifdef ANDROID
    // Dump all compiled PCs in [lo, hi) to Android logcat (tag SupermodelDBG).
    void dump_compiled_pcs(uint32_t lo, uint32_t hi) const;
#endif

    // Function pointer table: reserved at the very start of the code buffer.
    // Function pointer table: 16 entries × 8 bytes = 128 bytes.
    // Stubs: 16 entries × 12 bytes = 192 bytes.  Both at the start of the code
    // buffer and preserved across flush() calls.  JIT blocks start at BLOCK_START.
    static constexpr size_t FN_TABLE_ENTRIES = 16;
    static constexpr size_t FN_TABLE_BYTES   = FN_TABLE_ENTRIES * 8;   // 128 bytes
    static constexpr size_t STUB_BYTES       = FN_TABLE_ENTRIES * 12;  // 192 bytes
    static constexpr size_t BLOCK_START      = FN_TABLE_BYTES + STUB_BYTES;  // 320 bytes

private:
    JitArm64() = default;

    JitBlock *compile(uint32_t pc);

    // Code buffer (executable memory region)
    static constexpr size_t CODE_BUF_SIZE = 16 * 1024 * 1024;  // 16 MB
    uint8_t  *m_code_buf   = nullptr;
    uint8_t  *m_write_buf  = nullptr;   // may differ from m_code_buf on W^X systems
    size_t    m_code_pos   = 0;
    bool      m_init_attempted = false; // avoid retrying a denied allocation every CPU slice

    // Block cache: guest PC → JitBlock
    std::unordered_map<uint32_t, JitBlock> m_cache;

    // Backpatch fixups: guest PC → list of B-instruction addresses in compiled code
    // that should be patched to jump directly to the block once it is compiled.
    std::unordered_map<uint32_t, std::vector<uint32_t*>> m_fixups;

    // Direct-mapped fast lookup: avoids hash map on repeated block entries.
    // Indexed by (pc >> 2) & (FAST_CACHE_MASK). Null means no entry for that slot.
    static constexpr uint32_t FAST_CACHE_SIZE = 4096;
    static constexpr uint32_t FAST_CACHE_MASK = FAST_CACHE_SIZE - 1;
    JitBlock *m_fast_cache[FAST_CACHE_SIZE] = {};

    bool m_dual_map = false;  // true when write ptr != exec ptr

    // Page bitmap for SMC: one byte per 4 KB page in 0x00000000–0x00FFFFFF (16 MB / 4 KB = 4096).
    // Set when a block is compiled that covers the page; cleared only by flush().
    static constexpr uint32_t CODE_PAGE_COUNT = 4096;
    uint8_t m_code_pages[CODE_PAGE_COUNT] = {};

    Stats m_stats = {};
};

#endif // __aarch64__ && HAVE_PPC_JIT
