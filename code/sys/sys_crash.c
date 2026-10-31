/*
===========================================================================
Crash reports: logs/crash-<timestamp>.txt with the signal or fatal error,
a backtrace, the last 200 console lines, the current map and mode, and
OS/CPU/GPU info. Written from Sys_SigHandler and Sys_ErrorDialog.

Runs inside a signal handler, so it sticks to write(2) and static buffers
where it can. snprintf/localtime are not strictly async-signal-safe but are
fine in practice for a crash report.

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../qcommon/dev.h"
#include "sys_local.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>
#if defined( __APPLE__ ) || defined( __linux__ )
#include <execinfo.h>
#define CRASH_HAVE_BACKTRACE
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

#define CRASH_LOG_LINES 200

#ifndef _WIN32

static char crashLogBuf[ 32768 ];

static void Crash_Write( int fd, const char *fmt, ... ) Q_PRINTF_FUNC( 2, 3 );

static void Crash_Write( int fd, const char *fmt, ... ) {
	char buf[ 1024 ];
	va_list ap;
	int len;

	va_start( ap, fmt );
	len = Q_vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );

	if ( len > 0 ) {
		if ( len >= (int)sizeof( buf ) ) {
			len = sizeof( buf ) - 1;
		}
		if ( write( fd, buf, len ) < 0 ) {
			// nothing useful to do while crashing
		}
	}
}

static void Crash_WriteCpu( int fd ) {
#ifdef __APPLE__
	char brand[ 256 ];
	size_t size = sizeof( brand );

	if ( sysctlbyname( "machdep.cpu.brand_string", brand, &size, NULL, 0 ) == 0 ) {
		Crash_Write( fd, "cpu: %s\n", brand );
		return;
	}
#elif defined( __linux__ )
	FILE *f = fopen( "/proc/cpuinfo", "r" );
	char line[ 256 ];

	if ( f ) {
		while ( fgets( line, sizeof( line ), f ) ) {
			if ( !strncmp( line, "model name", 10 ) ) {
				const char *colon = strchr( line, ':' );
				Crash_Write( fd, "cpu:%s", colon ? colon + 1 : line );
				fclose( f );
				return;
			}
		}
		fclose( f );
	}
#endif
	Crash_Write( fd, "cpu: unknown\n" );
}

static void Crash_WriteLogTail( int fd ) {
	// peek: Sys_ErrorDialog still copies the same tail into crashlog.txt afterwards
	unsigned int size = CON_LogPeek( crashLogBuf, sizeof( crashLogBuf ) - 1 );
	const char *start;
	int lines = 0;

	crashLogBuf[ size ] = '\0';

	// walk back CRASH_LOG_LINES newlines from the end
	start = crashLogBuf + size;
	while ( start > crashLogBuf ) {
		if ( start[ -1 ] == '\n' && ++lines > CRASH_LOG_LINES ) {
			break;
		}
		start--;
	}

	Crash_Write( fd, "\n---- last %d log lines ----\n", CRASH_LOG_LINES );
	if ( write( fd, start, strlen( start ) ) < 0 ) {
	}
}

/*
=================
Sys_WriteCrashReport

sig is the signal number, or 0 for a fatal error described by reason.
Returns the report path, or NULL if it couldn't be written.
=================
*/
const char *Sys_WriteCrashReport( int sig, const char *reason ) {
	static char path[ MAX_OSPATH ];
	char stamp[ 32 ];
	struct utsname uts;
	time_t now = time( NULL );
	struct tm tmNow;
	int fd;

	localtime_r( &now, &tmNow );
	strftime( stamp, sizeof( stamp ), "%Y%m%d-%H%M%S", &tmNow );

	Sys_Mkdir( Dev_LogDir() );
	Com_sprintf( path, sizeof( path ), "%s/crash-%s.txt", Dev_LogDir(), stamp );

	fd = open( path, O_CREAT | O_TRUNC | O_WRONLY, 0644 );
	if ( fd < 0 ) {
		return NULL;
	}

	Crash_Write( fd, "Skaar crash report\n" );
	if ( sig ) {
		Crash_Write( fd, "signal: %d (%s)\n", sig, strsignal( sig ) );
	} else {
		Crash_Write( fd, "fatal error: %s\n", reason ? reason : "unknown" );
	}
	Crash_Write( fd, "time: %s\n", stamp );
	Crash_Write( fd, "version: %s %s %s\n", Q3_VERSION, PLATFORM_STRING, PRODUCT_DATE );
	Crash_Write( fd, "map: %s\n", Cvar_VariableString( "mapname" ) );
	Crash_Write( fd, "mode: gametype %s, ruleset %s\n", Cvar_VariableString( "g_gametype" ),
		Cvar_VariableString( "skaar_ruleset" )[ 0 ] ? Cvar_VariableString( "skaar_ruleset" ) : "(default)" );
	if ( uname( &uts ) == 0 ) {
		Crash_Write( fd, "os: %s %s %s\n", uts.sysname, uts.release, uts.machine );
	}
	Crash_WriteCpu( fd );
	Crash_Write( fd, "gpu: %s\n", Dev_GpuInfo() );
	Crash_Write( fd, "session log: %s\n", Dev_LogPath() );

#ifdef CRASH_HAVE_BACKTRACE
	{
		void *frames[ 64 ];
		int n = backtrace( frames, ARRAY_LEN( frames ) );

		Crash_Write( fd, "\n---- backtrace ----\n" );
		backtrace_symbols_fd( frames, n, fd );
	}
#endif

	Crash_WriteLogTail( fd );
	close( fd );

	fprintf( stderr, "FATAL: crash report written to %s\n", path );
	return path;
}

#else

const char *Sys_WriteCrashReport( int sig, const char *reason ) {
	// TODO: Windows crash reports (MiniDumpWriteDump + StackWalk64)
	(void)sig;
	(void)reason;
	return NULL;
}

#endif
