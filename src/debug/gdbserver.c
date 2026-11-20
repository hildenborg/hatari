/*
  Hatari - gdbserver.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  GDB remote serial protocol server.  It listens on a TCP port and lets
  gdb (e.g. gdb-multiarch, or m68k-atari-elf-gdb) debug the emulated CPU
  through the Hatari debugger:

  - Run control (continue, step, range step, Ctrl-C) and breakpoints use
    the Hatari debugger's stepping and conditional breakpoints.  gdb
    breakpoints show up in the Hatari debugger as quiet conditional
    breakpoints, and Hatari debugger stops (its own breakpoints,
    exceptions, debugger shortcut, NatFeats) are reported to gdb.
  - "monitor <command>" in gdb runs the given Hatari debugger command
    and sends its output back to gdb.
  - Symbols: gdb can use its own ELF file (relocated with qOffsets to the
    address the program was loaded at), or, when gdb has no file, gdb is
    given an ELF file generated from the Hatari debugger's symbols for
    the currently running program (qXfer:exec-file + vFile).
  - Programs started from GEMDOS HD are the gdb "process": gdb 'run'
    (in extended-remote mode) waits for the next program to start and
    stops at its first instruction, and the program exit is reported.

  The emulated machine is single threaded: while gdb has it stopped,
  emulation is blocked in this server's command loop, the same way as
  with the console debugger.
*/
const char GdbServer_fileid[] = "Hatari gdbserver.c";

#ifdef _WIN32
/* Winsock must come before windows.h, and WSAPoll() needs Vista or later */
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
#endif

#include "main.h"
#include "gdbserver.h"

#if HAVE_GDBSERVER

#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <errno.h>
#include <ctype.h>

#include "configuration.h"
#include "breakcond.h"
#include "debugcpu.h"
#include "debugInfo.h"
#include "debug_priv.h"
#include "file.h"
#include "log.h"
#include "m68000.h"
#include "screen.h"
#include "stMemory.h"
#include "str.h"
#include "symbols.h"
#include "fpp.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Socket API differences between Winsock and POSIX */
#ifdef _WIN32
typedef SOCKET sock_t;
#define SOCK_NONE	INVALID_SOCKET
#define SockClose(fd)	closesocket(fd)
#define SockPoll(pfd, n, timeout)	WSAPoll(pfd, n, timeout)
#define SockError()	WSAGetLastError()
#define SockInterrupted(err)	((err) == WSAEINTR)
#define SockWouldBlock(err)	((err) == WSAEWOULDBLOCK)
#else
typedef int sock_t;
#define SOCK_NONE	(-1)
#define SockClose(fd)	close(fd)
#define SockPoll(pfd, n, timeout)	poll(pfd, n, timeout)
#define SockError()	errno
#define SockInterrupted(err)	((err) == EINTR)
#define SockWouldBlock(err)	((err) == EAGAIN || (err) == EWOULDBLOCK)
#endif

#ifdef _WIN32
static bool wsaStarted;

/**
 * Initialize Winsock (once). Return false on failure.
 */
static bool SockInit(void)
{
	WSADATA wsaData;

	if (wsaStarted)
		return true;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return false;
	wsaStarted = true;
	return true;
}

/**
 * Release Winsock, if it was initialized.
 */
static void SockExit(void)
{
	if (wsaStarted) {
		WSACleanup();
		wsaStarted = false;
	}
}
#else
#define SockInit()	true
#define SockExit()
#endif

/* Max packet size accepted from gdb (advertised as hex in qSupported) */
#define GDB_PACKET_SIZE 0x4000
/* Max watchpoint length in bytes */
#define GDB_WATCH_MAX 16
/* File descriptor number given to gdb for the generated ELF file */
#define GDB_ELF_FD 3
/* Virtual directory for files served to gdb */
#define GDB_VFILE_DIR "/hatari/"

/* gdb signal numbers */
#define GDB_SIGINT  2
#define GDB_SIGILL  4
#define GDB_SIGTRAP 5
#define GDB_SIGFPE  8
#define GDB_SIGBUS  10
#define GDB_SIGSEGV 11

/* exceptions that are reported to gdb while it is attached */
#define GDB_EXCEPTIONS (EXCEPT_BUS|EXCEPT_ADDRESS|EXCEPT_ILLEGAL|EXCEPT_ZERODIV|EXCEPT_TRAPV|EXCEPT_PRIVILEGE)

typedef enum {
	ACT_NONE,	/* keep processing packets */
	ACT_RESUME,	/* resume emulation */
	ACT_DETACH,	/* close connection and resume emulation */
	ACT_ATTACH	/* stop emulation (from idle state) */
} gdb_action_t;

typedef enum {
	STOP_SIGNAL,
	STOP_SWBREAK,
	STOP_HWBREAK,
	STOP_WATCH
} stop_kind_t;

typedef enum {
	BP_SW = 0,
	BP_HW = 1,
	BP_WATCH = 2
} bp_type_t;

typedef struct {
	bp_type_t type;
	uint32_t addr;
	uint32_t len;
	uint8_t value[GDB_WATCH_MAX];	/* last seen watched memory */
} gdb_bp_t;

typedef struct {
	bool valid;
	uint32_t basepage;
	uint32_t text, textlen;
	uint32_t data, datalen;
	uint32_t bss, bsslen;
	bool mintelf;
} prog_info_t;

/* --- connection --- */
static sock_t listenFd = SOCK_NONE;
static sock_t clientFd = SOCK_NONE;
static char listenAddr[160];

/* --- protocol state --- */
static bool noAckMode;
static bool extendedMode;
static bool ctrlC;		/* gdb sent interrupt request */

static uint8_t *inBuf;		/* received raw bytes */
static size_t inLen, inSize;
static char pkt[GDB_PACKET_SIZE + 1];	/* current unescaped packet */
static size_t pktLen;
static uint8_t *outBuf;		/* reply being built */
static size_t outLen, outSize;
static uint8_t *lastSent;	/* last framed packet, for resend on NAK */
static size_t lastSentLen;

/* --- run state --- */
static bool attached;		/* gdb has a "process" (the emulation) */
static bool stopped;		/* in the stop loop */
static bool stopReplyPending;	/* gdb waits for a stop reply */
static bool waitForProgram;	/* vRun: waiting for a program to start */
static char waitProgramName[256];
static bool waitAtStart;	/* --gdbserver-wait */
static int lastSignal = GDB_SIGTRAP;
static stop_kind_t stopKind;
static uint32_t stopWatchAddr;
static uint32_t rangeStart, rangeEnd;	/* vCont;r range */
static unsigned rangeSteps;
static int pendingException;
static bool faultPcValid;	/* report faulting instruction address as PC */
static uint32_t faultPc;
static uint32_t startBreak;	/* program start breakpoint address, or 0 */

static int savedExceptionMask;
static bool exceptionMaskApplied;

/* --- tracked program (started while gdb was waiting for it) --- */
static prog_info_t prog;

/* --- breakpoints added by gdb --- */
static gdb_bp_t *bps;
static int bpCount, bpAlloc;

/* --- generated ELF file --- */
static uint8_t *elfBuf;
static size_t elfLen;
static bool elfOpen;


/* ------------------------------------------------------------------ */
/* helpers */

static int HexVal(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/**
 * Parse hex number from *str, advance *str past it.
 * Return false if there were no hex digits.
 */
static bool ParseHex(const char **str, uint32_t *value)
{
	const char *s = *str;
	uint32_t v = 0;
	int d;

	while ((d = HexVal((unsigned char)*s)) >= 0) {
		v = (v << 4) | d;
		s++;
	}
	if (s == *str)
		return false;
	*str = s;
	*value = v;
	return true;
}

/**
 * Decode hex string to bytes, return number of bytes or -1 on error.
 */
static int HexDecode(const char *hex, size_t hexlen, uint8_t *dst, size_t dstsize)
{
	size_t i;
	if (hexlen & 1 || hexlen / 2 > dstsize)
		return -1;
	for (i = 0; i < hexlen; i += 2) {
		int h = HexVal((unsigned char)hex[i]);
		int l = HexVal((unsigned char)hex[i+1]);
		if (h < 0 || l < 0)
			return -1;
		dst[i/2] = (h << 4) | l;
	}
	return hexlen / 2;
}

static bool HasFpu(void)
{
	return ConfigureParams.System.n_FPUType != FPU_NONE;
}

static void PutBE32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void PutBE16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}


/* ------------------------------------------------------------------ */
/* output buffer */

