#pragma once

#include "Types.h"

// ---------------------------------------------------------------------------
// Types shared between the interpreter (ppc.cpp) and the JIT (Jit/JitArm64.cpp)
// ---------------------------------------------------------------------------

typedef union {
    UINT64  id;
    double  fd;
} FPR;

typedef union {
    UINT32 i;
    float  f;
} FPR32;

typedef struct {
    UINT32 u;
    UINT32 l;
} BATENT;

typedef struct
{
    UINT32  start;
    UINT32  end;
    UINT32 *ptr;
} PPC_FETCH_REGION;

typedef struct {
    bool    fatalError;     // halt PowerPC until hard reset

    UINT32  r[32];
    UINT32  pc;
    UINT32  npc;

    UINT32 *op;

    UINT32  lr;
    UINT32  ctr;
    UINT32  xer;
    UINT32  msr;
    UINT8   cr[8];
    UINT32  pvr;
    UINT32  srr0;
    UINT32  srr1;
    UINT32  srr2;
    UINT32  srr3;
    UINT32  hid0;
    UINT32  hid1;
    UINT32  hid2;
    UINT32  sdr1;
    UINT32  sprg[4];

    UINT32  dsisr;
    UINT32  dar;
    UINT32  ear;
    UINT32  dmiss;
    UINT32  dcmp;
    UINT32  hash1;
    UINT32  hash2;
    UINT32  imiss;
    UINT32  icmp;
    UINT32  rpa;

    BATENT  ibat[4];
    BATENT  dbat[4];

    UINT32  evpr;
    UINT32  exier;
    UINT32  exisr;
    UINT32  bear;
    UINT32  besr;
    UINT32  iocr;
    UINT32  br[8];
    UINT32  iabr;
    UINT32  esr;
    UINT32  iccr;
    UINT32  dccr;
    UINT32  pit;
    UINT32  pit_counter;
    UINT32  pit_int_enable;
    UINT32  tsr;
    UINT32  dbsr;
    UINT32  sgr;
    UINT32  pid;

    int     reserved;
    UINT32  reserved_address;

    int     interrupt_pending;
    int     external_int;

    UINT64  tb;             // 56-bit timebase

    int   (*irq_callback)(int irqline);

    PPC_FETCH_REGION cur_fetch;
    PPC_FETCH_REGION *fetch;

    // 6xx additions
    UINT32  dec;
    UINT32  fpscr;

    FPR     fpr[32];
    UINT32  sr[16];

    // Timing
    int     timer_ratio;
    UINT32  timer_frac;
    int     tb_base_icount;
    int     dec_base_icount;
    int     dec_trigger_cycle;

    // Cycle counters
    UINT64  total_cycles;
    int     icount;
    int     cur_cycles;
    int     bus_freq_multiplier;
    int     cycles_per_second;

    // Fast CA cache: mirrors XER bit 29 as 0 or 1 for cheaper JIT access
    UINT8   xer_ca;

    // JIT fast RAM pointer: set to the 8MB PowerPC RAM base so the JIT can
    // inline RAM reads/writes without a C++ function call.
    UINT8   *ram_ptr;

#if HAS_PPC603
    int is603;
#endif
#if HAS_PPC602
    int is602;
#endif
} PPC_REGS;
