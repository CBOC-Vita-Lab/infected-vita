# Real-state snapshots for the differential harness

Random arguments cannot reach code that dereferences pointers into live game structures (the oracle faults on unmapped
memory), so the harness can replay *recorded* calls: `difftest --snap DIR`.

1. Apply `portablekit-snapshot.patch` to a PortableKit checkout and build the game runtime with it.  The patch adds a
   hook to the interpreter loop; it has no effect unless these environment variables are set:
   * `LEAN_SNAP_DIR`    output directory
   * `LEAN_SNAP_FUNCS`  text file, one function per line: `0xADDRESS occ[,occ...]` (see `mksnaplist.py`)
   * `LEAN_SNAP_IMAGE`  optional, the guest image loaded at 0x08804000; pages identical to it are not stored
   * `LEAN_SNAP_LIMIT`  instruction limit of one captured call (default 20,000,000)
2. Run the game (interpreter mode).  At the n-th entry of a listed function the process forks; the child re-executes that one
   call with all of guest RAM made inaccessible and copies each page the call touches on first touch, then writes
   `DIR/<ENTRY>_<n>.snap` (registers incl. the VFPU state + the touched pages).  Imports reached by the call are answered with
   v0 = v1 = 0; VRAM and the scratchpad are not captured.
3. `difftest --snap DIR [--list funcs.txt] [--snap-limit N]` installs registers and pages on both the interpreter and the
   lean side and compares exactly as for random trials (the callee's dead frame below the entry sp is not compared).
The snapshot files contain game memory: keep them private, never commit them.