static void OutReserve(size_t len)
{
	if (outLen + len > outSize) {
		outSize = (outLen + len) * 2;
		outBuf = realloc(outBuf, outSize);
		if (!outBuf) {
			perror("GDB server");
			exit(1);
		}
	}
}

static void OutClear(void)
{
	outLen = 0;
}

static void OutData(const void *data, size_t len)
{
	OutReserve(len);
	memcpy(outBuf + outLen, data, len);
	outLen += len;
}

static void OutStr(const char *str)
{
	OutData(str, strlen(str));
}

static void OutPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void OutPrintf(const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len > 0)
		OutData(buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

static void OutHexBytes(const uint8_t *data, size_t len)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	OutReserve(len * 2);
	for (i = 0; i < len; i++) {
		outBuf[outLen++] = digits[data[i] >> 4];
		outBuf[outLen++] = digits[data[i] & 15];
	}
}

static void OutHex32(uint32_t value)
{
	uint8_t b[4];
	PutBE32(b, value);
	OutHexBytes(b, 4);
}

static void OutError(int err)
{
	OutPrintf("E%02x", err);
}


/* ------------------------------------------------------------------ */
/* socket I/O */

static void CloseClient(void);

static bool WriteAll(const uint8_t *data, size_t len)
{
	while (len > 0) {
		long n = send(clientFd, (const void *)data, len, MSG_NOSIGNAL);
		if (n < 0) {
			int err = SockError();
			if (SockInterrupted(err))
				continue;
			if (SockWouldBlock(err)) {
				struct pollfd pfd = { clientFd, POLLOUT, 0 };
				SockPoll(&pfd, 1, 1000);
				continue;
			}
			return false;
		}
		data += n;
		len -= n;
	}
	return true;
}

/**
 * Send the output buffer as a packet: $<escaped data>#<checksum>
 */
static void SendPacket(void)
{
	uint8_t sum = 0;
	size_t i, n = 0;

	if (clientFd == SOCK_NONE)
		return;

	lastSent = realloc(lastSent, outLen * 2 + 8);
	if (!lastSent) {
		perror("GDB server");
		exit(1);
	}
	lastSent[n++] = '$';
	for (i = 0; i < outLen; i++) {
		uint8_t c = outBuf[i];
		if (c == '$' || c == '#' || c == '}' || c == '*') {
			lastSent[n++] = '}';
			sum += '}';
			c ^= 0x20;
		}
		lastSent[n++] = c;
		sum += c;
	}
	n += sprintf((char *)lastSent + n, "#%02x", sum);
	lastSentLen = n;

	if (!WriteAll(lastSent, n)) {
		fprintf(stderr, "GDB server: write to gdb failed.\n");
	}
	OutClear();
}

/**
 * Read available bytes from gdb, waiting up to timeout milliseconds
 * (-1 = forever).  Return -1 when the connection has been closed,
 * otherwise the number of bytes read.
 */
static int ReadInput(int timeout)
{
	struct pollfd pfd;
	long n;
	int ret;

	if (clientFd == SOCK_NONE)
		return -1;

	pfd.fd = clientFd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	do {
		ret = SockPoll(&pfd, 1, timeout);
	} while (ret < 0 && SockInterrupted(SockError()));
	if (ret <= 0)
		return 0;

	if (inLen + 4096 > inSize) {
		inSize = inLen + 8192;
		inBuf = realloc(inBuf, inSize);
		if (!inBuf) {
			perror("GDB server");
			exit(1);
		}
	}
	do {
		n = recv(clientFd, (void *)(inBuf + inLen), inSize - inLen, 0);
	} while (n < 0 && SockInterrupted(SockError()));
	if (n <= 0) {
		if (n < 0 && SockWouldBlock(SockError()))
			return 0;
		return -1;
	}
	inLen += n;
	return n;
}

static void InputConsume(size_t len)
{
	memmove(inBuf, inBuf + len, inLen - len);
	inLen -= len;
}

/**
 * Parse next packet from the input buffer into pkt.
 * Acks, NAKs and interrupt requests are handled on the way.
 * Return true if a complete, valid packet was received.
 */
static bool ParsePacket(void)
{
	while (inLen > 0) {
		uint8_t c = inBuf[0];
		size_t i, end;
		uint8_t sum;
		bool escaped;

		if (c == '+') {
			InputConsume(1);
			continue;
		}
		if (c == '-') {
			InputConsume(1);
			if (!noAckMode && lastSentLen)
				WriteAll(lastSent, lastSentLen);
			continue;
		}
		if (c == 0x03) {
			InputConsume(1);
			ctrlC = true;
			continue;
		}
		if (c != '$') {
			InputConsume(1);
			continue;
		}
		/* find end of the packet */
		for (end = 1; end < inLen && inBuf[end] != '#'; end++) {
			if (inBuf[end] == '$')
				break;
		}
		if (end < inLen && inBuf[end] == '$') {
			/* broken packet, skip it */
			InputConsume(end);
			continue;
		}
		if (end + 3 > inLen)
			return false;	/* incomplete */

		sum = 0;
		pktLen = 0;
		escaped = false;
		for (i = 1; i < end; i++) {
			c = inBuf[i];
			sum += c;
			if (escaped) {
				c ^= 0x20;
				escaped = false;
			} else if (c == '}') {
				escaped = true;
				continue;
			}
			if (pktLen < GDB_PACKET_SIZE)
				pkt[pktLen++] = c;
		}
		pkt[pktLen] = '\0';

		if (!noAckMode) {
			int h = HexVal(inBuf[end+1]), l = HexVal(inBuf[end+2]);
			if (h < 0 || l < 0 || ((h << 4) | l) != sum) {
				InputConsume(end + 3);
				WriteAll((const uint8_t *)"-", 1);
				continue;
			}
			WriteAll((const uint8_t *)"+", 1);
		}
		InputConsume(end + 3);
		return true;
	}
	return false;
}

/**
 * Look for interrupt requests (Ctrl-C) between packets in the input
 * buffer while emulation is running.  Packets are left in the buffer
 * to be processed when emulation stops.
 */
static void ScanInterrupt(void)
{
	size_t i = 0;

	while (i < inLen) {
		if (inBuf[i] == 0x03) {
			memmove(inBuf + i, inBuf + i + 1, inLen - i - 1);
			inLen--;
			ctrlC = true;
			continue;
		}
		if (inBuf[i] == '$') {
			/* skip packet, up to and including checksum */
			while (i < inLen && inBuf[i] != '#')
				i++;
			i += 3;
			continue;
		}
		i++;
	}
}

/**
 * Wait for next packet.  Return 1 for packet, 0 if interrupted
 * without a packet (Ctrl-C), -1 when the connection was closed.
 */
static int GetPacket(void)
{
	for (;;) {
		if (ParsePacket())
			return 1;
		if (ctrlC)
			return 0;
		if (ReadInput(-1) < 0)
			return -1;
	}
}


/* ------------------------------------------------------------------ */
/* program information */

static bool IsMintElf(const char *path)
{
	uint8_t hdr[22];
	bool ret = false;
	FILE *fp;

	if (!path || !(fp = fopen(path, "rb")))
		return false;
	if (fread(hdr, sizeof(hdr), 1, fp) == 1) {
		/* symbol table format in the "reserved" header field */
		uint32_t magic = (hdr[18] << 24) | (hdr[19] << 16) | (hdr[20] << 8);
		ret = (magic == 0x454c4600);	/* "ELF" */
	}
	fclose(fp);
	return ret;
}

static bool ReadProgramInfo(uint32_t basepage, const char *path, prog_info_t *info)
{
	memset(info, 0, sizeof(*info));
	if (!basepage || !STMemory_CheckAreaType(basepage, 0x100, ABFLAG_RAM))
		return false;
	info->basepage = basepage;
	info->text    = STMemory_ReadLong(basepage + 0x08);
	info->textlen = STMemory_ReadLong(basepage + 0x0c);
	info->data    = STMemory_ReadLong(basepage + 0x10);
	info->datalen = STMemory_ReadLong(basepage + 0x14);
	info->bss     = STMemory_ReadLong(basepage + 0x18);
	info->bsslen  = STMemory_ReadLong(basepage + 0x1c);
	info->mintelf = IsMintElf(path);
	info->valid = true;
	return true;
}

/**
 * Get information about the program gdb is debugging: the program
 * started for gdb, or the program currently running from GEMDOS HD.
 */
