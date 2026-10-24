# Lean recompiler log

- Setup: proj cloned to ws/lean/proj (branch lean-recompiler); PortableKit cloned to ws/lean/PortableKit (infected-bringup).
- Discovery (leangen/{mips,image,cfg,discover}.py): 9,601 functions, 660,094/661,557 words claimed, 995 orphan words (794 runs),
  173 jr sites all resolved to jump tables (200 switch nodes), 0 dynamic instructions uncovered.
- Analysis phase 1 (leangen/analysis.py): MW fixpoint over the call graph in ~64 s (13.4k function runs).
  Callee-saved registers modified: 5435 functions, caused by a handful of ABI-violating leaves (s0-s3 as temps, f21..f31 odd as temps)
  propagating up to callers that do not save them. This only matters if some caller has them live across the call (demand R).
- Analysis redesign (leangen/analysis.py, live.py): typed/regfile classification now gives 9496 typed / 81 regfile of 9577 functions
  (1.3 % of static words regfile). Key decisions that made it work (each was measured):
  * params are limited to a0..a7/f12..f19/sp; garbage reads of other registers are dropped (read as 0)
  * weak uses (import / jalr / variadic marshalling, return pass-through) keep defs alive but never create demand or hard params;
    a0..a3/f12..f15 weak uses become optional params (Aw) so thin wrappers still forward arguments
  * indirect calls clobber caller-saved registers only (callee-saved are assumed preserved), demand after jalr is ignored
  * setjmp/longjmp: longjmp (0x08A2B794) is declared noreturn (profile); Throw-like callers become noreturn by inference.
    longjmp was never executed in the recorded runs; it will be pk_fatal. setjmp (executed 44x at boot) is a normal function.
  * save slots (callee-saved/ra) are always promoted to locals; other slots only if no callee reads the caller's frame (W analysis)
- Unaligned lwl/lwr (swl/swr) pairs are fused (leangen/fuse.py, 171 pairs): the halves' mutual read of rt was a false
  dependency that made callers of memcpy/memset/strcmp demand r5..r9. Demand after a call is now only counted for
  v0/v1/f0 and callee-saved registers (a read of any other caller-saved register after a call is junk by the ABI and
  infected whole call chains): regfile functions 81 -> 15 (3 callee-saved returners, 12 sp/ra structural).
- First total emission: 9577 functions, 829k C statements, 73 TUs, generated in 27 s; all TUs pass g++ -fsyntax-only.
- Harness (recomp/tests/difftest.cpp): oracle = PortableKit interpreter on its own 32 MiB RAM, lean on the 96 MiB arena; page tracking by
  SIGSEGV/mprotect; compares returned registers, VFPU state, all RAM. Explained classes (each counted): ret-forced (v0/v1/f0 pass-through
  junk), uninitialised stack / out-of-contract probe (second oracle run with other dead-stack and non-parameter register values),
  non-function jalr, unmodelled arena region, NaN-vs-NaN words (operand-order payload/sign), oracle stores into guest code.
  Run 5 on the real image: 57,462 random trials, 33,494 compared, 0 mismatches. Blind spot: 2893 functions (1133 executed, 49.6 % of
  dynamic instructions, incl. 0x08935F54) never complete a random trial (the oracle faults on unmapped pointers).
- Snapshot replay (--snap DIR): the PortableKit interpreter is patched (tests/snapshot/portablekit-snapshot.patch) to fork at the n-th
  entry of listed functions; the child re-executes the call with all of RAM PROT_NONE and logs the pages it touches (read set) with the
  entry registers incl. VFPU state; difftest installs registers + pages on both sides and compares as usual.
  First run: 10,562 snapshots of 4,409 functions, 10,305 compared, 262 mismatches in 133 functions. Cause of all of them (traced with the
  LEAN_TRACE build): thin wrappers that forward undefined incoming a1..a3 (and f12..f15) through a jalr (vtable call) lost them, because
  a jalr only took the registers the function itself defines as weak uses. Fix: jalr weak uses = all weak argument registers
  (profile jalr_weak_all, default true).
- VFPU step 2 (leangen/vfpu.py, lean_vfpu_fast.h): per-function dataflow of the three prefix registers (default at entry and after
  calls; impure call/tail/ret sites are listed in the manifest, none today); fully known state -> lean_vk::form<W,S,T,D>, otherwise the
  generic lean_vf helper. 918 static VFPU instructions in 32 functions are specialised, 10 generic (vmin/vmax, vcrsp, vcmp/vcmov,
  vs2i, vi2s, ...). NEON (LEAN_HAVE_NEON; x86 uses tests/lean_neon_shim.h) for contiguous column quads.
- Focused tests (leangen/synth.py assembler + tests/synth/cases_*.py + tests/run_synth.sh): synthetic images run through the real
  generator and the harness. core: 126 functions (delay-slot corners, branch-likely, calls, tail calls, jump tables idx/val, lwl/lwr,
  ALU, HI/LO, FPU compares/conversions): 24k trials, 0 mismatches. vfpu: 1145 functions per seed (every VFPU form x random registers,
  lengths and prefixes, NEON-shaped variants, prefix-tracking programs), generic and specialised lowering, --special float inputs
  (NaN, inf, denormals, -0, rounding boundaries): 68k compared trials per seed, 0 mismatches on seeds 1-4 (NaN payload words differ
  in ~110 words per seed for the specialised forms: counted, not compared; signalling NaNs excluded from the pool, a compiler may fold
  -0.0f - x into -x).
- Snapshot triage (this window): 58 -> 0 mismatches. Causes, all in the harness/generator, none in the semantics of the ALU/FPU/VFPU lowering:
  (1) harness: the fake HLE wrote out-parameters only into the synthetic arena; in snapshot mode sceKernelSysClock2USec & co. left
  stale dead-stack words that the oracle and lean read differently -> fake_hle_writes now also fills one word at stack out-pointers
  (sceCtrlPeekBufferPositive: 16 bytes, words 1..3 zero), the uninitialised-stack probe randomises the recorded thread's dead frame,
  hash_win/trace use the snapshot pages. (2) generator BUG: a call to a regfile function stored only A|Aw before the call, but the
  callee returns callee-saved registers it does not write on every path (t_0888735C returns r20): the caller read back stale
  g_cpu.gpr[20] -> endless loops (14 lean timeouts) and 3 memory mismatches. Fix: the caller also stores every register in the
  callee's return set that is live afterwards (and at tail calls). (3) a typed function that returns an untouched incoming
  v0/v1/f0 that some DIRECT caller really reads would return its zero local: only 1 such function (0x088DCAC0, never executed);
  it is now classified regfile ("pass-through of r2"). 1056 other functions pass v0/f0 through only because an indirect call forces them.
- Codegen: frame accesses use hb = g_ram + ((entry sp + frame_lo) & MASK) (positive displacements; Thumb-2 cannot encode large negative
  ones), memory accesses share one masked base pointer b<reg> per register along single-predecessor chains, lean_fcmp without the
  explicit unordered test: -6.6 % ARM instructions in the TU of 0x089118B0 (3018 -> 2799 for that function).
- Verified weight: 4,428 executed functions in the recorded runs; snapshot replay compares all but 20 of them (0.003 % of the executed
  instructions); random trials alone reach 62.0 %.
