/*
===========================================================================
Skaar developer diagnostics (engine side): warning/error capture for the
in-game toasts, frame spike and tick backlog logging, dev_crash, dev_reload,
and last-session tracking for scripts/dev-watch.sh. See docs/dev-loop.md.

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#include "q_shared.h"
#include "qcommon.h"
#include "dev.h"
#include "dev_assert.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define MAX_DEV_NOTES		32
#define SPIKE_AVG_WEIGHT	0.05f	// EMA weight of a new frame in the running average
#define SPIKE_LOG_GAP_MSEC	250		// at most one spike line per this many msec
#define BACKLOG_LOG_GAP_MSEC 1000

static cvar_t	*dev_logdir;
static cvar_t	*dev_logpath;
static cvar_t	*dev_spikeMinMs;
static cvar_t	*dev_maxTickBacklog;
static cvar_t	*dev_toastSpikes;

static devNote_t	notes[ MAX_DEV_NOTES ];
static int			notesHead;		// next write slot
static int			notesCount;

static char			lastError[ 192 ];
static int			lastErrorTime;
static int			lastMapLoadTime;

static char			gpuInfo[ 256 ] = "n/a";

// frame timing
static long long	lastFrameEndUsec;
static float		avgWorkUsec;
static float		avgIntervalUsec;
static int			suppressSpikeFrames = 120;
static int			lastSpikeLogTime;
static int			fpsWindowStart;
static int			fpsWindowFrames;
static float		fpsMeasured;

// frame interval history for percentiles
#define FRAME_RING 2048
static int			frameRing[ FRAME_RING ];	// intervals, usec
static int			frameRingHead;
static int			frameRingCount;

// input age
static long long	pendingInputUsec;
static float		inputAgeAvgUsec;
static int			inputAgeMaxUsec, inputAgeMaxWindowUsec;
static int			inputWindowStart;
static char			mouseSource[ 64 ] = "SDL";

// server ticks
static int			tickWindowStart;
static int			tickWindowTicks;
static float		tickRateMeasured;
static int			lastTickRunTime;
static int			ticksThisFrame;
static int			lastBacklogLogTime;

/*
=================
Dev_Microseconds
=================
*/
long long Dev_Microseconds( void ) {
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;

	if ( !freq.QuadPart ) {
		if ( !QueryPerformanceFrequency( &freq ) || !freq.QuadPart ) {
			// no usable counter: callers compare this clock with itself, so 0
			// is better than a division by zero
			return 0;
		}
	}
	QueryPerformanceCounter( &now );
	// split the multiply: QPF is the TSC rate (GHz) on some systems, where
	// now * 1000000 overflows int64 after an hour of uptime
	return ( now.QuadPart / freq.QuadPart ) * 1000000LL +
		( now.QuadPart % freq.QuadPart ) * 1000000LL / freq.QuadPart;
#else
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

/*
=================
Notes (toast feed)
=================
*/
static void Dev_StripColors( const char *in, char *out, int outSize ) {
	int n = 0;

	while ( *in && n < outSize - 1 ) {
		if ( Q_IsColorString( in ) ) {
			in += 2;
			continue;
		}
		if ( *in == '\r' || *in == '\n' ) {
			break;
		}
		out[ n++ ] = *in++;
	}
	out[ n ] = '\0';
}

static void Dev_AddNote( devNoteLevel_t level, const char *text ) {
	devNote_t *note = &notes[ notesHead ];

	note->time = Sys_Milliseconds();
	note->level = level;
	Q_strncpyz( note->text, text, sizeof( note->text ) );

	notesHead = ( notesHead + 1 ) % MAX_DEV_NOTES;
	if ( notesCount < MAX_DEV_NOTES ) {
		notesCount++;
	}
}

static void Dev_ClassifyLine( const char *line ) {
	char clean[ 192 ];
	const char *text;

	Dev_StripColors( line, clean, sizeof( clean ) );

	text = clean;
	while ( *text == ' ' || *text == '*' ) {
		text++;
	}
	if ( !*text ) {
		return;
	}

	if ( strstr( text, "ERROR" ) || !strncmp( text, "Error:", 6 ) || strstr( text, "FATAL" ) || !strncmp( text, "ASSERT ", 7 ) ||
		strstr( text, "Sanitizer" ) || strstr( text, "runtime error:" ) ) {
		Dev_AddNote( DEV_NOTE_ERROR, text );

		if ( !strncmp( text, "ERROR: ", 7 ) ) {
			Q_strncpyz( lastError, text + 7, sizeof( lastError ) );
			lastErrorTime = Sys_Milliseconds();
		}
	} else if ( strstr( text, "WARNING" ) ||
		( !strncmp( text, "spike:", 6 ) && dev_toastSpikes && dev_toastSpikes->integer ) ) {
		// spikes stay in the log and watch.sh unless asked for: in toasts they
		// crowd out the warnings that matter
		Dev_AddNote( DEV_NOTE_WARNING, text );
	}
}

void Dev_OnPrint( const char *msg ) {
	const char *line = msg;

	// A single print can hold several lines (ERR_DROP prints a framed block)
	while ( line && *line ) {
		const char *next = strchr( line, '\n' );

		Dev_ClassifyLine( line );
		line = next ? next + 1 : NULL;
	}
}

int Dev_RecentNotes( devNote_t *out, int max, int maxAgeMsec ) {
	int now = Sys_Milliseconds();
	int i, n = 0;
	int start = notesCount < max ? notesCount : max;

	// walk from the oldest of the newest `start` notes forward
	for ( i = start; i > 0; i-- ) {
		const devNote_t *note = &notes[ ( notesHead - i + MAX_DEV_NOTES ) % MAX_DEV_NOTES ];

		if ( maxAgeMsec > 0 && now - note->time > maxAgeMsec ) {
			continue;
		}
		out[ n++ ] = *note;
	}
	return n;
}

const char *Dev_RecentError( int maxAgeMsec ) {
	if ( !lastError[ 0 ] || lastErrorTime < lastMapLoadTime ) {
		return NULL;
	}
	if ( Sys_Milliseconds() - lastErrorTime > maxAgeMsec ) {
		return NULL;
	}
	return lastError;
}

/*
=================
Frame and tick diagnostics
=================
*/
void Dev_SuppressSpikes( int frames ) {
	if ( frames > suppressSpikeFrames ) {
		suppressSpikeFrames = frames;
	}
}

void Dev_FrameEnd( int eventsUsec, int serverUsec, int clientUsec ) {
	long long now = Dev_Microseconds();
	int nowMsec = Sys_Milliseconds();
	int workUsec = eventsUsec + serverUsec + clientUsec;
	int intervalUsec = lastFrameEndUsec ? (int)( now - lastFrameEndUsec ) : workUsec;

	lastFrameEndUsec = now;

	frameRing[ frameRingHead ] = intervalUsec;
	frameRingHead = ( frameRingHead + 1 ) % FRAME_RING;
	if ( frameRingCount < FRAME_RING ) {
		frameRingCount++;
	}

	// input age: newest mouse motion this frame used -> now (after the swap)
	if ( pendingInputUsec ) {
		int age = (int)( now - pendingInputUsec );
		if ( age >= 0 && age < 1000000 ) {
			inputAgeAvgUsec = inputAgeAvgUsec ? inputAgeAvgUsec + ( age - inputAgeAvgUsec ) * 0.1f : age;
			if ( age > inputAgeMaxWindowUsec ) {
				inputAgeMaxWindowUsec = age;
			}
		}
		pendingInputUsec = 0;
	}
	if ( nowMsec - inputWindowStart >= 1000 ) {
		inputAgeMaxUsec = inputAgeMaxWindowUsec;
		inputAgeMaxWindowUsec = 0;
		inputWindowStart = nowMsec;
	}

	// fps over a one second window
	fpsWindowFrames++;
	if ( nowMsec - fpsWindowStart >= 1000 ) {
		fpsMeasured = fpsWindowFrames * 1000.0f / ( nowMsec - fpsWindowStart );
		fpsWindowStart = nowMsec;
		fpsWindowFrames = 0;
	}

	if ( !avgWorkUsec ) {
		ticksThisFrame = 0;
		avgWorkUsec = workUsec;
		avgIntervalUsec = intervalUsec;
		return;
	}

	if ( suppressSpikeFrames > 0 ) {
		ticksThisFrame = 0;
		// loading, vid_restart etc: don't report, and don't let it skew the average
		suppressSpikeFrames--;
		avgIntervalUsec = intervalUsec;
		return;
	}

	if ( workUsec > 2.0f * avgWorkUsec && workUsec > dev_spikeMinMs->value * 1000.0f &&
		nowMsec - lastSpikeLogTime >= SPIKE_LOG_GAP_MSEC ) {
		lastSpikeLogTime = nowMsec;
		Com_Printf( S_COLOR_YELLOW "spike: frame %.1f ms (avg %.1f ms): events %.1f, server %.1f (%d ticks), client+render %.1f\n",
			workUsec / 1000.0f, avgWorkUsec / 1000.0f, eventsUsec / 1000.0f,
			serverUsec / 1000.0f, ticksThisFrame, clientUsec / 1000.0f );
	}

	avgWorkUsec += ( workUsec - avgWorkUsec ) * SPIKE_AVG_WEIGHT;
	ticksThisFrame = 0;
	avgIntervalUsec += ( intervalUsec - avgIntervalUsec ) * SPIKE_AVG_WEIGHT;
}

void Dev_ServerTicks( int ticksRun, int backlogMsec, int frameMsec ) {
	int now = Sys_Milliseconds();

	if ( ticksRun > 0 ) {
		lastTickRunTime = now;
	}
	ticksThisFrame += ticksRun;

	tickWindowTicks += ticksRun;
	if ( now - tickWindowStart >= 1000 ) {
		tickRateMeasured = tickWindowTicks * 1000.0f / ( now - tickWindowStart );
		tickWindowStart = now;
		tickWindowTicks = 0;
	}

	if ( frameMsec > 0 && suppressSpikeFrames <= 0 &&
		backlogMsec > dev_maxTickBacklog->integer * (long long)frameMsec &&
		now - lastBacklogLogTime >= BACKLOG_LOG_GAP_MSEC ) {
		lastBacklogLogTime = now;
		Com_Printf( S_COLOR_YELLOW "WARNING: server tick loop behind: %d ms backlog (%.1f ticks at %d ms/tick), ran %d ticks this frame\n",
			backlogMsec, (float)backlogMsec / frameMsec, frameMsec, ticksRun );
	}
}

void Dev_NoteInput( long long usec ) {
	if ( usec > pendingInputUsec ) {
		pendingInputUsec = usec;
	}
}

void Dev_SetMouseSource( const char *source ) {
	Q_strncpyz( mouseSource, source, sizeof( mouseSource ) );
}

static int Dev_CompareInt( const void *a, const void *b ) {
	return *(const int *)a - *(const int *)b;
}

// p50/p99 of the frame intervals covering the last second
static void Dev_FramePercentiles( float *p50, float *p99 ) {
	static int sorted[ FRAME_RING ];
	int n = 0, total = 0;

	while ( n < frameRingCount && total < 1000000 ) {
		int v = frameRing[ ( frameRingHead - 1 - n + FRAME_RING ) % FRAME_RING ];
		sorted[ n++ ] = v;
		total += v;
	}
	if ( !n ) {
		*p50 = *p99 = 0;
		return;
	}
	qsort( sorted, n, sizeof( sorted[0] ), Dev_CompareInt );
	*p50 = sorted[ n / 2 ] / 1000.0f;
	*p99 = sorted[ ( n * 99 ) / 100 < n ? ( n * 99 ) / 100 : n - 1 ] / 1000.0f;
}

void Dev_GetStats( devStats_t *stats ) {
	stats->fps = fpsMeasured;
	stats->frameMsec = avgIntervalUsec / 1000.0f;
	Dev_FramePercentiles( &stats->frameP50, &stats->frameP99 );
	stats->inputAgeMsec = inputAgeAvgUsec / 1000.0f;
	stats->inputAgeMaxMsec = inputAgeMaxUsec / 1000.0f;
	stats->mouseSource = mouseSource;
	// no ticks for two seconds means no local server is running
	stats->tickRate = Sys_Milliseconds() - lastTickRunTime < 2000 ? tickRateMeasured : 0.0f;
}

/*
=================
Paths and session info
=================
*/
const char *Dev_LogDir( void ) {
	static char dir[ MAX_OSPATH ];

	if ( dev_logdir && dev_logdir->string[ 0 ] ) {
		return dev_logdir->string;
	}
	Com_sprintf( dir, sizeof( dir ), "%s%clogs", Cvar_VariableString( "fs_homepath" ), PATH_SEP );
	return dir;
}

const char *Dev_LogPath( void ) {
	static char path[ MAX_OSPATH ];

	if ( dev_logpath && dev_logpath->string[ 0 ] ) {
		return dev_logpath->string;
	}
	Com_sprintf( path, sizeof( path ), "%s%c%s%cqconsole.log", Cvar_VariableString( "fs_homepath" ),
		PATH_SEP, FS_GetCurrentGameDir(), PATH_SEP );
	return path;
}

void Dev_SetGpuInfo( const char *info ) {
	Q_strncpyz( gpuInfo, info, sizeof( gpuInfo ) );
}

const char *Dev_GpuInfo( void ) {
	return gpuInfo;
}

void Dev_OnMapLoaded( const char *mapname ) {
	lastMapLoadTime = Sys_Milliseconds();
	Dev_SuppressSpikes( 60 );
	Com_DPrintf( "dev: map loaded: %s (gametype %s)\n", mapname, Cvar_VariableString( "g_gametype" ) );

#ifdef SKAAR_DEV
	{
		// Read by scripts/dev.sh to relaunch into the same map and mode
		char path[ MAX_OSPATH ];
		FILE *f;

		Sys_Mkdir( Dev_LogDir() );
		Com_sprintf( path, sizeof( path ), "%s%clast-session.cfg", Dev_LogDir(), PATH_SEP );
		f = fopen( path, "w" );
		if ( f ) {
			// always devmap: sv_cheats isn't updated until after SV_SpawnServer
			fprintf( f, "set g_gametype %s\n", Cvar_VariableString( "g_gametype" ) );
			fprintf( f, "devmap %s\n", mapname );
			fclose( f );
		}
	}
#endif
}

/*
=================
Commands
=================
*/
#ifdef SKAAR_DEV
static void Dev_Crash_f( void ) {
	const char *kind = Cmd_Argv( 1 );

	if ( !Q_stricmp( kind, "segv" ) ) {
		volatile int *p = NULL;
		Com_Printf( "dev_crash: writing to NULL\n" );
		*p = 1;
	} else if ( !Q_stricmp( kind, "abort" ) ) {
		abort();
	} else if ( !Q_stricmp( kind, "fpe" ) ) {
		raise( SIGFPE );	// integer division by zero doesn't trap on arm64
	} else if ( !Q_stricmp( kind, "assert" ) ) {
		DEV_ASSERT( Cmd_Argc() < 0, "dev_crash assert test" );
	} else if ( !Q_stricmp( kind, "warning" ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: dev_crash warning test\n" );
	} else if ( !Q_stricmp( kind, "drop" ) ) {
		Com_Error( ERR_DROP, "dev_crash drop test" );
	} else if ( !Q_stricmp( kind, "fatal" ) ) {
		Com_Error( ERR_FATAL, "dev_crash fatal test" );
	} else if ( !Q_stricmp( kind, "spike" ) ) {
		long long until = Dev_Microseconds() + 50000;
		while ( Dev_Microseconds() < until ) {
		}
	} else {
		Com_Printf( "usage: dev_crash <segv|abort|fpe|assert|warning|drop|fatal|spike>\n" );
	}
}
#endif

static qboolean Dev_HasExt( const char *path, const char *ext ) {
	return !Q_stricmp( COM_GetExtension( path ), ext );
}

/*
=================
Dev_Reload_f

dev_reload <file> [file...]
Sent by scripts/dev-watch.sh (through com_pipefile) when files under data/
change. Paths may be repo-relative ("data/base/default.cfg") or game-relative.
=================
*/
static void Dev_Reload_f( void ) {
	qboolean vidRestart = qfalse, sndRestart = qfalse, mapRestart = qfalse;
	char gameDirPrefix[ MAX_QPATH ];
	int i;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: dev_reload <file> [file...]\n" );
		return;
	}

	Com_sprintf( gameDirPrefix, sizeof( gameDirPrefix ), "data/%s/", FS_GetCurrentGameDir() );

	for ( i = 1; i < Cmd_Argc(); i++ ) {
		const char *file = Cmd_Argv( i );

		if ( !Q_stricmpn( file, gameDirPrefix, strlen( gameDirPrefix ) ) ) {
			file += strlen( gameDirPrefix );
		}

		if ( Dev_HasExt( file, "cfg" ) ) {
			Com_Printf( "dev_reload: exec %s\n", file );
			Cbuf_AddText( va( "exec %s\n", file ) );
		} else if ( Dev_HasExt( file, "bsp" ) ) {
			char name[ MAX_QPATH ];
			COM_StripExtension( COM_SkipPath( (char *)file ), name, sizeof( name ) );
			if ( !Q_stricmp( name, Cvar_VariableString( "mapname" ) ) ) {
				mapRestart = qtrue;
			}
			Com_Printf( "dev_reload: map changed: %s\n", file );
		} else if ( Dev_HasExt( file, "tga" ) || Dev_HasExt( file, "png" ) || Dev_HasExt( file, "jpg" ) ||
			Dev_HasExt( file, "shader" ) ) {
			vidRestart = qtrue;
			Com_Printf( "dev_reload: image/shader changed: %s\n", file );
		} else if ( Dev_HasExt( file, "wav" ) || Dev_HasExt( file, "ogg" ) || Dev_HasExt( file, "opus" ) ) {
			sndRestart = qtrue;
			Com_Printf( "dev_reload: sound changed: %s\n", file );
		} else {
			// scenarios/configs from P2 on: game modules register "skaar_reload"
			Com_Printf( "dev_reload: no reload handler for %s yet\n", file );
		}
	}

	if ( mapRestart ) {
		const char *cmd = Cvar_VariableIntegerValue( "sv_cheats" ) ? "devmap" : "map";
		Cbuf_AddText( va( "%s %s\n", cmd, Cvar_VariableString( "mapname" ) ) );
	} else if ( vidRestart ) {
		Cbuf_AddText( "vid_restart\n" );
	}
	if ( sndRestart ) {
		Cbuf_AddText( "snd_restart\n" );
	}
}

static void Dev_Stats_f( void ) {
	devStats_t stats;

	Dev_GetStats( &stats );
	Com_Printf( "dev_stats: fps %.1f, frame p50 %.2f ms, p99 %.2f ms, input->swap %.1f ms (max %.1f), tick %.1f/s, mouse %s\n",
		stats.fps, stats.frameP50, stats.frameP99, stats.inputAgeMsec, stats.inputAgeMaxMsec, stats.tickRate,
		stats.mouseSource );
}

static void Dev_Notes_f( void ) {
	devNote_t list[ MAX_DEV_NOTES ];
	int i, n = Dev_RecentNotes( list, MAX_DEV_NOTES, 0 );

	Com_Printf( "%d recent warnings/errors:\n", n );
	for ( i = 0; i < n; i++ ) {
		Com_Printf( "  %s\n", list[ i ].text );
	}
}

/*
=================
Dev_Init
=================
*/
void Dev_Init( void ) {
	dev_logdir = Cvar_Get( "dev_logdir", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( dev_logdir, "Directory for crash reports and last-session.cfg (default <homepath>/logs)" );
	dev_logpath = Cvar_Get( "dev_logpath", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( dev_logpath, "Session log path shown on error screens (set by scripts/dev.sh)" );
	dev_spikeMinMs = Cvar_Get( "dev_spikeMinMs", "12", 0 );
	Cvar_SetDescription( dev_spikeMinMs, "Frames must also exceed this many ms to be logged as a spike" );
	dev_maxTickBacklog = Cvar_Get( "dev_maxTickBacklog", "3", 0 );
	Cvar_CheckRange( dev_maxTickBacklog, 0, 3600, qtrue );
	Cvar_SetDescription( dev_maxTickBacklog, "Warn when the server tick loop is more than this many ticks behind" );

	dev_toastSpikes = Cvar_Get( "dev_toastSpikes", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( dev_toastSpikes, "Also show frame spike lines as toasts (they're always in the log)" );

	Cmd_AddCommand( "dev_reload", Dev_Reload_f );
	Cmd_AddCommand( "dev_notes", Dev_Notes_f );
	Cmd_AddCommand( "dev_stats", Dev_Stats_f );
#ifdef SKAAR_DEV
	Cmd_AddCommand( "dev_crash", Dev_Crash_f );
#endif
}