static bool GetProgramInfo(prog_info_t *info)
{
	const char *path;

	if (prog.valid) {
		*info = prog;
		return true;
	}
	path = Symbols_GetCurrentProgramPath();
	if (!path)
		return false;
	return ReadProgramInfo(DebugInfo_GetBASEPAGE(), path, info);
}

/**
 * Offsets that gdb adds to the text and data sections of its ELF file.
 * Elf toolchain (m68k-atari-elf) data symbols use the text segment as
 * base, mintelf toolchain data symbols use the data segment as base.
 */
static void GetSectionOffsets(const prog_info_t *info, uint32_t *textoff, uint32_t *dataoff)
{
	*textoff = info->text;
	*dataoff = info->mintelf ? info->data : info->text;
}

/**
 * Case-insensitively compare basenames of given (Atari or host) paths.
 */
static bool ProgramNameMatches(const char *path, const char *name)
{
	const char *a, *b;

	if (!name[0])
		return true;
	a = strrchr(path, '/');
	if (!a || strrchr(path, '\\') > a)
		a = strrchr(path, '\\');
	a = a ? a + 1 : path;
	b = strrchr(name, '/');
	if (!b || strrchr(name, '\\') > b)
		b = strrchr(name, '\\');
	if (!b)
		b = strrchr(name, ':');
	b = b ? b + 1 : name;
	return strcasecmp(a, b) == 0;
}


/* ------------------------------------------------------------------ */
/* breakpoints */

static bool AddHatariBreak(const char *expr)
{
	char cmd[96];
	snprintf(cmd, sizeof(cmd), "%s :quiet", expr);
	return BreakCond_Command(cmd, false);
}

static void BreakExpression(char *buf, size_t size, bp_type_t type, uint32_t addr, int width)
{
	if (type == BP_WATCH) {
		char mode = width == 4 ? 'l' : (width == 2 ? 'w' : 'b');
		snprintf(buf, size, "($%x).%c!($%x).%c", addr, mode, addr, mode);
	} else {
		snprintf(buf, size, "pc=$%x", addr);
	}
}

/* Watched region is split to long / word / byte sized conditions */
static int WatchChunk(uint32_t remaining)
{
	return remaining >= 4 ? 4 : (remaining >= 2 ? 2 : 1);
}

static void RemoveHatariBreaks(const gdb_bp_t *bp)
{
	char expr[64];
	uint32_t off = 0;

	if (bp->type != BP_WATCH) {
		BreakExpression(expr, sizeof(expr), bp->type, bp->addr, 0);
		BreakCond_RemoveCpuExpression(expr);
		return;
	}
	while (off < bp->len) {
		int width = WatchChunk(bp->len - off);
		BreakExpression(expr, sizeof(expr), bp->type, bp->addr + off, width);
		BreakCond_RemoveCpuExpression(expr);
		off += width;
	}
}

static void ReadWatchValue(gdb_bp_t *bp, uint8_t *dst)
{
	uint32_t i;
	for (i = 0; i < bp->len; i++)
		dst[i] = STMemory_ReadByte(bp->addr + i);
}

static int FindBreak(bp_type_t type, uint32_t addr, uint32_t len)
{
	int i;
	for (i = 0; i < bpCount; i++) {
		if (bps[i].type == type && bps[i].addr == addr &&
		    (type != BP_WATCH || bps[i].len == len))
			return i;
	}
	return -1;
}

static bool InsertBreak(bp_type_t type, uint32_t addr, uint32_t len)
{
	gdb_bp_t *bp;
	char expr[64];
	uint32_t off = 0;

	if (FindBreak(type, addr, len) >= 0)
		return true;
	if (type == BP_WATCH && (len == 0 || len > GDB_WATCH_MAX))
		return false;

	if (bpCount >= bpAlloc) {
		bpAlloc = bpAlloc ? bpAlloc * 2 : 16;
		bps = realloc(bps, bpAlloc * sizeof(gdb_bp_t));
		if (!bps) {
			perror("GDB server");
			exit(1);
		}
	}
	bp = &bps[bpCount];
	memset(bp, 0, sizeof(*bp));
	bp->type = type;
	bp->addr = addr;
	bp->len = (type == BP_WATCH) ? len : 0;

	if (type != BP_WATCH) {
		BreakExpression(expr, sizeof(expr), type, addr, 0);
		if (!AddHatariBreak(expr))
			return false;
	} else {
		while (off < len) {
			int width = WatchChunk(len - off);
			BreakExpression(expr, sizeof(expr), type, addr + off, width);
			if (!AddHatariBreak(expr)) {
				/* remove the already added parts */
				bp->len = off;
				RemoveHatariBreaks(bp);
				return false;
			}
			off += width;
		}
		ReadWatchValue(bp, bp->value);
	}
	bpCount++;
	return true;
}

static bool RemoveBreak(bp_type_t type, uint32_t addr, uint32_t len)
{
	int i = FindBreak(type, addr, len);
	if (i < 0)
		return false;
	RemoveHatariBreaks(&bps[i]);
	bps[i] = bps[--bpCount];
	return true;
}

static void RemoveAllBreaks(void)
{
	while (bpCount > 0)
		RemoveHatariBreaks(&bps[--bpCount]);
}

static void RemoveStartBreak(void)
{
	char expr[64];
	if (!startBreak)
		return;
	BreakExpression(expr, sizeof(expr), BP_SW, startBreak, 0);
	BreakCond_RemoveCpuExpression(expr);
	startBreak = 0;
}

/**
 * Find out why gdb breakpoint stop happened, and update the
 * watched values.
 */
static void ClassifyBreak(uint32_t pc)
{
	uint8_t value[GDB_WATCH_MAX];
	int i;

	stopKind = STOP_SIGNAL;
	for (i = 0; i < bpCount; i++) {
		gdb_bp_t *bp = &bps[i];
		if (bp->type != BP_WATCH)
			continue;
		ReadWatchValue(bp, value);
		if (memcmp(value, bp->value, bp->len) != 0) {
			memcpy(bp->value, value, bp->len);
			if (stopKind != STOP_WATCH) {
				stopKind = STOP_WATCH;
				stopWatchAddr = bp->addr;
			}
		}
	}
	if (stopKind == STOP_WATCH)
		return;
	if ((i = FindBreak(BP_SW, pc, 0)) >= 0)
		stopKind = STOP_SWBREAK;
	else if ((i = FindBreak(BP_HW, pc, 0)) >= 0)
		stopKind = STOP_HWBREAK;
}

static void UpdateWatchValues(void)
{
	int i;
	for (i = 0; i < bpCount; i++) {
		if (bps[i].type == BP_WATCH)
			ReadWatchValue(&bps[i], bps[i].value);
	}
}


/* ------------------------------------------------------------------ */
/* attach / detach */

static void ApplyExceptionMask(void)
{
	if (exceptionMaskApplied)
		return;
	savedExceptionMask = ExceptionDebugMask;
	ExceptionDebugMask |= GDB_EXCEPTIONS;
	exceptionMaskApplied = true;
}

static void RestoreExceptionMask(void)
{
	if (!exceptionMaskApplied)
		return;
	ExceptionDebugMask = (ExceptionDebugMask & ~GDB_EXCEPTIONS)
		| (savedExceptionMask & GDB_EXCEPTIONS);
	exceptionMaskApplied = false;
}

static void Attach(void)
{
	attached = true;
	ApplyExceptionMask();
}

/**
 * gdb no longer debugs anything: remove its breakpoints and let
 * emulation run freely.
 */
static void Release(void)
{
	RemoveAllBreaks();
	RemoveStartBreak();
	RestoreExceptionMask();
	DebugCpu_SetStepCount(0);
	rangeStart = rangeEnd = 0;
	attached = false;
	stopReplyPending = false;
	waitForProgram = false;
	prog.valid = false;
	DebugCpu_SetDebugging();
}

static void CloseClient(void)
{
	if (clientFd == SOCK_NONE)
		return;
	SockClose(clientFd);
	clientFd = SOCK_NONE;
	Release();
	inLen = 0;
	noAckMode = false;
	extendedMode = false;
	ctrlC = false;
	elfOpen = false;
	fprintf(stderr, "GDB server: gdb disconnected.\n");
}

static bool AcceptClient(int timeout)
{
	struct pollfd pfd;
	sock_t fd;
	int one = 1;

	pfd.fd = listenFd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (SockPoll(&pfd, 1, timeout) <= 0)
		return false;
	fd = accept(listenFd, NULL, NULL);
	if (fd == SOCK_NONE)
		return false;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const void *)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const void *)&one, sizeof(one));
