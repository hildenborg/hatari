# Hatari GDB server

Hatari includes a server for the gdb remote serial protocol, so that gdb
can debug programs running in the emulated machine, with source level
debugging for programs that have debug information.

This document describes how the server works internally: how gdb
connects to it, what it supports, and how symbols are looked up. Usage
instructions for end users are in the "Debugging with gdb" section of
[debugger.html](debugger.html#gdb). The implementation is in
`src/debug/gdbserver.c`.

## Contents

1. [Overview](#1-overview)
2. [Connecting gdb](#2-connecting-gdb)
3. [Run control and stops](#3-run-control-and-stops)
4. [Programs as the gdb "process"](#4-programs-as-the-gdb-process)
5. [Breakpoints and watchpoints](#5-breakpoints-and-watchpoints)
6. [Registers and memory](#6-registers-and-memory)
7. [Monitor commands](#7-monitor-commands)
8. [Symbol lookup](#8-symbol-lookup)
9. [Supported packets](#9-supported-packets)
10. [Limitations](#10-limitations)
11. [Testing](#11-testing)


## 1. Overview

The server is a front end to the Hatari debugger, not a separate
debugger. gdb requests are mapped to existing debugger features:

- gdb breakpoints and write watchpoints become Hatari conditional
  breakpoints (`breakcond.c`) with the `:quiet` option, so they also
  show up in the Hatari debugger's breakpoint list
- stepping uses the debugger step count (`DebugCpu_SetStepCount()`)
- read and access watchpoints use a hook on CPU data accesses in the
  CPU core (see [section 5](#5-breakpoints-and-watchpoints))
- `monitor` commands run Hatari debugger commands
- symbols come from gdb's own ELF file, or from the Hatari debugger's
  symbols for the program (see [section 8](#8-symbol-lookup))

### Threading

Everything runs in the emulation thread; the server has no threads of
its own. While gdb has the emulation stopped, the server's `StopLoop()`
blocks inside the debugger hook, the same way as the console debugger
blocks while it waits for commands. While emulation runs,
`GdbServer_Poll()` is called from the SDL event loop
(`src/sdl/gui_event.c`) about once per VBL. It accepts new connections
and catches gdb's interrupt request (Ctrl-C, byte 0x03).

### Platforms

The server works on Unix systems (Linux, macOS) and on Windows
(Winsock). The socket API differences are hidden behind a few macros
(`sock_t`, `SockClose()`, `SockPoll()`, `SockError()` etc.) at the top
of `gdbserver.c`. When the needed socket headers are missing, CMake
leaves `HAVE_GDBSERVER` undefined and `gdbserver.h` replaces the server
functions with stubs.

The libretro core uses its own event loop instead of `gui_event.c`, so
`GdbServer_Poll()` is not called there and the server is not usable in
the core.


## 2. Connecting gdb

### Starting the server

The server is started with command line options:

| Option                     | Purpose                              |
|----------------------------|--------------------------------------|
| `--gdbserver [host:]port`  | listen on given TCP port (0 = off)   |
| `--gdbserver-wait <bool>`  | stop programs at their start for gdb |

or with the `gdbserver` debugger command:

| Command                  | Purpose                       |
|--------------------------|-------------------------------|
| `gdbserver [host:]port`  | start listening               |
| `gdbserver off`          | stop the server               |
| `gdbserver wait on\|off` | same as `--gdbserver-wait`    |
| `gdbserver`              | show server status            |

Without a host, the server listens only on localhost. `host:port`
listens on the given address, and `:port` on all addresses (other
machines can then connect, so use it only on trusted networks).

### Connecting

gdb is any gdb with m68k support, e.g. `gdb-multiarch` or an
m68k-atari-elf / m68k-atari-mint cross gdb:

```
$ gdb-multiarch hello.elf
(gdb) target extended-remote localhost:1234
```

`target remote` works too, but `extended-remote` mode is needed for gdb
`run` (see [section 4](#4-programs-as-the-gdb-process)).

What happens when gdb connects:

1. `GdbServer_Poll()` accepts the connection, marks gdb as attached and
   stops emulation (`StopNow()`). Hatari can't stop in the middle of an
   instruction, so the stop happens between instructions.
2. gdb negotiates features with `qSupported`. The server offers no-ack
   mode, swbreak / hwbreak stop reasons, the target description, the
   exec-file query and binary memory upload. gdb then switches to
   no-ack mode (`QStartNoAckMode`), so packets are not acknowledged
   separately (TCP is reliable anyway).
3. gdb reads the target description (`qXfer:features:read`), which tells
   it the register set (see [section 6](#6-registers-and-memory)), and
   asks for the stop reason (`?`). The reply is a `T` stop packet with
   the current fp, sp, sr and pc register values included.
4. gdb asks for the section offsets (`qOffsets`) and, when it has no
   file, for the executable file (see [section 8](#8-symbol-lookup)).

There is a single thread ("thread 1") and a single process, the
emulated machine, so thread queries get fixed replies.

### Disconnecting

When gdb disconnects (or the connection is lost), the server removes
gdb's breakpoints, restores the exception settings it changed and
resumes emulation. In extended mode, gdb `detach` and `kill` keep the
connection open (closing it would hang gdb), only the debugged
"process" goes away and emulation continues.


## 3. Run control and stops

gdb resumes emulation with `c` / `s` or their `vCont` equivalents. The
server sets the debugger step count (1 for a single step, 0 for
continue), leaves `StopLoop()` and lets emulation run. gdb then waits
for a stop reply.

Range stepping (`vCont;r`, used by gdb `next` and `step`) continues
stepping while PC is within the given address range, inside the
debugger hook, without reporting each step to gdb. Every 4096 steps it
checks the connection for an interrupt request.

### Stop reasons

Stops come through the debugger entry point `DebugUI()`, which calls
`GdbServer_DebugUI()` first. When gdb is attached, the server handles
the stop and reports it to gdb:

| Reason                     | gdb sees                           |
|----------------------------|------------------------------------|
| step finished              | SIGTRAP                            |
| gdb breakpoint             | SIGTRAP, `swbreak` or `hwbreak`    |
| write watchpoint           | SIGTRAP, `watch:<addr>`            |
| read watchpoint            | SIGTRAP, `rwatch:<addr>`           |
| access watchpoint          | SIGTRAP, `awatch:<addr>`           |
| Hatari debugger breakpoint | SIGTRAP                            |
| CPU exception              | signal for the exception (below)   |
| Ctrl-C in gdb              | SIGINT                             |
| debugger shortcut key      | SIGINT                             |
| program start / NatFeats   | SIGTRAP                            |

DSP breakpoints and steps, and any stops while gdb is not attached, go
to the console debugger as usual.

### Exceptions

While gdb is attached, these CPU exceptions invoke the debugger (in
addition to the ones enabled with `--debug-except`), and are reported
with the matching signal:

| Exception                                              | Signal  |
|--------------------------------------------------------|---------|
| bus error, address error                               | SIGBUS  |
| illegal instruction, privilege violation, line-A, line-F | SIGILL |
| division by zero, CHK, TRAPV                           | SIGFPE  |
| trace                                                  | SIGTRAP |
| other exceptions                                       | SIGSEGV |

For bus and address errors, PC has already moved past the faulting
instruction when the debugger is invoked. The server reports the
faulting instruction's address as PC instead, so gdb shows where the
error happened.

### Interrupting

gdb's interrupt request (Ctrl-C) is a single 0x03 byte sent while
emulation runs. `GdbServer_Poll()` notices it and stops emulation with
SIGINT.


## 4. Programs as the gdb "process"

gdb thinks in terms of processes, but Hatari emulates a whole machine.
The server treats programs started from GEMDOS HD emulation as gdb's
"process". The hooks for this are in `gemdos.c`:
`GdbServer_ProgramStart()` is called when Pexec has loaded a program
(basepage in D0), and `GdbServer_ProgramExit()` from the Pterm0 / Pterm
/ Ptermres handlers.

There are three ways to debug a program from its start:

- **`--gdbserver-wait on`, gdb not connected yet:** when a program
  starts, the server sets a breakpoint at its first instruction, prints
  a message and blocks until gdb connects. Hatari's window doesn't
  respond while waiting. This is the easiest way to debug a program
  which Hatari autostarts.

- **extended-remote mode, gdb `run` (also `start` and `starti`):** gdb
  sends `vRun` with the program name from `set remote exec-file`. The
  server lets emulation continue and waits for a program with that name
  (any program, if the name is empty) to be started, e.g. from the
  desktop, and stops at its first instruction. gdb fetches the section
  offsets after `vRun`, so symbols are relocated correctly. Waiting is
  limited by gdb's `remotetimeout`, but gdb accepts a late reply after
  a warning.

- **`--gdbserver-wait on`, gdb already connected and continued (e.g.
  during boot):** the server stops at the program's first instruction,
  but gdb read the section offsets when it connected, before the
  program was loaded, so its symbols are not relocated. The server
  prints a gdb command that relocates them to the gdb console (an `O`
  packet), e.g. `symbol-file -o 0x18a38 hello.elf`. Reconnecting is not
  a fix, as detaching gdb resumes emulation.

For programs started with the first two methods, the server tracks the
program (`prog_info_t prog`), and reports its exit to gdb with a `W`
exit packet carrying the exit code. In plain `remote` mode the
connection is closed after that. gdb `kill` doesn't terminate the
program, it only detaches gdb from it.


## 5. Breakpoints and watchpoints

gdb inserts breakpoints with `Z` packets (`z` removes them). The server
keeps its own list of them (`bps`), so that it can classify stops and
remove all of them when gdb goes away.

| Packet      | Type                               | Implementation                   |
|-------------|------------------------------------|----------------------------------|
| `Z0` / `Z1` | software / hardware breakpoint     | conditional breakpoint `pc=$<addr>` |
| `Z2`        | write watchpoint, up to 16 bytes   | value change conditional breakpoints |
| `Z3` / `Z4` | read / access watchpoint, any size | CPU core data access hook        |

### Breakpoints

Software and hardware breakpoints are the same: a quiet conditional
breakpoint `pc=$<addr>`. gdb never writes trap instructions to memory.

### Write watchpoints

The watched area is split into long / word / byte sized conditional
breakpoints, each comparing the value with itself
(`($addr).l ! ($addr).l`), which Hatari evaluates as "value changed".
The server keeps a copy of the watched bytes, so that after a stop it
can tell gdb which watchpoint changed. As with gdb's own software
watchpoints, writes that don't change the value are not reported.
Writes by DMA are seen too.

### Read and access watchpoints

These need to see accesses, not value changes, so they use a hook in
the CPU core instead of the breakpoint engine:

- `src/cpu/memory.h` declares `dbgwatch_func`, `dbgwatch_start` and
  `dbgwatch_end`. `memory_get_*` and `memory_put_*` in `memory.c` call
  `dbgwatch_access()`, which calls `dbgwatch_func` when it is set and
  the access overlaps the watched range (the union of all read / access
  watchpoints). When nothing is watched, the cost is one pointer test
  per access.
- Instruction fetches use separate functions (`memory_get_wordi()`
  etc.), so they don't trigger the hook.
- Some bus accesses are not the CPU's operand access. They are done
  with `dbgwatch_suspend` raised, and the real access is reported with
  `dbgwatch_access()` where needed:
  - DMA (blitter, DMA sound, crossbar) in `stMemory.c`
  - byte reads that the cycle exact 68000 does as word reads
    (`custom.c`)
  - 68030 and MegaSTE data cache line fills; cache hits don't go to the
    bus at all, so they are reported separately (`newcpu.c`,
    `m68000.c`)
- On a hit, the server records the watchpoint and sets the step count
  to 1, so the debugger stops after the accessing instruction, which is
  where gdb expects a watchpoint stop.

The stop reply names the changed or accessed address (`watch:`,
`rwatch:`, `awatch:`), and gdb shows the old and new value itself.


## 6. Registers and memory

The target description (generated by `BuildTargetXml()`) uses gdb's
standard m68k register layout:

| Feature                                        | Registers                                   |
|------------------------------------------------|---------------------------------------------|
| `org.gnu.gdb.m68k.core`                        | d0-d7, a0-a5, fp (a6), sp (a7), ps (SR), pc |
| `org.gnu.gdb.coldfire.fp` (when an FPU is emulated) | fp0-fp7, fpcontrol, fpstatus, fpiaddr  |

Registers can be read and written as a whole (`g` / `G`) or one by one
(`p` / `P`). FPU registers use the 96-bit extended precision format.

Memory is read and written with `STMemory_ReadByte()` and
`STMemory_WriteByte()` (`m`, `M`, and the binary `x`, `X` variants).
These access the memory banks directly, so reading I/O registers has no
hardware side effects, and writes flush the CPU caches. Addresses are
physical, MMU translation is not done. After a memory write the server
updates its copies of the write watchpoint values, so a write by gdb
itself doesn't trigger a watchpoint.


## 7. Monitor commands

gdb `monitor <command>` (`qRcmd`) runs the given Hatari debugger
command, e.g.:

```
(gdb) monitor info basepage
(gdb) monitor r
(gdb) monitor b pc = TEXT :once
```

The command's stdout and stderr output is captured into a temporary
file (by redirecting the file descriptors), and sent to gdb as console
output (`O`) packets. On Windows, a GUI program without a console has
no file descriptors behind stdout / stderr, so they are first pointed
to NUL. `monitor` without a command shows the debugger help.


## 8. Symbol lookup

gdb needs symbols for the program at the addresses where it was loaded.
Atari programs are relocated at load time, so the addresses in gdb's
ELF file and in the emulated memory differ. There are two ways for gdb
to get symbols.

### 8.1 gdb's own ELF file

The usual case: gdb is given the ELF file from which the program was
made (`gdb hello.elf` or `file hello.elf`). gdb then asks the server for
the section offsets with `qOffsets`, and relocates the ELF file's
symbols and debug information with them. The reply is:

```
TextSeg=<offset>;DataSeg=<offset>
```

The text offset is the text section start from the program's basepage.
The data offset depends on the toolchain that made the program, which
the server tells from the PRG header field at offset 18:

| Toolchain          | Header field         | Data addresses relative to                     | DataSeg     |
|--------------------|----------------------|------------------------------------------------|-------------|
| m68k-atari-elf     | `0x68e1f001`         | text section start (data follows text in the ELF file) | text offset |
| m68k-atari-mintelf | starts with `"ELF"`  | data section start                             | data offset |

The basepage comes from the tracked program (see
[section 4](#4-programs-as-the-gdb-process)) or, if there is none, from
the program that is currently running from GEMDOS HD. When there is no
program, the reply is empty and gdb uses the ELF addresses as they are.

gdb reads `qOffsets` only when it connects (and after `run`). If the
program starts later, gdb's symbols are wrong until relocated (see the
`--gdbserver-wait` case in [section 4](#4-programs-as-the-gdb-process)).

### 8.2 ELF file generated from Hatari symbols

When gdb is started without a file, it asks for the executable's name
(`qXfer:exec-file:read`), and the server gives it a name in a virtual
directory, e.g. `/hatari/HELLO.PRG.elf` (`/hatari/symbols.elf` when no
program is running). gdb then reads that file from the target with
`vFile` packets (setfs, open, pread, fstat, close). gdb shows this as
"Reading /hatari/HELLO.PRG.elf from remote target...".

The file doesn't exist on disk. On open, `BuildElf()` generates it in
memory from the Hatari debugger's CPU symbols:

- Symbols are first loaded with Hatari's normal symbol autoloading:
  `<program>.sym` if it exists, otherwise the program's own symbol table
  (DRI/GST, a.out, or mintelf ELF format). Programs made with the
  m68k-atari-elf toolchain never have a symbol table, but their symbols
  can be given in a `.sym` file made with:

  ```
  m68k-atari-elf-nm --defined-only hello.elf > hello.sym
  ```

  Manually loaded symbols, e.g. for TOS, are used as well.
- The ELF file has .text, .data and .bss sections with the program's
  section sizes, but no contents (`SHT_NOBITS`). gdb reads code and data
  from the emulated memory.
- Section addresses and symbol values are made relative to the same
  offsets that `qOffsets` reports, so gdb relocates them exactly like a
  real ELF file from the toolchain. Symbols outside the program's
  sections stay absolute.
- Code symbols become functions (`STT_FUNC`), with the distance to the
  next code symbol as their size, so that gdb's `finish`, backtraces and
  `info symbol` work. Data and BSS symbols become objects.

This gives gdb function and variable names, but no types or source line
information.

### 8.3 Hatari's own symbol use

Independently of gdb, the server calls
`Symbols_AutoLoadCurrentProgram()` whenever emulation stops for gdb, so
Hatari's symbols are available for `monitor` commands (e.g.
`monitor b pc = main`).


## 9. Supported packets

| Packet                                   | Purpose                                         |
|------------------------------------------|-------------------------------------------------|
| `qSupported`                             | feature negotiation                             |
| `QStartNoAckMode`                        | disable packet acknowledgements                 |
| `!`                                      | enable extended mode                            |
| `?`                                      | stop reason                                     |
| `g`, `G`, `p`, `P`                       | read / write registers                          |
| `m`, `M`, `x`, `X`                       | read / write memory (hex / binary)              |
| `c`, `C`, `s`, `S`                       | continue / step (signals are ignored)           |
| `vCont?`, `vCont;c/s/r/t`                | continue, step, range step, stop                |
| `Z0`-`Z4`, `z0`-`z4`                     | insert / remove breakpoints and watchpoints     |
| `vRun`                                   | wait for a program to start (extended mode)     |
| `vAttach`                                | attach to the emulation (extended mode)         |
| `vKill`, `k`                             | forget the "process", emulation continues       |
| `D`                                      | detach                                          |
| `H`, `T`                                 | thread selection / query (single thread)        |
| `qC`, `qfThreadInfo`, `qsThreadInfo`     | thread queries                                  |
| `qAttached`                              | 0 for a program started with `vRun`, 1 (attached) otherwise |
| `qOffsets`                               | program section offsets                         |
| `qSymbol`                                | symbol lookup service (not needed)              |
| `qXfer:features:read`                    | target description                              |
| `qXfer:exec-file:read`                   | executable file name                            |
| `vFile:...`                              | read the generated ELF file                     |
| `qRcmd`                                  | `monitor` commands                              |

Other packets get an empty reply, which gdb treats as "not supported".
Packets are limited to 16 KiB (advertised as `PacketSize`).


## 10. Limitations

- Only the CPU can be debugged. DSP breakpoints and steps invoke the
  console debugger.
- Memory addresses are physical, MMU translation is not done.
- gdb can't deliver signals to the program; signals given to `C` / `S`
  are ignored.
- Read and access watchpoints don't see accesses served from an enabled
  68040 / 68060 data cache, and cache line fills can trigger them for
  neighbouring addresses. A few rare instruction fetches (e.g. prefetch
  refill after an exception) go through the data access functions, so
  read watchpoints on code can trigger on them.
- Only programs started from GEMDOS HD can be the gdb "process" (and
  have their start and exit reported). Programs on disk images can
  still be debugged, but gdb's symbols must be relocated manually.
- The server doesn't work in the libretro core (see
  [section 1](#platforms)).


## 11. Testing

### With the m68k-atari-elf toolchain

Build a test program and convert it to an Atari program:

```
m68k-atari-elf-gcc -g -o hello.elf hello.c
m68k-atari-elf-prg hello.elf HELLO.PRG
```

Run Hatari headless with the program's directory as drive C:, and let
it autostart the program and wait for gdb:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy hatari --machine ste \
	--tos tos206.img --fast-boot true --sound off \
	--confirm-quit false --harddrive <dir> --gemdos-drive C \
	--conout 2 --gdbserver 1234 --gdbserver-wait on \
	<dir>/HELLO.PRG
```

Then drive gdb with a script (`gdb-multiarch -batch -x test.gdb`):

```
set confirm off
set remotetimeout 10
file hello.elf
target extended-remote localhost:1234
break main
continue
...
```

### With the m68k-atari-mintelf toolchain

The linker output is the Atari program itself, but it also contains the
ELF file (headers, symbols and debug information), so the same file is
used both on the Hatari drive and for gdb:

```
m68k-atari-mintelf-gcc -g -o hello hello.c
cp hello <dir>/HELLO.PRG
```

Start Hatari the same way as above, and give gdb the linker output:

```
set confirm off
set remotetimeout 10
file hello
target extended-remote localhost:1234
break main
continue
...
```

Testing with both toolchains is useful, because they relocate data
differently: `qOffsets` reports the data section start as DataSeg for
mintelf programs, and the text section start for m68k-atari-elf ones
(see [section 8.1](#81-gdbs-own-elf-file)). Hatari's own debugger reads
the symbols of mintelf programs directly from the program file, so the
ELF file generated for gdb (see
[section 8.2](#82-elf-file-generated-from-hatari-symbols)) can be tested
with them without a `.sym` file.

### Tips

- `set debug remote 1` makes gdb log all packets.
- In batch mode, `continue &` followed by `interrupt` never processes
  the stop. To test Ctrl-C, use `shell (sleep 2; kill -INT $PPID) &`
  followed by a plain `continue`.
