# Hatari

Atari ST/STE/TT/Falcon emulator. Coding rules are in `doc/coding.txt`: TABs for indentation, doxygen-style function comments, no magic numbers, ASCII only in sources. Keep the original style in imported code (`src/cpu/` is from WinUAE).

## Build and test

- Build dir is `build/` (CMake + ninja, Debug): `cd build && ninja`. After changing `CMakeLists.txt` or `cmake/config-cmake.h`, run `cmake ..` first. The binary is `build/src/hatari`.
- `cd build && ctest`: 98 tests, about 15 s. Run them after debugger changes.
- `src/debug/CMakeLists.txt` has two libraries. `DebugUT` files (`breakcond.c`, `debugcpu.c`, `evaluate.c`, `history.c`, `stacktrace.c`, `symbols.c`, `vars.c`) are also linked into the unit tests in `tests/debugger/`, so they must not call functions from the `Debug` library (e.g. `gdbserver.c`, `debugui.c`) or the test build breaks.
- Optional features use `HAVE_*` defines. Each one is checked in `CMakeLists.txt`, declared in `cmake/config-cmake.h`, and stubbed with macros in the feature's header when it is missing (see `src/includes/control.h`, `src/debug/gdbserver.h`). To check that the fallback compiles, compile the touched files against a copy of `build/config.h` with the define removed, placed first in the include path.
- The same object files go into the libretro core (`src/retro/`). It has its own `gui_event.c`, so hooks added to `src/sdl/gui_event.c` don't run there.

## Debugger structure

- `DebugUI(reason)` in `debugui.c` is the single debugger entry point. Breakpoints and steps call it from `DebugCpu_Check()`, which runs after every CPU instruction while debugging is enabled. Other callers are exceptions (`DebugUI_Exceptions`), the shortcut key, NatFeats and XBIOS.
- `DebugCpu_Check()` only runs after `DebugCpu_SetDebugging()` has enabled it. Call that after changing breakpoints or the step count from outside the console debugger.
- `DebugCpu_Check()` runs before the instruction at PC executes. A `pc=$addr` breakpoint therefore stops with the instruction at addr not yet executed.
- Conditional breakpoints (`breakcond.c`) are the only breakpoint engine. Value-change watches use the same expression on both sides, e.g. `($addr).l ! ($addr).l`. The `:quiet` option suppresses all output.
- Program symbols are loaded for programs started from GEMDOS HD. Loading happens on debugger entry by default (`--symload`). `<prog>.sym` (lowercase extension, `nm` format) overrides the symbol table in the program.
- Program start and exit hooks are in `gemdos.c`. Start is where `Event_DoPrgExecActions()` is called (Pexec mode 0, basepage in D0). Exit is in the `Pterm0`/`Pterm`/`Ptermres` handlers, where the terminating basepage is `STMemory_ReadLong(act_pd)`.
- Memory access for debuggers: `STMemory_Read*/Write*` access the memory banks directly, with no hardware side effects. Writes flush the CPU caches.

## GDB server (`src/debug/gdbserver.c`)

The server implements gdb's remote serial protocol on a TCP port (`--gdbserver [host:]port`, `--gdbserver-wait <bool>`, debugger command `gdbserver`). It is modelled on the Atari-side gdbserver in `../m68k-atari-dev/gdbserver`. User documentation is the "Debugging with gdb" section of `doc/debugger.html`.

- **Threading:** everything runs in the emulation thread, and there are no other threads. While gdb has the emulation stopped, `StopLoop()` blocks inside the `DebugUI` hook or in `GdbServer_Poll()`. `GdbServer_Poll()` is called from `src/sdl/gui_event.c` about once per VBL; it accepts connections and catches Ctrl-C (0x03) while emulation runs.
- **States:**
  - `attached`: gdb has a "process", which is the emulation.
  - `stopReplyPending`: gdb is waiting for a stop reply after c/s/vCont/vRun.
  - `waitForProgram`: a vRun is waiting for a program to start.
  - `startBreak`: a breakpoint at the started program's first instruction.
  - `prog`: the program started for gdb; its exit is reported as `W`.
- **Stops:** on connect, emulation stops and gdb asks `?`. `DebugUI()` calls `GdbServer_DebugUI()` first. DSP reasons, and calls while gdb is not attached, go to the console debugger. Range stepping (`vCont;r`) continues inside the hook without reporting each step.
- **Symbols:** `qOffsets` reports `TextSeg`/`DataSeg`. DataSeg is the text base for m68k-atari-elf programs and the data base for mintelf programs (the PRG header field at offset 18 is "ELF"). When gdb has no file, it fetches `qXfer:exec-file` and then reads an ELF built in memory from Hatari's symbols over `vFile`. That ELF has NOBITS sections, and its addresses are relative to the same offsets.
- **Read/access watchpoints:** `dbgwatch_*` in `src/cpu/memory.h` is a hook on CPU data accesses, called from `memory_get_*`/`memory_put_*` in `memory.c`. Bus accesses that aren't the CPU operand (DMA in `stMemory.c`, 68030 and MegaSTE cache fills, byte reads done as word reads in `custom.c`) are wrapped in `dbgwatch_suspend++/--`, and cache hits report the access with `dbgwatch_access()`. A hit sets the step count to 1, so the stop happens after the accessing instruction.
- **Early connect:** with `--gdbserver-wait`, a gdb that connected before the program started (attached and running, not waiting with vRun) also stops at the program start, but its qOffsets are stale. `StopAtEarlyProgramStart()` sends a `symbol-file -o` hint as an `O` console packet. Detaching to reconnect doesn't work, because detach resumes emulation.
- **Extended mode:** `D` and `k` must keep the connection open (closing it hangs gdb). gdb's `run` sends vKill and then vRun; gdb then continues by itself, so the vRun stop reply is not where the user stops.

### Manual gdb testing

Test programs: build with `~/toolchain/m68k-atari-elf/bin/m68k-atari-elf-gcc -g` and convert with `m68k-atari-elf-prg <elf> <PRG>`. Programs that `Pexec` others need `long _stksize = 65536;`, otherwise they keep all memory and Pexec fails with -39.

Run Hatari headless, with a host directory as drive C:. The last argument autostarts a program.

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/src/hatari --machine ste \
        --tos ~/tos/tos206.img --fast-boot true --sound off --confirm-quit false \
        --harddrive <dir> --gemdos-drive C --conout 2 \
        --gdbserver 1234 --gdbserver-wait on <dir>/HELLO.PRG > log 2>&1 &

- Boot to the program start takes about 8 s. For FPU tests use `--machine tt --tos ~/tos/etos512se.img --fpu 68882`.
- Drive gdb with `gdb-multiarch -batch -x script.gdb`, using `set confirm off` and `set remotetimeout 10`. `set debug remote 1` logs the packets.
- In batch mode, `continue &` followed by `interrupt` never processes the stop. To test Ctrl-C, use `shell (sleep 2; kill -INT $PPID) &` and then a plain `continue`.
- Stop Hatari through its pid, or with `pkill -x hatari`. `pkill -f` patterns can match and kill the calling shell.