#endif
	clientFd = fd;
	inLen = 0;
	noAckMode = false;
	extendedMode = false;
	ctrlC = false;
	lastSentLen = 0;
	fprintf(stderr, "GDB server: gdb connected.\n");
	Attach();
	return true;
}


/* ------------------------------------------------------------------ */
/* registers */

static int NumRegs(void)
{
	return HasFpu() ? 18 + 8 + 3 : 18;
}

static int RegSize(int n)
{
	return (n >= 18 && n < 26) ? 12 : 4;
}

static void OutRegister(int n)
{
	if (n < 16) {
		OutHex32(Regs[REG_D0 + n]);
	} else if (n == 16) {
		OutHex32(M68000_GetSR());
	} else if (n == 17) {
		OutHex32(faultPcValid ? faultPc : M68000_GetPC());
	} else if (n < 26) {
		uae_u32 w1 = 0, w2 = 0, w3 = 0;
		fpp_from_exten_fmovem(&regs.fp[n - 18], &w1, &w2, &w3);
		OutHex32(w1);
		OutHex32(w2);
		OutHex32(w3);
	} else if (n == 26) {
		OutHex32(fpp_get_fpcr());
	} else if (n == 27) {
		OutHex32(fpp_get_fpsr());
	} else {
		OutHex32(fpp_get_fpiar());
	}
}

static uint32_t GetBE32(const uint8_t *p)
{
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void SetRegister(int n, const uint8_t *val)
{
	uint32_t v = GetBE32(val);

	if (n < 16) {
		Regs[REG_D0 + n] = v;
	} else if (n == 16) {
		M68000_SetSR(v);
	} else if (n == 17) {
		M68000_SetPC(v);
		faultPcValid = false;
	} else if (n < 26) {
		fpp_to_exten_fmovem(&regs.fp[n - 18], v, GetBE32(val + 4), GetBE32(val + 8));
	} else if (n == 26) {
		fpp_set_fpcr(v);
	} else if (n == 27) {
		fpp_set_fpsr(v);
	} else {
		fpp_set_fpiar(v);
	}
}

static void CmdReadRegisters(void)
{
	int i;
	for (i = 0; i < NumRegs(); i++)
		OutRegister(i);
}

static void CmdWriteRegisters(void)
{
	uint8_t val[12];
	const char *p = pkt + 1;
	size_t left = pktLen - 1;
	int i;

	for (i = 0; i < NumRegs(); i++) {
		int size = RegSize(i);
		if (left < (size_t)size * 2)
			break;
		/* 'x' digits mean register value is unavailable / not to be changed */
		if (!memchr(p, 'x', size * 2)) {
			if (HexDecode(p, size * 2, val, sizeof(val)) != size) {
				OutError(1);
				return;
			}
			SetRegister(i, val);
		}
		p += size * 2;
		left -= size * 2;
	}
	OutStr("OK");
}

static void CmdReadRegister(void)
{
	const char *p = pkt + 1;
	uint32_t n;

	if (!ParseHex(&p, &n) || n >= (uint32_t)NumRegs()) {
		OutError(1);
		return;
	}
	OutRegister(n);
}

static void CmdWriteRegister(void)
{
	const char *p = pkt + 1;
	uint8_t val[12];
	uint32_t n;
	int size;

	if (!ParseHex(&p, &n) || *p++ != '=' || n >= (uint32_t)NumRegs()) {
		OutError(1);
		return;
	}
	size = RegSize(n);
	if (HexDecode(p, strlen(p), val, sizeof(val)) != size) {
		OutError(1);
		return;
	}
	SetRegister(n, val);
	OutStr("OK");
}


/* ------------------------------------------------------------------ */
/* memory */

static bool ParseAddrLen(const char *p, uint32_t *addr, uint32_t *len, const char **rest)
{
	if (!ParseHex(&p, addr) || *p++ != ',' || !ParseHex(&p, len))
		return false;
	if (rest)
		*rest = p;
	return true;
}

static void CmdReadMemory(bool binary)
{
	uint8_t buf[GDB_PACKET_SIZE / 2];
	uint32_t addr, len, i;

	if (!ParseAddrLen(pkt + 1, &addr, &len, NULL)) {
		OutError(1);
		return;
	}
	if (len > sizeof(buf))
		len = sizeof(buf);
	for (i = 0; i < len; i++)
		buf[i] = STMemory_ReadByte(addr + i);
	if (binary) {
		OutStr("b");
		OutData(buf, len);
	} else {
		OutHexBytes(buf, len);
	}
}

static void WriteMemory(uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i++)
		STMemory_WriteByte(addr + i, data[i]);
	UpdateWatchValues();
}

static void CmdWriteMemory(bool binary)
{
	uint8_t buf[GDB_PACKET_SIZE / 2];
	uint32_t addr, len;
	const char *data;

	if (!ParseAddrLen(pkt + 1, &addr, &len, &data) || *data++ != ':') {
		OutError(1);
		return;
	}
	if (binary) {
		size_t avail = pktLen - (data - pkt);
		if (avail < len) {
			OutError(1);
			return;
		}
		WriteMemory(addr, (const uint8_t *)data, len);
	} else {
		if (len > sizeof(buf) || HexDecode(data, strlen(data), buf, sizeof(buf)) != (int)len) {
			OutError(1);
			return;
		}
		WriteMemory(addr, buf, len);
	}
	OutStr("OK");
}


/* ------------------------------------------------------------------ */
/* stop replies */

static void OutStopReply(void)
{
	if (!attached) {
		OutStr("W00");
		return;
	}
	OutPrintf("T%02x", lastSignal);
	switch (stopKind) {
	case STOP_SWBREAK:
		OutStr("swbreak:;");
		break;
	case STOP_HWBREAK:
		OutStr("hwbreak:;");
		break;
	case STOP_WATCH:
		OutPrintf("watch:%x;", stopWatchAddr);
		break;
	default:
		break;
	}
	OutStr("thread:1;");
	/* expedite fp, sp, ps & pc */
	OutStr("0e:"); OutRegister(14); OutStr(";");
	OutStr("0f:"); OutRegister(15); OutStr(";");
	OutStr("10:"); OutRegister(16); OutStr(";");
	OutStr("11:"); OutRegister(17); OutStr(";");
}

static int ExceptionSignal(int nr)
{
	switch (nr) {
	case 2:	/* bus error */
	case 3:	/* address error */
		return GDB_SIGBUS;
	case 4:	/* illegal instruction */
	case 8:	/* privilege violation */
	case 10: /* line-A */
	case 11: /* line-F */
		return GDB_SIGILL;
	case 5:	/* division by zero */
	case 6:	/* CHK */
	case 7:	/* TRAPV */
		return GDB_SIGFPE;
	case 9:	/* trace */
		return GDB_SIGTRAP;
	default:
		return GDB_SIGSEGV;
	}
}


/* ------------------------------------------------------------------ */
/* target description & generated ELF file */

static void BuildTargetXml(char **xml, size_t *len)
{
	static char buf[4096];
	size_t n = 0;
	int i;

#define XML(...) n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__)
	XML("<?xml version=\"1.0\"?>\n"
	    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
	    "<target version=\"1.0\">\n"
	    "<architecture>m68k</architecture>\n"
	    "<feature name=\"org.gnu.gdb.m68k.core\">\n");
	for (i = 0; i < 8; i++)
		XML("<reg name=\"d%d\" bitsize=\"32\"/>\n", i);
	for (i = 0; i < 6; i++)
		XML("<reg name=\"a%d\" bitsize=\"32\" type=\"data_ptr\"/>\n", i);
	XML("<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	    "<reg name=\"ps\" bitsize=\"32\"/>\n"
	    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\n"
	    "</feature>\n");
	if (HasFpu()) {
		XML("<feature name=\"org.gnu.gdb.coldfire.fp\">\n");
		for (i = 0; i < 8; i++)
			XML("<reg name=\"fp%d\" bitsize=\"96\" type=\"float\" group=\"float\"/>\n", i);
		XML("<reg name=\"fpcontrol\" bitsize=\"32\" group=\"float\"/>\n"
		    "<reg name=\"fpstatus\" bitsize=\"32\" group=\"float\"/>\n"
		    "<reg name=\"fpiaddr\" bitsize=\"32\" type=\"code_ptr\" group=\"float\"/>\n"
		    "</feature>\n");
	}
	XML("</target>\n");
#undef XML
	*xml = buf;
	*len = n;
}

