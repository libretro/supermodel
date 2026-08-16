#pragma once
#ifdef __aarch64__

#include <cstdint>
#include <cstddef>

// ARM64 special register aliases
static constexpr int A64_WZR = 31;
static constexpr int A64_XZR = 31;
static constexpr int A64_SP  = 31;

// Condition codes
static constexpr int A64_EQ = 0;   // Equal (Z=1)
static constexpr int A64_NE = 1;   // Not equal
static constexpr int A64_CS = 2;   // Carry set (unsigned >=)
static constexpr int A64_CC = 3;   // Carry clear (unsigned <)
static constexpr int A64_MI = 4;   // Minus / negative
static constexpr int A64_PL = 5;   // Plus / positive or zero
static constexpr int A64_VS = 6;   // Overflow set
static constexpr int A64_VC = 7;   // Overflow clear
static constexpr int A64_HI = 8;   // Unsigned higher
static constexpr int A64_LS = 9;   // Unsigned lower or same
static constexpr int A64_GE = 10;  // Signed >=
static constexpr int A64_LT = 11;  // Signed <
static constexpr int A64_GT = 12;  // Signed >
static constexpr int A64_LE = 13;  // Signed <=

// ---------------------------------------------------------------------------
// Minimal ARM64 instruction emitter
// All offsets/immediates are in bytes unless noted otherwise.
// ---------------------------------------------------------------------------
class Arm64Emitter
{
public:
    Arm64Emitter(uint32_t* buf, size_t capacity_words)
        : m_buf(buf), m_pos(0), m_cap(capacity_words) {}

    uint32_t* ptr() const   { return m_buf + m_pos; }
    size_t    size() const  { return m_pos * 4; }
    bool      full() const  { return m_pos >= m_cap; }

    void emit(uint32_t w)
    {
        if (m_pos < m_cap) m_buf[m_pos++] = w;
    }

    // --- Stack pairs (64-bit) ---
    // STP Xt1, Xt2, [Xn, #off]! (pre-index)  off must be multiple of 8, -512..504
    void STP_pre(int Xt1, int Xt2, int Xn, int off)
    {
        int imm7 = (off / 8) & 0x7F;
        emit(0xA9800000 | (imm7 << 15) | (Xt2 << 10) | (Xn << 5) | Xt1);
    }
    // LDP Xt1, Xt2, [Xn], #off (post-index)
    void LDP_post(int Xt1, int Xt2, int Xn, int off)
    {
        int imm7 = (off / 8) & 0x7F;
        emit(0xA8C00000 | (imm7 << 15) | (Xt2 << 10) | (Xn << 5) | Xt1);
    }
    // STP Wt1, Wt2, [Xn, #off] (signed offset)  off must be multiple of 4, -256..252
    void STP_W(int Wt1, int Wt2, int Xn, int off)
    {
        int imm7 = (off / 4) & 0x7F;
        emit(0x29000000 | (imm7 << 15) | (Wt2 << 10) | (Xn << 5) | Wt1);
    }

