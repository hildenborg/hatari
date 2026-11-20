/*
  Hatari - gdbserver.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  GDB remote serial protocol server for the Hatari debugger.
*/

#ifndef HATARI_GDBSERVER_H
#define HATARI_GDBSERVER_H

#include "debugui.h"

#if HAVE_GDBSERVER

/* options / debugger command */
extern const char *GdbServer_Start(const char *address);
extern void GdbServer_Stop(void);
extern void GdbServer_SetWaitAtProgramStart(bool wait);
extern void GdbServer_ShowStatus(FILE *fp);

/* called regularly from the main event loop */
extern void GdbServer_Poll(void);
extern bool GdbServer_IsListening(void);

/* debugger entry, returns true when gdb handled it */
extern bool GdbServer_DebugUI(debug_reason_t reason);
extern void GdbServer_NoteException(int nr);

/* GEMDOS HD program start / termination */
extern void GdbServer_ProgramStart(const char *path, uint32_t basepage);
extern void GdbServer_ProgramExit(uint32_t basepage, int exitcode);

extern void GdbServer_UnInit(void);

#else

#define GdbServer_Start(addr) "GDB server is not supported on this platform."
#define GdbServer_Stop()
#define GdbServer_SetWaitAtProgramStart(wait)
#define GdbServer_ShowStatus(fp) fputs("GDB server is not supported on this platform.\n", fp)
#define GdbServer_Poll()
#define GdbServer_IsListening() false
#define GdbServer_DebugUI(reason) false
#define GdbServer_NoteException(nr)
#define GdbServer_ProgramStart(path, basepage)
#define GdbServer_ProgramExit(basepage, exitcode)
#define GdbServer_UnInit()

#endif /* HAVE_GDBSERVER */

#endif /* HATARI_GDBSERVER_H */