/**
 * Name of the generated ELF file given to gdb as the executable
 */
static const char *ExecFileName(void)
{
	static char name[FILENAME_MAX];
	const char *path = Symbols_GetCurrentProgramPath();
	const char *base;

	if (prog.valid || path) {
		if (!path)
			path = "program";
		base = strrchr(path, PATHSEP);
		base = base ? base + 1 : path;
		snprintf(name, sizeof(name), GDB_VFILE_DIR "%s.elf", base);
	} else {
		snprintf(name, sizeof(name), GDB_VFILE_DIR "symbols.elf");
	}
	return name;
}

static void ElfAppend(size_t *size, size_t len)
{
	if (elfLen + len > *size) {
		*size = (elfLen + len) * 2;
		elfBuf = realloc(elfBuf, *size);
		if (!elfBuf) {
			perror("GDB server");
			exit(1);
		}
	}
}

/**
 * Generate an ELF file with the Hatari debugger symbols of the current
 * program.  It has no section contents, gdb reads code and data from
 * the emulated memory.  Section addresses and symbol values are
 * relative to the qOffsets offsets, like they are in the ELF file
 * from which the program was made.
 */
static void BuildElf(void)
{
	enum { SH_NULL, SH_TEXT, SH_DATA, SH_BSS, SH_SYMTAB, SH_STRTAB, SH_SHSTRTAB, SH_COUNT };
	static const char shstrtab[] = "\0.text\0.data\0.bss\0.symtab\0.strtab\0.shstrtab";
	static const int shname[SH_COUNT] = { 0, 1, 7, 13, 18, 26, 34 };
	const symbol_t *syms;
	prog_info_t info;
	uint32_t textoff = 0, dataoff = 0;
	uint32_t secaddr[SH_COUNT] = { 0 }, secsize[SH_COUNT] = { 0 };
	uint32_t secoff[SH_COUNT] = { 0 };
	uint32_t *funcsize;
	size_t size = 0, strtabLen, symtabOff, strtabOff, shstrOff, shOff;
	int count, i, phnum;
	uint8_t *p;

	Symbols_AutoLoadCurrentProgram(NULL, 0);
	syms = Symbols_GetCpuSymbols(&count);

	if (GetProgramInfo(&info)) {
		GetSectionOffsets(&info, &textoff, &dataoff);
		secaddr[SH_TEXT] = info.text - textoff;
		secsize[SH_TEXT] = info.textlen;
		secaddr[SH_DATA] = info.data - dataoff;
		secsize[SH_DATA] = info.datalen;
		secaddr[SH_BSS] = info.bss - dataoff;
		secsize[SH_BSS] = info.bsslen;
		phnum = 2;
	} else {
		info.valid = false;
		phnum = 0;
	}

	/* function sizes: distance to the next code symbol */
	funcsize = calloc(count + 1, sizeof(uint32_t));
	if (!funcsize) {
		perror("GDB server");
		exit(1);
	}
	if (count > 0) {
		uint32_t next = info.valid ? info.text + info.textlen : 0;
		for (i = count - 1; i >= 0; i--) {
			if (!(syms[i].type & SYMTYPE_CODE))
				continue;
			if (next > syms[i].address)
				funcsize[i] = next - syms[i].address;
			if (i == 0 || syms[i-1].address != syms[i].address)
				next = syms[i].address;
		}
	}

	elfLen = 0;
	/* ELF header + program headers */
	ElfAppend(&size, 52 + phnum * 32);
	memset(elfBuf, 0, 52 + phnum * 32);
	elfLen = 52 + phnum * 32;

	/* symbol table */
	symtabOff = elfLen;
	ElfAppend(&size, (count + 1) * 16);
	memset(elfBuf + elfLen, 0, 16);
	elfLen += 16;
	strtabLen = 1;
	for (i = 0; i < count; i++) {
		const symbol_t *s = &syms[i];
		uint32_t a = s->address, value = a;
		uint16_t shndx = 0xfff1;	/* SHN_ABS */
		uint8_t type, bind;

		if (info.valid && s->type != SYMTYPE_ABS) {
			if (a >= info.text && a < info.text + info.textlen) {
				shndx = SH_TEXT;
				value = a - textoff;
			} else if (a >= info.data && a < info.data + info.datalen) {
				shndx = SH_DATA;
				value = a - dataoff;
			} else if (a >= info.bss && a <= info.bss + info.bsslen) {
				shndx = SH_BSS;
				value = a - dataoff;
			}
		}
		if (s->type & SYMTYPE_CODE)
			type = 2;	/* STT_FUNC */
		else if (s->type & (SYMTYPE_DATA|SYMTYPE_BSS))
			type = 1;	/* STT_OBJECT */
		else
			type = 0;	/* STT_NOTYPE */
		bind = (s->type == SYMTYPE_WEAK) ? 2 : 1;	/* STB_WEAK / STB_GLOBAL */

		p = elfBuf + elfLen;
		PutBE32(p, strtabLen);
		PutBE32(p + 4, value);
		PutBE32(p + 8, (s->type & SYMTYPE_CODE) ? funcsize[i] : 0);
		p[12] = (bind << 4) | type;
		p[13] = 0;
		PutBE16(p + 14, shndx);
		elfLen += 16;
		strtabLen += strlen(s->name) + 1;
	}
	free(funcsize);

	/* string table */
	strtabOff = elfLen;
	ElfAppend(&size, strtabLen);
	elfBuf[elfLen++] = 0;
	for (i = 0; i < count; i++) {
		size_t len = strlen(syms[i].name) + 1;
		memcpy(elfBuf + elfLen, syms[i].name, len);
		elfLen += len;
	}

	/* section header string table */
	shstrOff = elfLen;
	ElfAppend(&size, sizeof(shstrtab));
	memcpy(elfBuf + elfLen, shstrtab, sizeof(shstrtab));
	elfLen += sizeof(shstrtab);

	/* section headers, 4-byte aligned */
	while (elfLen & 3) {
		ElfAppend(&size, 1);
		elfBuf[elfLen++] = 0;
	}
	shOff = elfLen;
	ElfAppend(&size, SH_COUNT * 40);
	memset(elfBuf + elfLen, 0, SH_COUNT * 40);
	secoff[SH_SYMTAB] = symtabOff;
	secsize[SH_SYMTAB] = strtabOff - symtabOff;
	secoff[SH_STRTAB] = strtabOff;
	secsize[SH_STRTAB] = strtabLen;
	secoff[SH_SHSTRTAB] = shstrOff;
	secsize[SH_SHSTRTAB] = sizeof(shstrtab);
	for (i = 1; i < SH_COUNT; i++) {
		static const uint32_t shtype[SH_COUNT] = { 0, 8, 8, 8, 2, 3, 3 };	/* NOBITS, SYMTAB, STRTAB */
		static const uint32_t shflags[SH_COUNT] = { 0, 6, 3, 3, 0, 0, 0 };	/* AX, WA */
		p = elfBuf + elfLen + i * 40;
		PutBE32(p, shname[i]);
		PutBE32(p + 4, shtype[i]);
		PutBE32(p + 8, shflags[i]);
		PutBE32(p + 12, secaddr[i]);
		PutBE32(p + 16, secoff[i] ? secoff[i] : (uint32_t)shOff);
		PutBE32(p + 20, secsize[i]);
		if (i == SH_SYMTAB) {
			PutBE32(p + 24, SH_STRTAB);	/* sh_link */
			PutBE32(p + 28, 1);		/* first global symbol */
			PutBE32(p + 32, 4);		/* sh_addralign */
			PutBE32(p + 36, 16);		/* sh_entsize */
		} else {
			PutBE32(p + 32, (i <= SH_BSS) ? 2 : 1);
		}
	}
	elfLen += SH_COUNT * 40;

	/* ELF header */
	p = elfBuf;
	p[0] = 0x7f; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
	p[4] = 1;	/* ELFCLASS32 */
	p[5] = 2;	/* ELFDATA2MSB */
	p[6] = 1;	/* EV_CURRENT */
	PutBE16(p + 16, 2);	/* ET_EXEC */
	PutBE16(p + 18, 4);	/* EM_68K */
	PutBE32(p + 20, 1);	/* EV_CURRENT */
	PutBE32(p + 24, secaddr[SH_TEXT]);	/* entry */
	PutBE32(p + 28, phnum ? 52 : 0);	/* phoff */
	PutBE32(p + 32, shOff);
	PutBE16(p + 40, 52);	/* ehsize */
	PutBE16(p + 42, 32);	/* phentsize */
	PutBE16(p + 44, phnum);
	PutBE16(p + 46, 40);	/* shentsize */
	PutBE16(p + 48, SH_COUNT);
	PutBE16(p + 50, SH_SHSTRTAB);

	/* program headers: text, and data + bss segments */
	if (phnum) {
		p = elfBuf + 52;
		PutBE32(p, 1);			/* PT_LOAD */
		PutBE32(p + 4, shOff);		/* p_offset */
		PutBE32(p + 8, secaddr[SH_TEXT]);
		PutBE32(p + 12, secaddr[SH_TEXT]);
		PutBE32(p + 16, 0);		/* p_filesz */
		PutBE32(p + 20, secsize[SH_TEXT]);
		PutBE32(p + 24, 5);		/* R+X */
		PutBE32(p + 28, 2);
		p += 32;
		PutBE32(p, 1);
		PutBE32(p + 4, shOff);
		PutBE32(p + 8, secaddr[SH_DATA]);
		PutBE32(p + 12, secaddr[SH_DATA]);
		PutBE32(p + 16, 0);
		PutBE32(p + 20, secaddr[SH_BSS] + secsize[SH_BSS] - secaddr[SH_DATA]);
		PutBE32(p + 24, 6);		/* R+W */
		PutBE32(p + 28, 2);
	}
}