    // --- Load / Store 32-bit (unsigned scaled offset, off multiple of 4, 0..16380) ---
    void LDR_W(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0xB9400000 | ((byte_off / 4) << 10) | (Xn << 5) | Wt);
    }
    void STR_W(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0xB9000000 | ((byte_off / 4) << 10) | (Xn << 5) | Wt);
    }
    // Unscaled offset, -256..255
    void LDUR_W(int Wt, int Xn, int simm9)
    {
        emit(0xB8400000 | ((simm9 & 0x1FF) << 12) | (Xn << 5) | Wt);
    }
    void STUR_W(int Wt, int Xn, int simm9)
    {
        emit(0xB8000000 | ((simm9 & 0x1FF) << 12) | (Xn << 5) | Wt);
    }
    // Byte load/store (unsigned scaled, off 0..4095)
    void LDRB(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0x39400000 | (byte_off << 10) | (Xn << 5) | Wt);
    }
    void STRB(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0x39000000 | (byte_off << 10) | (Xn << 5) | Wt);
    }
    void LDURB(int Wt, int Xn, int simm9)
    {
        emit(0x38400000 | ((simm9 & 0x1FF) << 12) | (Xn << 5) | Wt);
    }
    void STURB(int Wt, int Xn, int simm9)
    {
        emit(0x38000000 | ((simm9 & 0x1FF) << 12) | (Xn << 5) | Wt);
    }
    // Halfword load/store (unsigned scaled offset, byte_off must be multiple of 2)
    void LDRH(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0x79400000 | ((byte_off / 2) << 10) | (Xn << 5) | Wt);
    }
    void STRH(int Wt, int Xn, uint32_t byte_off)
    {
        emit(0x79000000 | ((byte_off / 2) << 10) | (Xn << 5) | Wt);
    }

    // --- Move immediates ---
    // MOVZ Xd, #imm16 [, LSL #hw*16]  (zeros other bits)
    void MOVZ_X(int Xd, uint16_t imm16, int hw = 0)
    {
        emit(0xD2800000 | (hw << 21) | ((uint32_t)imm16 << 5) | Xd);
    }
    // MOVK Xd, #imm16 [, LSL #hw*16]  (keeps other bits)
    void MOVK_X(int Xd, uint16_t imm16, int hw)
    {
        emit(0xF2800000 | (hw << 21) | ((uint32_t)imm16 << 5) | Xd);
    }
    // MOVZ Wd, #imm16
    void MOVZ_W(int Wd, uint16_t imm16)
    {
        emit(0x52800000 | ((uint32_t)imm16 << 5) | Wd);
    }
    // MOVZ Wd, #imm16, LSL #16  (load upper halfword, zero lower)
    void MOVZ_W_hi(int Wd, uint16_t imm16)
    {
        emit(0x52A00000 | ((uint32_t)imm16 << 5) | Wd);
    }
    // MOVK Wd, #imm16, LSL #16
    void MOVK_W_hi(int Wd, uint16_t imm16)
    {
        emit(0x72A00000 | ((uint32_t)imm16 << 5) | Wd);
    }
    // MOVN Wd, #imm16  (Wd = ~imm16; for negative 32-bit values)
    void MOVN_W(int Wd, uint16_t imm16)
    {
        emit(0x12800000 | ((uint32_t)imm16 << 5) | Wd);
    }

    // Load a 64-bit address/constant into Xd (2-4 instructions)
    void MOV_X64(int Xd, uint64_t v)
    {
        MOVZ_X(Xd, (uint16_t)(v), 0);
        if ((v >> 16) & 0xFFFF) MOVK_X(Xd, (uint16_t)(v >> 16), 1);
        if ((v >> 32) & 0xFFFF) MOVK_X(Xd, (uint16_t)(v >> 32), 2);
        if ((v >> 48) & 0xFFFF) MOVK_X(Xd, (uint16_t)(v >> 48), 3);
    }

    // Load a 32-bit constant into Wd (1-2 instructions)
    void MOV_W32(int Wd, uint32_t v)
    {
        uint16_t lo = (uint16_t)v;
        uint16_t hi = (uint16_t)(v >> 16);
        if (lo == 0) {
            // Upper-half-only: single MOVZ with LSL#16 (covers 0, positive, and negative)
            MOVZ_W_hi(Wd, hi);
        } else if ((int32_t)v < 0) {
            // Negative with non-zero lo: MOVN encodes ~v in upper bits
            if (hi == 0xFFFF) {
                MOVN_W(Wd, (uint16_t)(~lo));   // MOVN Wd, ~lo = (0xFFFF0000 | lo)
            } else {
                MOVZ_W(Wd, lo);
                MOVK_W_hi(Wd, hi);
            }
        } else {
            MOVZ_W(Wd, lo);
            if (hi) MOVK_W_hi(Wd, hi);
        }
    }

    // MOV register (32-bit via ORR Wd, WZR, Wm)
    void MOV_W(int Wd, int Wn)
    {
        emit(0x2A0003E0 | (Wn << 16) | Wd);
    }
    // MOV register (64-bit)
    void MOV_X(int Xd, int Xn)
    {
        emit(0xAA0003E0 | (Xn << 16) | Xd);
    }

    // --- Integer ALU (register, 32-bit) ---
    void ADD_W(int Wd, int Wn, int Wm)   { emit(0x0B000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void ADDS_W(int Wd, int Wn, int Wm)  { emit(0x2B000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void SUB_W(int Wd, int Wn, int Wm)   { emit(0x4B000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void SUBS_W(int Wd, int Wn, int Wm)  { emit(0x6B000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void AND_W(int Wd, int Wn, int Wm)   { emit(0x0A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void ANDS_W(int Wd, int Wn, int Wm)  { emit(0x6A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    // AND/ANDS with bitmask immediate (N=0, 32-bit element): encodes any single contiguous run of 1-bits.
    // immr = (32 - run_start) & 31, imms = run_width - 1.  Cannot encode all-zeros or all-ones.
    void AND_W_BITMASK(int Wd, int Wn, int immr, int imms)
    {
        emit(0x12000000 | ((immr & 0x3F) << 16) | ((imms & 0x3F) << 10) | (Wn << 5) | Wd);
    }
    void ANDS_W_BITMASK(int Wd, int Wn, int immr, int imms)
    {
        emit(0x72000000 | ((immr & 0x3F) << 16) | ((imms & 0x3F) << 10) | (Wn << 5) | Wd);
    }
    void TST_W_BITMASK(int Wn, int immr, int imms) { ANDS_W_BITMASK(A64_WZR, Wn, immr, imms); }
    void ORR_W_BITMASK(int Wd, int Wn, int immr, int imms)
    {
        emit(0x32000000 | ((immr & 0x3F) << 16) | ((imms & 0x3F) << 10) | (Wn << 5) | Wd);
    }
    void EOR_W_BITMASK(int Wd, int Wn, int immr, int imms)
    {
        emit(0x52000000 | ((immr & 0x3F) << 16) | ((imms & 0x3F) << 10) | (Wn << 5) | Wd);
    }
    void ORR_W(int Wd, int Wn, int Wm)   { emit(0x2A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void ORN_W(int Wd, int Wn, int Wm)   { emit(0x2A200000 | (Wm << 16) | (Wn << 5) | Wd); }
    void EOR_W(int Wd, int Wn, int Wm)   { emit(0x4A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void BIC_W(int Wd, int Wn, int Wm)   { emit(0x0A200000 | (Wm << 16) | (Wn << 5) | Wd); }
    void BICS_W(int Wd, int Wn, int Wm)  { emit(0x6A200000 | (Wm << 16) | (Wn << 5) | Wd); }
    void MVN_W(int Wd, int Wm)            { ORN_W(Wd, A64_WZR, Wm); }
    void NEG_W(int Wd, int Wm)            { SUB_W(Wd, A64_WZR, Wm); }
    void MUL_W(int Wd, int Wn, int Wm)   { emit(0x1B007C00 | (Wm << 16) | (Wn << 5) | Wd); }
    void SDIV_W(int Wd, int Wn, int Wm)  { emit(0x1AC00C00 | (Wm << 16) | (Wn << 5) | Wd); }
    void UDIV_W(int Wd, int Wn, int Wm)  { emit(0x1AC00800 | (Wm << 16) | (Wn << 5) | Wd); }
    // Multiply-high signed (returns upper 32 bits of 64-bit product)
    void SMULH_W(int Wd, int Wn, int Wm) { emit(0x1B400000 | (Wm << 16) | (A64_WZR << 10) | (Wn << 5) | Wd); }
    // Shift by register
    void LSL_W(int Wd, int Wn, int Wm)   { emit(0x1AC02000 | (Wm << 16) | (Wn << 5) | Wd); }
    void LSR_W(int Wd, int Wn, int Wm)   { emit(0x1AC02400 | (Wm << 16) | (Wn << 5) | Wd); }
    void ASR_W(int Wd, int Wn, int Wm)   { emit(0x1AC02800 | (Wm << 16) | (Wn << 5) | Wd); }
    void ROR_W(int Wd, int Wn, int Wm)   { emit(0x1AC02C00 | (Wm << 16) | (Wn << 5) | Wd); }
    // CLZ (count leading zeros)
    void CLZ_W(int Wd, int Wn)            { emit(0x5AC01000 | (Wn << 5) | Wd); }
    // Byte-reverse
    void REV_W(int Wd, int Wn)            { emit(0x5AC00800 | (Wn << 5) | Wd); }  // reverse 4 bytes
    void REV16_W(int Wd, int Wn)          { emit(0x5AC00400 | (Wn << 5) | Wd); }  // reverse bytes within each 16-bit half
    // Negate with flags
    void NEGS_W(int Wd, int Wm)           { SUBS_W(Wd, A64_WZR, Wm); }
    // Add/subtract with carry (reads ARM C flag as carry-in)
    void ADCS_W(int Wd, int Wn, int Wm)  { emit(0x3A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void SBC_W(int Wd, int Wn, int Wm)   { emit(0x5A000000 | (Wm << 16) | (Wn << 5) | Wd); }
    void SBCS_W(int Wd, int Wn, int Wm)  { emit(0x7A000000 | (Wm << 16) | (Wn << 5) | Wd); }

    // ADD with shifted register: ADD Wd, Wn, Wm, LSL #imm  (Wd = Wn + (Wm << imm))
    void ADD_W_LSL(int Wd, int Wn, int Wm, int imm)
    {
        emit(0x0B000000 | (Wm << 16) | (imm << 10) | (Wn << 5) | Wd);
    }
    // ORR with shifted register: ORR Wd, Wn, Wm, LSL #imm
    void ORR_W_LSL(int Wd, int Wn, int Wm, int imm)
    {
        emit(0x2A000000 | (Wm << 16) | (imm << 10) | (Wn << 5) | Wd);
    }
    void ORR_W_LSR(int Wd, int Wn, int Wm, int imm)
    {
        emit(0x2A400000 | (Wm << 16) | (imm << 10) | (Wn << 5) | Wd);
    }
    // SUB with shifted register: SUB Wd, Wn, Wm, LSL #imm  (Wd = Wn - (Wm << imm))
    void SUB_W_LSL(int Wd, int Wn, int Wm, int imm)
    {
        emit(0x4B000000 | (Wm << 16) | (imm << 10) | (Wn << 5) | Wd);
    }
    // AND with ASR-shifted register: AND Wd, Wn, Wm, ASR #imm  (Wd = Wn & (Wm >> imm) signed)
    void AND_W_ASR(int Wd, int Wn, int Wm, int imm)
    {
        emit(0x0A800000 | (Wm << 16) | (imm << 10) | (Wn << 5) | Wd);
    }

    // --- Integer ALU (immediate, 32-bit) ---
    // imm12 must fit in 12 bits (0..4095); sh=1 shifts imm left by 12
    void ADD_W_IMM(int Wd, int Wn, uint32_t imm12, int sh = 0)
    {
        emit(0x11000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (Wn << 5) | Wd);
    }
    void ADDS_W_IMM(int Wd, int Wn, uint32_t imm12, int sh = 0)
    {
        emit(0x31000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (Wn << 5) | Wd);
    }
    void SUB_W_IMM(int Wd, int Wn, uint32_t imm12, int sh = 0)
    {
        emit(0x51000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (Wn << 5) | Wd);
    }
    void SUBS_W_IMM(int Wd, int Wn, uint32_t imm12, int sh = 0)
    {
        emit(0x71000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (Wn << 5) | Wd);
    }
    void CMP_W_IMM(int Wn, uint32_t imm12, int sh = 0) { SUBS_W_IMM(A64_WZR, Wn, imm12, sh); }
    void CMN_W_IMM(int Wn, uint32_t imm12, int sh = 0) { ADDS_W_IMM(A64_WZR, Wn, imm12, sh); }
    void CMP_W(int Wn, int Wm)             { SUBS_W(A64_WZR, Wn, Wm); }
    void CMN_W(int Wn, int Wm)             { ADDS_W(A64_WZR, Wn, Wm); }
    void TST_W(int Wn, int Wm)             { ANDS_W(A64_WZR, Wn, Wm); }
    // Conditional compare (immediate): if cond, set NZCV from Wn-imm5; else NZCV = nzcv_const
    void CCMP_W_IMM(int Wn, int imm5, int nzcv_const, int cond)
    {
        emit(0x7A400800 | ((imm5 & 0x1F) << 16) | ((cond & 0xF) << 12) | (Wn << 5) | (nzcv_const & 0xF));
    }
    // ORR with a single-bit logical immediate: Wd = Wn | (1 << bitpos)
    void ORR_W_SET_BIT(int Wd, int Wn, int bitpos)
    {
        int immr = (32 - bitpos) & 0x1F;
        emit(0x32000000 | (immr << 16) | (Wn << 5) | Wd);
    }
    // EOR Wd, Wn, #(1<<bitpos)  — flip a single bit
    void EOR_W_FLIP_BIT(int Wd, int Wn, int bitpos)
    {
        int immr = (32 - bitpos) & 0x1F;
        emit(0x52000000 | (immr << 16) | (Wn << 5) | Wd);
    }

    // --- Bitfield (32-bit) ---
    // UBFM Wd, Wn, #immr, #imms  (unsigned bitfield move)
    void UBFM_W(int Wd, int Wn, int immr, int imms)
    {
        emit(0x53000000 | ((immr & 0x1F) << 16) | ((imms & 0x1F) << 10) | (Wn << 5) | Wd);
    }
    // SBFM Wd, Wn, #immr, #imms  (signed bitfield move)
    void SBFM_W(int Wd, int Wn, int immr, int imms)
    {
        emit(0x13000000 | ((immr & 0x1F) << 16) | ((imms & 0x1F) << 10) | (Wn << 5) | Wd);
    }
    // LSL/LSR/ASR by immediate
    void LSL_W_IMM(int Wd, int Wn, int imm) { UBFM_W(Wd, Wn, (32 - imm) & 31, 31 - imm); }
    void LSR_W_IMM(int Wd, int Wn, int imm) { UBFM_W(Wd, Wn, imm, 31); }
    void ASR_W_IMM(int Wd, int Wn, int imm) { SBFM_W(Wd, Wn, imm, 31); }
    // ROR by immediate via EXTR Wd, Wn, Wn, #imm
    void ROR_W_IMM(int Wd, int Wn, int imm)
    {
        emit(0x13800000 | (Wn << 16) | ((imm & 0x1F) << 10) | (Wn << 5) | Wd);
    }
    // Sign/zero extend
    void SXTB_W(int Wd, int Wn) { SBFM_W(Wd, Wn, 0, 7); }
    void SXTH_W(int Wd, int Wn) { SBFM_W(Wd, Wn, 0, 15); }
    void UXTB_W(int Wd, int Wn) { UBFM_W(Wd, Wn, 0, 7); }
    void UXTH_W(int Wd, int Wn) { UBFM_W(Wd, Wn, 0, 15); }
    // BFM Wd, Wn, #immr, #imms  (bitfield move — insert)
    void BFM_W(int Wd, int Wn, int immr, int imms)
    {
        emit(0x33000000 | ((immr & 0x1F) << 16) | ((imms & 0x1F) << 10) | (Wn << 5) | Wd);
    }
    // BFI Wd, Wn, #lsb, #width — insert Wn[0..width-1] into Wd[lsb..lsb+width-1]
    void BFI_W(int Wd, int Wn, int lsb, int width) { BFM_W(Wd, Wn, (32 - lsb) & 31, width - 1); }
    // AND Wd, Wn, #~3 — clear low 2 bits (4-byte align): AND Wd, Wn, #0xFFFFFFFC
    void AND_W_ALIGN4(int Wd, int Wn) { emit(0x121E7400 | (Wn << 5) | Wd); }

    // ADD Xd, Xn, Wm, UXTW  (zero-extend Wm to 64-bit, add to Xn — used for dynamic struct offsets)
    // Encoding: sf=1, op=0, S=0, extended-reg form (bit21=1), option=010 (UXTW), shift=0
    void ADD_X_UXTW(int Xd, int Xn, int Wm)
    {
        emit(0x8B204000 | ((Wm & 0x1F) << 16) | ((Xn & 0x1F) << 5) | (Xd & 0x1F));
    }

    // --- 64-bit ops ---
    void SUB_X_IMM(int Xd, int Xn, uint32_t imm12)
    {
        emit(0xD1000000 | ((imm12 & 0xFFF) << 10) | (Xn << 5) | Xd);
    }
    void ADD_X_IMM(int Xd, int Xn, uint32_t imm12)
    {
        emit(0x91000000 | ((imm12 & 0xFFF) << 10) | (Xn << 5) | Xd);
    }
    void LDR_X(int Xt, int Xn, uint32_t byte_off)
    {
        emit(0xF9400000 | ((byte_off / 8) << 10) | (Xn << 5) | Xt);
    }
    void STR_X(int Xt, int Xn, uint32_t byte_off)
    {
        emit(0xF9000000 | ((byte_off / 8) << 10) | (Xn << 5) | Xt);
    }
    // LSR Xd, Xn, #imm  (logical shift right immediate, 64-bit)
    void LSR_X_IMM(int Xd, int Xn, int imm)
    {
        emit(0xD3400000 | ((imm & 0x3F) << 16) | (63 << 10) | (Xn << 5) | Xd);
    }
    // SMULL Xd, Wn, Wm  (signed multiply long: Xd = (int64)Wn * (int64)Wm)
    void SMULL_X(int Xd, int Wn, int Wm) { emit(0x9B207C00 | (Wm << 16) | (Wn << 5) | Xd); }
    // UMULL Xd, Wn, Wm  (unsigned multiply long: Xd = (uint64)Wn * (uint64)Wm)
    void UMULL_X(int Xd, int Wn, int Wm) { emit(0x9BA07C00 | (Wm << 16) | (Wn << 5) | Xd); }

    // --- Conditional selects ---
    // CSET Wd, cond  (= CSINC Wd, WZR, WZR, !cond)
    void CSET_W(int Wd, int cond)
    {
        int inv = (cond ^ 1) & 0xF;
        emit(0x1A9F07E0 | (inv << 12) | Wd);
    }
    // CSEL Wd, Wn, Wm, cond  (Wd = cond ? Wn : Wm)
    void CSEL_W(int Wd, int Wn, int Wm, int cond)
    {
        emit(0x1A800000 | (Wm << 16) | (cond << 12) | (Wn << 5) | Wd);
    }

    // --- Branches ---
    void B(int off_bytes)
    {
        emit(0x14000000 | ((off_bytes / 4) & 0x3FFFFFF));
    }
    void BL(int64_t off_bytes)
    {
        emit(0x94000000 | ((int32_t)(off_bytes / 4) & 0x3FFFFFF));
    }
    void BLR(int Xn)  { emit(0xD63F0000 | (Xn << 5)); }
    void BR(int Xn)   { emit(0xD61F0000 | (Xn << 5)); }
    void RET()        { emit(0xD65F03C0); }
    void NOP()        { emit(0xD503201F); }

    // B.cond #off (off in bytes, ±1MB range)
    void B_COND(int cond, int off_bytes)
    {
        emit(0x54000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | cond);
    }
    void CBZ_W(int Wn, int off_bytes)
    {
        emit(0x34000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | Wn);
    }
    void CBNZ_W(int Wn, int off_bytes)
    {
        emit(0x35000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | Wn);
    }
    // TBZ/TBNZ: test bit b (0-31) and branch if zero/non-zero (±32 KB range)
    void TBZ_W(int Wt, int b, int off_bytes)
    {
        emit(0x36000000 | ((b & 0x1F) << 19) | (((off_bytes / 4) & 0x3FFF) << 5) | Wt);
    }
    void TBNZ_W(int Wt, int b, int off_bytes)
    {
        emit(0x37000000 | ((b & 0x1F) << 19) | (((off_bytes / 4) & 0x3FFF) << 5) | Wt);
    }

    // Emit a branch with 0 offset (to be patched later), return address of instruction
    uint32_t* emit_B_placeholder()
    {
        uint32_t* p = ptr();
        B(0);
        return p;
    }
    uint32_t* emit_B_COND_placeholder(int cond)
    {
        uint32_t* p = ptr();
        B_COND(cond, 0);
        return p;
    }
    uint32_t* emit_CBZ_W_placeholder(int Wn)
    {
        uint32_t* p = ptr();
        CBZ_W(Wn, 0);
        return p;
    }
    uint32_t* emit_CBNZ_W_placeholder(int Wn)
    {
        uint32_t* p = ptr();
        CBNZ_W(Wn, 0);
        return p;
    }
    uint32_t* emit_TBZ_W_placeholder(int Wt, int b)
    {
        uint32_t* p = ptr();
        TBZ_W(Wt, b, 0);
        return p;
    }
    uint32_t* emit_TBNZ_W_placeholder(int Wt, int b)
    {
        uint32_t* p = ptr();
        TBNZ_W(Wt, b, 0);
        return p;
    }

    void patch_B(uint32_t* at, uint32_t* target)
    {
        int off = (int)((target - at) * 4);
        *at = 0x14000000 | ((off / 4) & 0x3FFFFFF);
    }
    void patch_B_COND(uint32_t* at, uint32_t* target)
    {
        int off = (int)((target - at) * 4);
        *at = (*at & 0xFF00001F) | (((off / 4) & 0x7FFFF) << 5);
    }
    void patch_CBZ(uint32_t* at, uint32_t* target)
    {
        int off = (int)((target - at) * 4);
        *at = (*at & 0xFF00001F) | (((off / 4) & 0x7FFFF) << 5);
    }
    // patch_TBZ: update imm14 field in bits [18:5]; preserves b5, opcode, b[4:0], Rt
    void patch_TBZ(uint32_t* at, uint32_t* target)
    {
        int off = (int)((target - at) * 4);
        *at = (*at & 0xFFF8001F) | (((off / 4) & 0x3FFF) << 5);
    }

    // --- Floating-point load/store (scaled unsigned offset) ---
    // LDR Dd, [Xn, #byte_off]  byte_off multiple of 8, 0..32760
    void LDR_D(int Dd, int Xn, uint32_t byte_off)
    {
        emit(0xFD400000 | ((byte_off / 8) << 10) | (Xn << 5) | Dd);
    }
    void STR_D(int Dd, int Xn, uint32_t byte_off)
    {
        emit(0xFD000000 | ((byte_off / 8) << 10) | (Xn << 5) | Dd);
    }
    // LDR Sd, [Xn, #byte_off]  byte_off multiple of 4, 0..16380
    void LDR_S(int Sd, int Xn, uint32_t byte_off)
    {
        emit(0xBD400000 | ((byte_off / 4) << 10) | (Xn << 5) | Sd);
    }
    void STR_S(int Sd, int Xn, uint32_t byte_off)
    {
        emit(0xBD000000 | ((byte_off / 4) << 10) | (Xn << 5) | Sd);
    }

    // --- FP ↔ integer register moves ---
    void FMOV_S_W(int Sd, int Wn) { emit(0x1E270000 | (Wn << 5) | Sd); }  // S = W (reinterpret bits)
    void FMOV_W_S(int Wt, int Sn) { emit(0x1E260000 | (Sn << 5) | Wt); }  // W = S (reinterpret bits)

    // --- FP conversion ---
    void FCVT_D_S(int Dd, int Sn) { emit(0x1E22C000 | (Sn << 5) | Dd); }  // D = (double)S
    void FCVT_S_D(int Sd, int Dn) { emit(0x1E624000 | (Dn << 5) | Sd); }  // S = (float)D
    void FCVTZS_W_D(int Wt, int Dn) { emit(0x1E780000 | (Dn << 5) | Wt); } // W = (int32)D (truncate)
    void FCVTZS_X_D(int Xt, int Dn) { emit(0x9E780000 | (Dn << 5) | Xt); } // X = sign_extend((int32)D)

    // --- FP arithmetic (double precision) ---
    void FADD_D(int Dd, int Dn, int Dm)  { emit(0x1E602800 | (Dm<<16) | (Dn<<5) | Dd); }
    void FSUB_D(int Dd, int Dn, int Dm)  { emit(0x1E603800 | (Dm<<16) | (Dn<<5) | Dd); }
    void FMUL_D(int Dd, int Dn, int Dm)  { emit(0x1E600800 | (Dm<<16) | (Dn<<5) | Dd); }
    void FDIV_D(int Dd, int Dn, int Dm)  { emit(0x1E601800 | (Dm<<16) | (Dn<<5) | Dd); }
    void FSQRT_D(int Dd, int Dn)         { emit(0x1E61C000 | (Dn<<5) | Dd); }
    void FMOV_D(int Dd, int Dn)          { emit(0x1E604000 | (Dn<<5) | Dd); }
    void FNEG_D(int Dd, int Dn)          { emit(0x1E614000 | (Dn<<5) | Dd); }
    void FABS_D(int Dd, int Dn)          { emit(0x1E60C000 | (Dn<<5) | Dd); }
    void FCMP_D(int Dn, int Dm)          { emit(0x1E602000 | (Dm<<16) | (Dn<<5)); }
    void FCMP_D_ZERO(int Dn)             { emit(0x1E602008 | (Dn<<5)); }               // FCMP Dn, #0.0
    void FCSEL_D(int Dd, int Dn, int Dm, int cond) { emit(0x1E600C00 | (Dm<<16) | (cond<<12) | (Dn<<5) | Dd); }
    // Fused multiply-accumulate family (double precision): Dd = Da ± Dn*Dm
    void FMADD_D(int Dd, int Dn, int Dm, int Da)  { emit(0x1F400000 | (Dm<<16) | (Da<<10) | (Dn<<5) | Dd); } // Da + Dn*Dm
    void FMSUB_D(int Dd, int Dn, int Dm, int Da)  { emit(0x1F408000 | (Dm<<16) | (Da<<10) | (Dn<<5) | Dd); } // Da - Dn*Dm
    void FNMADD_D(int Dd, int Dn, int Dm, int Da) { emit(0x1F600000 | (Dm<<16) | (Da<<10) | (Dn<<5) | Dd); } // -(Da + Dn*Dm)
    void FNMSUB_D(int Dd, int Dn, int Dm, int Da) { emit(0x1F608000 | (Dm<<16) | (Da<<10) | (Dn<<5) | Dd); } // -(Da - Dn*Dm)

    // FMOV between 64-bit integer and double FP registers (bit-cast)
    void FMOV_D_X(int Dd, int Xn) { emit(0x9E670000 | (Xn<<5) | Dd); }  // Dd = Xn (reinterpret bits)
    void FMOV_X_D(int Xd, int Dn) { emit(0x9E660000 | (Dn<<5) | Xd); }  // Xd = Dn (reinterpret bits)

    // ADRP Xd, #page_off_bytes  (page_off_bytes must be page-aligned, ±4 GB)
    // Sets Xd = (PC & ~0xFFF) + page_off_bytes
    void ADRP(int Xd, int64_t page_off_bytes)
    {
        int64_t imm21 = page_off_bytes / 4096;
        uint32_t immlo = (uint32_t)(imm21 & 3) << 29;
        uint32_t immhi = (uint32_t)((imm21 >> 2) & 0x7FFFF) << 5;
        emit(0x90000000 | immlo | immhi | (uint32_t)Xd);
    }

    // DSB + ISB (data/instruction sync) for I-cache invalidation
    void DSB_ISH() { emit(0xD5033BBF); }
    void ISB()     { emit(0xD5033FDF); }

private:
    uint32_t *m_buf;
    size_t    m_pos;
    size_t    m_cap;
};

#endif // __aarch64__
