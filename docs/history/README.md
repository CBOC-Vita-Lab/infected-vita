# Development history

The working logs of the port, kept as they were written: what was tried, what failed and why, with the measurements of the day. They are
the evidence behind [`../STATUS.md`](../STATUS.md), not a description of the finished port, and they were **not edited for accuracy
afterwards**. Where one disagrees with `STATUS.md` or `KNOWN-ISSUES.md`, those win; where a log says "the owner", it means the person who directed the
work and supplied the console. Paths such as `/work/...` are the development machine's.

| File | What it covers |
| --- | --- |
| [`PORTABLEKIT_VITA_ASSESSMENT.md`](PORTABLEKIT_VITA_ASSESSMENT.md) | The first assessment: whether PortableKit could run this game on a Vita, and what that would take |
| [`BRINGUP_LOG.md`](BRINGUP_LOG.md) | Getting the game to boot on a PC under PortableKit's interpreter |
| [`LEAN_LOG.md`](LEAN_LOG.md) | The recompiler: its forms, the differential tests against the interpreter, the VFPU work |
| [`KERNEL_LOG.md`](KERNEL_LOG.md) | The coroutine kernel that runs the game's threads |
| [`RENDER_LOG.md`](RENDER_LOG.md) | The GL renderer: what the PSP's graphics chip does that OpenGL ES does not, and how it was bridged |
| [`../../vita/LOG.md`](../../vita/LOG.md) | The Vita workstream: pinned libraries, the exact library build commands, self-test results |
| [`STATUS-release-candidate.md`](STATUS-release-candidate.md), [`release-candidate/`](release-candidate) | The hand-off documents of the 10 Oct release candidate (a PC and Vita3K build): its status, changes, hardware-test plan and known issues. The first console run came after this, so statements in these documents about what had run on a Vita are out of date |