/* ------------------------------------------------------------------ */
/* monitor commands */

/**
 * Run Hatari debugger command, and send its output to gdb as
 * console output packets.
 */
static void CmdMonitor(const char *hexcmd)
{
	char cmd[MAX_DEBUG_CMD_LEN];
	int len, outFd, errFd, save1, save2;
	char buf[512], *tmpName;
	FILE *tmp;
	size_t n;

	len = HexDecode(hexcmd, strlen(hexcmd), (uint8_t *)cmd, sizeof(cmd) - 1);
	if (len < 0) {
		OutError(1);
		return;
	}
	cmd[len] = '\0';
	if (!len) {
		strcpy(cmd, "help");
	}

	tmp = File_OpenTempFile(&tmpName);
	if (!tmp) {
		OutError(1);
		return;
	}
#ifdef _WIN32
	/* a GUI program without console has no file descriptors
	 * behind stdout & stderr, so give them one to redirect */
	if (fileno(stdout) < 0)
		freopen("NUL", "w", stdout);
	if (fileno(stderr) < 0)
		freopen("NUL", "w", stderr);
#endif
	/* capture both stdout & stderr (and debugOutput when it is one of them) */
	fflush(stdout);
	fflush(stderr);
	outFd = fileno(stdout);
	errFd = fileno(stderr);
	save1 = dup(outFd);
	save2 = dup(errFd);
	dup2(fileno(tmp), outFd);
	dup2(fileno(tmp), errFd);

	DebugUI_ParseLine(cmd);

	fflush(stdout);
	fflush(stderr);
	if (debugOutput)
		fflush(debugOutput);
	if (save1 >= 0) {
		dup2(save1, outFd);
		close(save1);
	}
	if (save2 >= 0) {
		dup2(save2, errFd);
		close(save2);
	}

	rewind(tmp);
	while ((n = fread(buf, 1, sizeof(buf), tmp)) > 0) {
		OutStr("O");
		OutHexBytes((uint8_t *)buf, n);
		SendPacket();
	}
	fclose(tmp);
	if (tmpName)
		remove(tmpName);
	OutStr("OK");
}


/* ------------------------------------------------------------------ */
/* packet handling */

static bool StartsWith(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

/**
 * Handle qXfer:<object>:read:<annex>:<offset>,<length> for given data
 */
static void XferReply(const char *args, const char *data, size_t datalen)
{
	uint32_t offset, len;

	if (!ParseAddrLen(args, &offset, &len, NULL)) {
		OutError(1);
		return;
	}
	if (offset >= datalen) {
		OutStr("l");
		return;
	}
	if (len > datalen - offset) {
		len = datalen - offset;
		OutStr("l");
	} else {
		OutStr(offset + len >= datalen ? "l" : "m");
	}
	OutData(data + offset, len);
}

static bool replySent;	/* handler sent the reply itself */

static void CmdQuery(void)
{
	const char *p;

	if (StartsWith(pkt, "qSupported")) {
		OutPrintf("PacketSize=%x;QStartNoAckMode+;swbreak+;hwbreak+;"
			  "qXfer:features:read+;qXfer:exec-file:read+;binary-upload+",
			  GDB_PACKET_SIZE);
		extendedMode = false;
	} else if (strcmp(pkt, "QStartNoAckMode") == 0) {
		/* reply is still acked, mode is changed after it */
		OutStr("OK");
		SendPacket();
		noAckMode = true;
		replySent = true;
		return;
	} else if (strcmp(pkt, "qAttached") == 0) {
		/* attached to existing "process" unless it was started by vRun */
		OutStr(prog.valid ? "0" : "1");
	} else if (strcmp(pkt, "qC") == 0) {
		OutStr("QC1");
	} else if (strcmp(pkt, "qfThreadInfo") == 0) {
		OutStr(attached ? "m1" : "l");
	} else if (strcmp(pkt, "qsThreadInfo") == 0) {
		OutStr("l");
	} else if (strcmp(pkt, "qOffsets") == 0) {
		prog_info_t info;
		if (GetProgramInfo(&info)) {
			uint32_t textoff, dataoff;
			GetSectionOffsets(&info, &textoff, &dataoff);
			OutPrintf("TextSeg=%x;DataSeg=%x", textoff, dataoff);
		}
	} else if (StartsWith(pkt, "qSymbol")) {
		OutStr("OK");
	} else if (StartsWith(pkt, "qXfer:features:read:target.xml:")) {
		char *xml;
		size_t len;
		BuildTargetXml(&xml, &len);
		XferReply(pkt + strlen("qXfer:features:read:target.xml:"), xml, len);
	} else if (StartsWith(pkt, "qXfer:exec-file:read:")) {
		const char *name = ExecFileName();
		p = strchr(pkt + strlen("qXfer:exec-file:read:"), ':');
		if (!p)
			OutError(1);
		else
			XferReply(p + 1, name, strlen(name));
	} else if (StartsWith(pkt, "qRcmd,")) {
		CmdMonitor(pkt + strlen("qRcmd,"));
	}
	/* everything else is unsupported (empty reply) */
}

/**
 * vFile operations for the generated ELF file
 */
static void CmdFile(void)
{
	const char *p = pkt + strlen("vFile:");
	uint32_t fd, count, offset;

	if (StartsWith(p, "setfs:")) {
		OutStr("F0");
	} else if (StartsWith(p, "open:")) {
		char name[FILENAME_MAX];
		const char *comma = strchr(p + 5, ',');
		int len;

		if (!comma) {
			OutStr("F-1,16");	/* EINVAL */
			return;
		}
		len = HexDecode(p + 5, comma - (p + 5), (uint8_t *)name, sizeof(name) - 1);
		if (len < 0) {
			OutStr("F-1,16");
			return;
		}
		name[len] = '\0';
		if (strcmp(name, ExecFileName()) != 0) {
			OutStr("F-1,2");	/* ENOENT */
			return;
		}
		BuildElf();
		elfOpen = true;
		OutPrintf("F%x", GDB_ELF_FD);
	} else if (StartsWith(p, "close:")) {
		p += 6;
		if (ParseHex(&p, &fd) && fd == GDB_ELF_FD && elfOpen) {
			elfOpen = false;
			OutStr("F0");
		} else {
			OutStr("F-1,9");	/* EBADF */
		}
	} else if (StartsWith(p, "pread:")) {
		p += 6;
		if (!ParseHex(&p, &fd) || *p++ != ',' || !ParseHex(&p, &count) ||
		    *p++ != ',' || !ParseHex(&p, &offset) || fd != GDB_ELF_FD || !elfOpen) {
			OutStr("F-1,9");
			return;
		}
		if (offset > elfLen)
			offset = elfLen;
		if (count > elfLen - offset)
			count = elfLen - offset;
		if (count > GDB_PACKET_SIZE)
			count = GDB_PACKET_SIZE;
		OutPrintf("F%x;", count);
		OutData(elfBuf + offset, count);
	} else if (StartsWith(p, "fstat:")) {
		uint8_t st[64];
		p += 6;
		if (!ParseHex(&p, &fd) || fd != GDB_ELF_FD || !elfOpen) {
			OutStr("F-1,9");
			return;
		}
		memset(st, 0, sizeof(st));
		PutBE32(st + 8, 0100444);	/* st_mode: regular, read-only */
		PutBE32(st + 12, 1);		/* st_nlink */
		PutBE32(st + 32, elfLen);	/* st_size (low part of 64-bit value) */
		PutBE32(st + 40, 512);		/* st_blksize */
		PutBE32(st + 48, (elfLen + 511) / 512);	/* st_blocks */
		OutPrintf("F%x;", (unsigned)sizeof(st));
		OutData(st, sizeof(st));
	}
	/* other vFile operations are unsupported */
}

/**
 * Resume emulation for continue / step.  With non-zero range end,
 * stepping continues while PC is within [start, end).
 */
static gdb_action_t Resume(bool step, uint32_t start, uint32_t end)
{
	if (!attached) {
		OutError(1);
		return ACT_NONE;
	}
	rangeStart = start;
	rangeEnd = step ? end : 0;
	rangeSteps = 0;
	DebugCpu_SetStepCount(step ? 1 : 0);
	stopReplyPending = true;
	return ACT_RESUME;
}

/* c [addr], s [addr], C sig[;addr], S sig[;addr] */
static gdb_action_t CmdContinue(bool step)
{
	const char *p = pkt + 1;
	uint32_t addr;

	if (pkt[0] == 'C' || pkt[0] == 'S') {
		/* signal can't be delivered, skip it */
		while (*p && *p != ';')
			p++;
		if (*p == ';')
			p++;
	}
	if (attached && ParseHex(&p, &addr))
		M68000_SetPC(addr);
	return Resume(step, 0, 0);
}

/* vCont;action[:thread][;action[:thread]]... uses first action */
static gdb_action_t CmdVCont(void)
{
	const char *p = pkt + strlen("vCont;");
	uint32_t start, end;

	switch (*p) {
	case 'c':
	case 'C':
		return Resume(false, 0, 0);
	case 's':
	case 'S':
		return Resume(true, 0, 0);
	case 'r':
		p++;
		if (!ParseHex(&p, &start) || *p++ != ',' || !ParseHex(&p, &end)) {
			OutError(1);
			return ACT_NONE;
		}
		return Resume(true, start, end);
	case 't':
		/* stop: already stopped */
		OutStr("OK");
		return ACT_NONE;
	default:
		OutError(1);
		return ACT_NONE;
	}
}

/* vRun;filename[;args...]: wait for given program to start */
static gdb_action_t CmdRun(void)
{
	const char *p = pkt + strlen("vRun;");
	const char *end = strchr(p, ';');
	int len;

	if (!end)
		end = p + strlen(p);
	len = HexDecode(p, end - p, (uint8_t *)waitProgramName, sizeof(waitProgramName) - 1);
	if (len < 0) {
		OutError(1);
		return ACT_NONE;
	}
	waitProgramName[len] = '\0';
	if (attached)
		Release();
	waitForProgram = true;
	stopReplyPending = true;
	if (waitProgramName[0])
		fprintf(stderr, "GDB server: waiting for program '%s' to start...\n", waitProgramName);
	else
		fprintf(stderr, "GDB server: waiting for a program to start...\n");
	return ACT_RESUME;
}

static gdb_action_t CmdBreakpoint(bool insert)
{
	const char *p = pkt + 1;
	uint32_t type, addr, len;
	bool ok;

	if (!ParseHex(&p, &type) || *p++ != ',' || !ParseAddrLen(p, &addr, &len, NULL)) {
		OutError(1);
		return ACT_NONE;
	}
	if (type > BP_WATCH)
		return ACT_NONE;	/* read & access watchpoints are unsupported */

	if (insert)
		ok = InsertBreak(type, addr, len);
	else
		ok = RemoveBreak(type, addr, len);
	if (ok) {
		OutStr("OK");
		DebugCpu_SetDebugging();
	} else {
		OutError(1);
	}
	return ACT_NONE;
}

/**
 * Handle packet in pkt.  Reply is sent unless the packet has no reply.
 */
static gdb_action_t HandlePacket(void)
{
	gdb_action_t act = ACT_NONE;

	OutClear();
	replySent = false;
	switch (pkt[0]) {
	case '!':
		extendedMode = true;
		OutStr("OK");
		break;
	case '?':
		OutStopReply();
		break;
	case 'c':
	case 'C':
		act = CmdContinue(false);
		break;
	case 's':
	case 'S':
		act = CmdContinue(true);
		break;
	case 'D':
		OutStr("OK");
		SendPacket();
		if (extendedMode) {
			/* gdb stays connected, emulation continues */
			Release();
			return ACT_RESUME;
		}
		return ACT_DETACH;
	case 'k':
		if (extendedMode) {
			Release();
			return ACT_RESUME;
		}
		return ACT_DETACH;
	case 'g':
		CmdReadRegisters();
		break;
	case 'G':
		CmdWriteRegisters();
		break;
	case 'p':
		CmdReadRegister();
		break;
	case 'P':
		CmdWriteRegister();
		break;
	case 'm':
		CmdReadMemory(false);
		break;
	case 'x':
		CmdReadMemory(true);
		break;
	case 'M':
		CmdWriteMemory(false);
		break;
	case 'X':
		CmdWriteMemory(true);
		break;
	case 'H':
	case 'T':
		OutStr("OK");
		break;
	case 'q':
	case 'Q':
		CmdQuery();
		break;
	case 'z':
	case 'Z':
		act = CmdBreakpoint(pkt[0] == 'Z');
		break;
	case 'v':
		if (strcmp(pkt, "vCont?") == 0) {
			OutStr("vCont;c;C;s;S;t;r");
		} else if (StartsWith(pkt, "vCont;")) {
			act = CmdVCont();
		} else if (StartsWith(pkt, "vRun;")) {
			act = CmdRun();
		} else if (StartsWith(pkt, "vAttach;")) {
			if (!attached) {
				Attach();
				return ACT_ATTACH;
			}
			OutError(1);
		} else if (StartsWith(pkt, "vKill")) {
			OutStr("OK");
			if (attached || waitForProgram) {
				Release();
				act = ACT_RESUME;
			}
		} else if (StartsWith(pkt, "vFile:")) {
			CmdFile();
		}
		/* e.g. vMustReplyEmpty gets an empty reply */
		break;
	default:
		break;
	}
	if (!replySent && (act != ACT_RESUME || outLen))
		SendPacket();
	return act;
}


/* ------------------------------------------------------------------ */
/* stopping */

/**
 * Emulation is stopped for gdb: report it (if gdb waits for that) and
 * process gdb commands until gdb resumes emulation or disconnects.
 */
static void StopLoop(int signal, bool reply)
{
	bool paused;

	stopped = true;
	lastSignal = signal;
	stopReplyPending = false;
	waitForProgram = false;
	rangeStart = rangeEnd = 0;
	DebugCpu_SetStepCount(0);

	Symbols_AutoLoadCurrentProgram(NULL, 0);
	UpdateWatchValues();

	if (bInFullScreen)
		Screen_ReturnFromFullScreen();
	Screen_UngrabMouse();
	paused = Main_PauseEmulation(false);
	Screen_StatusbarMessage("GDB debugging", 100);

	if (reply) {
		OutClear();
		OutStopReply();
		SendPacket();
	}
	while (clientFd != SOCK_NONE) {
		int ret = GetPacket();
		if (ret < 0) {
			CloseClient();
			break;
		}
		if (ret == 0) {
			/* interrupt request while already stopped */
			ctrlC = false;
			continue;
		}
		ret = HandlePacket();
		if (ret == ACT_RESUME)
			break;
		if (ret == ACT_DETACH) {
			CloseClient();
			break;
		}
	}
	stopped = false;
	faultPcValid = false;
	DebugCpu_SetDebugging();
	if (paused)
		Main_UnPauseEmulation();
}

/**
 * Stop emulation for gdb for other reasons than a debugger invocation
 */
static void StopNow(int signal, bool reply)
{
	stopKind = STOP_SIGNAL;
	StopLoop(signal, reply);
}

/**
 * Called from the debugger entry.  Returns true if the debugger
 * invocation was for gdb (and it has been handled).
 */
bool GdbServer_DebugUI(debug_reason_t reason)
{
	uint32_t pc;
	int signal;

	if (clientFd == SOCK_NONE || !attached || stopped)
		return false;

	pc = M68000_GetPC();
	stopKind = STOP_SIGNAL;

	switch (reason) {
	case REASON_CPU_STEPS:
		if (rangeEnd && pc >= rangeStart && pc < rangeEnd) {
			/* range stepping, check for interrupt now and then */
			if ((++rangeSteps & 0xfff) == 0) {
				if (ReadInput(0) < 0) {
					CloseClient();
					return true;
				}
				ScanInterrupt();
			}
			if (!ctrlC) {
				DebugCpu_SetStepCount(1);
				return true;
			}
			ctrlC = false;
			signal = GDB_SIGINT;
		} else {
			signal = GDB_SIGTRAP;
		}
		break;
	case REASON_CPU_BREAKPOINT:
		signal = GDB_SIGTRAP;
		if (startBreak && pc == startBreak) {
			RemoveStartBreak();
		} else {
			ClassifyBreak(pc);
		}
		break;
	case REASON_CPU_EXCEPTION:
		signal = ExceptionSignal(pendingException);
		/* PC has advanced past the instruction that caused
		 * bus / address error, show the instruction instead
		 */
		if ((pendingException == 2 || pendingException == 3) && regs.instruction_pc) {
			faultPc = regs.instruction_pc;
			faultPcValid = true;
		}
		break;
	case REASON_USER:
		signal = GDB_SIGINT;
		break;
	case REASON_PROGRAM:
		signal = GDB_SIGTRAP;
		break;
	default:
		/* DSP stops are for the console debugger */
		return false;
	}
	pendingException = 0;
	if (startBreak && !stopReplyPending) {
		/* gdb connected while waiting for program start,
		 * it will ask for stop reason itself
		 */
		RemoveStartBreak();
	}
	StopLoop(signal, stopReplyPending);
	return true;
}

/**
 * Store exception number for the following debugger invocation
 */
void GdbServer_NoteException(int nr)
{
	pendingException = nr;
}


/* ------------------------------------------------------------------ */
/* polling from the main loop */

void GdbServer_Poll(void)
{
	if (listenFd == SOCK_NONE || stopped)
		return;

	if (clientFd == SOCK_NONE) {
		if (!AcceptClient(0))
			return;
		/* stop emulation for gdb, gdb asks the stop reason */
		StopNow(GDB_SIGTRAP, false);
		return;
	}
	if (ReadInput(0) < 0) {
		CloseClient();
		return;
	}
	if (attached) {
		ScanInterrupt();
		if (ctrlC) {
			ctrlC = false;
			if (stopReplyPending)
				StopNow(GDB_SIGINT, true);
		}
		/* any packets are processed when stopped */
		return;
	}

	/* gdb connected, but no "process" (program exited or was killed) */
	if (ctrlC) {
		ctrlC = false;
		if (waitForProgram) {
			/* interrupt waiting for program, stop where we are */
			Attach();
			StopNow(GDB_SIGINT, true);
			return;
		}
	}
	while (clientFd != SOCK_NONE && !attached && !stopped && ParsePacket()) {
		gdb_action_t act = HandlePacket();
		if (act == ACT_DETACH) {
			CloseClient();
		} else if (act == ACT_ATTACH) {
			StopNow(GDB_SIGTRAP, true);
		}
	}
}

bool GdbServer_IsListening(void)
{
	return listenFd != SOCK_NONE;
}


/* ------------------------------------------------------------------ */
/* program start / exit */

/**
 * Called when a program has been loaded through GEMDOS HD and is
 * about to be started.
 */
void GdbServer_ProgramStart(const char *path, uint32_t basepage)
{
	char expr[64];

	if (listenFd == SOCK_NONE || stopped)
		return;
	if (clientFd != SOCK_NONE) {
		if (!waitForProgram || !ProgramNameMatches(path, waitProgramName))
			return;
	} else if (!waitAtStart) {
		return;
	}
	if (!ReadProgramInfo(basepage, path, &prog))
		return;

	/* stop at the first program instruction */
	RemoveStartBreak();
	BreakExpression(expr, sizeof(expr), BP_SW, prog.text, 0);
	if (!AddHatariBreak(expr)) {
		prog.valid = false;
		return;
	}
	startBreak = prog.text;

	if (clientFd == SOCK_NONE) {
		fprintf(stderr, "GDB server: program '%s' started, waiting for gdb to connect to %s...\n",
			path, listenAddr);
		if (!AcceptClient(-1)) {
			RemoveStartBreak();
			prog.valid = false;
			DebugCpu_SetDebugging();
			return;
		}
	} else {
		fprintf(stderr, "GDB server: program '%s' started.\n", path);
		Attach();
	}
	waitForProgram = false;
	DebugCpu_SetDebugging();
}

/**
 * Called when a program started from GEMDOS HD terminates
 */
void GdbServer_ProgramExit(uint32_t basepage, int exitcode)
{
	if (!prog.valid || basepage != prog.basepage)
		return;
	prog.valid = false;
	if (clientFd == SOCK_NONE || !attached)
		return;

	fprintf(stderr, "GDB server: program exited with code %d.\n", exitcode);
	Release();
	OutClear();
	OutPrintf("W%02x", exitcode & 0xff);
	SendPacket();
	if (!extendedMode)
		CloseClient();
}


/* ------------------------------------------------------------------ */
/* start / stop */

/**
 * Start listening for gdb connections on given "[host:]port".
 * Return NULL on success, error string otherwise.
 */
const char *GdbServer_Start(const char *address)
{
	struct addrinfo hints, *res, *ai;
	char host[64], *colon;
	const char *port;
	sock_t fd = SOCK_NONE;
#ifndef _WIN32
	int one = 1;
#endif

	if (strlen(address) >= sizeof(host))
		return "too long GDB server address";
	strcpy(host, address);
	colon = strrchr(host, ':');
	if (colon) {
		*colon = '\0';
		port = colon + 1;
	} else {
		port = host;
	}
	if (!*port || strspn(port, "0123456789") != strlen(port))
		return "invalid GDB server port";

	GdbServer_Stop();
	if (strcmp(port, "0") == 0)
		return NULL;	/* disabled */
	if (!SockInit())
		return "can't initialize sockets";

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	/* only local connections, unless a host is given */
	if (getaddrinfo(colon ? (host[0] ? host : NULL) : "localhost", port, &hints, &res) != 0)
		return "can't resolve GDB server address";

	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == SOCK_NONE)
			continue;
#ifndef _WIN32
		/* on Windows, SO_REUSEADDR would let others steal the port */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&one, sizeof(one));
#endif
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 1) == 0)
			break;
		SockClose(fd);
		fd = SOCK_NONE;
	}
	freeaddrinfo(res);
	if (fd == SOCK_NONE)
		return "can't listen on GDB server port";

#ifdef _WIN32
	SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
	listenFd = fd;
	snprintf(listenAddr, sizeof(listenAddr), "%s:%s",
		 colon ? (host[0] ? host : "*") : "localhost", port);
	fprintf(stderr, "GDB server: listening on %s.\n", listenAddr);
	return NULL;
}

void GdbServer_Stop(void)
{
	if (clientFd != SOCK_NONE) {
		if (attached && stopReplyPending) {
			/* tell gdb that the "process" is gone */
			OutClear();
			OutStr("X09");
			SendPacket();
		}
		CloseClient();
	}
	if (listenFd != SOCK_NONE) {
		SockClose(listenFd);
		listenFd = SOCK_NONE;
		fprintf(stderr, "GDB server: stopped.\n");
	}
}

void GdbServer_SetWaitAtProgramStart(bool wait)
{
	waitAtStart = wait;
}

void GdbServer_ShowStatus(FILE *fp)
{
	if (listenFd == SOCK_NONE) {
		fputs("GDB server is not running.\n", fp);
		return;
	}
	fprintf(fp, "GDB server listening on %s, %s.\n", listenAddr,
		clientFd != SOCK_NONE ? "gdb connected" : "no connection");
	fprintf(fp, "Wait for gdb at program start: %s.\n", waitAtStart ? "on" : "off");
	fprintf(fp, "gdb breakpoints/watchpoints: %d.\n", bpCount);
}

void GdbServer_UnInit(void)
{
	GdbServer_Stop();
	SockExit();
	free(inBuf);
	free(outBuf);
	free(lastSent);
	free(bps);
	free(elfBuf);
	inBuf = outBuf = lastSent = elfBuf = NULL;
	bps = NULL;
	inSize = outSize = 0;
	bpAlloc = bpCount = 0;
}

#endif /* HAVE_GDBSERVER */
