/*
===========================================================================
Skaar developer overlay (client side), drawn by the engine so it works even
when a game module fails to load:
  - toasts: warnings/errors/asserts, small, in the bottom-right for 5 seconds
  - error panel: full-screen message after an ERR_DROP, with the log path,
    only when no ui module is loaded to show it
  - dev_overlay 1: FPS, frame time, tick rate, ping, entities, last warnings
  - sens_test: measure mouse counts per 360 to verify cm/360

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#include "client.h"
#include "../qcommon/dev.h"

#define TOAST_MSEC			5000
#define TOAST_FADE_MSEC		500
#define MAX_TOASTS			3
#define TOAST_SCALE			0.65f	// of the console's small char
#define ERROR_PANEL_MSEC	10000

static cvar_t	*dev_overlay;
static cvar_t	*dev_toasts;
static cvar_t	*sens_dpi;

static const vec4_t colorPanel		= { 0.05f, 0.06f, 0.08f, 0.78f };
static const vec4_t colorErrorBg	= { 0.25f, 0.02f, 0.03f, 0.92f };
static const vec4_t colorErrorText	= { 1.00f, 0.45f, 0.45f, 1.00f };
static const vec4_t colorWarnText	= { 1.00f, 0.85f, 0.35f, 1.00f };
static const vec4_t colorText		= { 0.90f, 0.92f, 0.95f, 1.00f };
static const vec4_t colorDim		= { 0.60f, 0.63f, 0.68f, 1.00f };

static struct {
	qboolean	active;
	double		counts;		// raw mouse x counts, signed
	double		degrees;	// yaw applied from the mouse, signed
} sensTest;

static void CL_DevFill( float x, float y, float w, float h, const float *color ) {
	re.SetColor( color );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}

static void CL_DevText( int x, int y, const char *text, const float *color, float alpha ) {
	vec4_t c;

	Vector4Copy( color, c );
	c[3] *= alpha;
	SCR_DrawSmallStringExt( x, y, text, c, qtrue, qfalse );
}

// Copies at most maxChars printable characters of text into out
static void CL_DevClip( const char *text, int maxChars, char *out, int outSize ) {
	if ( maxChars < 4 ) {
		maxChars = 4;
	}
	if ( maxChars > outSize - 1 ) {
		maxChars = outSize - 1;
	}
	Q_strncpyz( out, text, maxChars + 1 );
	if ( (int)strlen( text ) > maxChars ) {
		Q_strncpyz( out + maxChars - 3, "...", 4 );
	}
}

// Console charset text at any cell size (the small-char helpers are fixed size)
static void CL_DevTextSized( float x, float y, float cw, float ch, const char *text, const float *color, float alpha ) {
	vec4_t c;

	Vector4Copy( color, c );
	c[3] *= alpha;
	re.SetColor( c );
	for ( ; *text; text++, x += cw ) {
		int code = *text & 255;

		if ( code == ' ' ) {
			continue;
		}
		re.DrawStretchPic( x, y, cw, ch, ( code & 15 ) * 0.0625f, ( code >> 4 ) * 0.0625f,
			( code & 15 ) * 0.0625f + 0.0625f, ( code >> 4 ) * 0.0625f + 0.0625f, cls.charSetShader );
	}
	re.SetColor( NULL );
}

// Small and out of the way in the bottom-right corner, newest at the bottom,
// so they never cover menus or the HUD; the full text is in the log
static void CL_DevDrawToasts( void ) {
	devNote_t notes[ MAX_TOASTS ];
	int now = Sys_Milliseconds();
	int n = Dev_RecentNotes( notes, MAX_TOASTS, TOAST_MSEC );
	float cw = g_smallchar_width * TOAST_SCALE, ch = g_smallchar_height * TOAST_SCALE;
	float lineH = ch + 6 * TOAST_SCALE, pad = cw;
	float margin = g_smallchar_width;
	int maxChars = (int)( cls.glconfig.vidWidth / 3 / cw );
	int i;

	for ( i = 0; i < n; i++ ) {
		char line[ 192 ];
		int age = now - notes[ i ].time;
		float alpha = age > TOAST_MSEC - TOAST_FADE_MSEC ? ( TOAST_MSEC - age ) / (float)TOAST_FADE_MSEC : 1.0f;
		float y = cls.glconfig.vidHeight - margin - ( n - i ) * ( lineH + 2 );
		float w, x;
		vec4_t bg;

		CL_DevClip( notes[ i ].text, maxChars, line, sizeof( line ) );
		w = strlen( line ) * cw + 2 * pad;
		x = cls.glconfig.vidWidth - margin - w;

		Vector4Copy( colorPanel, bg );
		bg[3] *= alpha;
		CL_DevFill( x, y, w, lineH, bg );
		CL_DevTextSized( x + pad, y + ( lineH - ch ) / 2, cw, ch,
			line, notes[ i ].level == DEV_NOTE_ERROR ? colorErrorText : colorWarnText, alpha );
	}
}

static void CL_DevDrawErrorPanel( const char *error ) {
	int w = cls.glconfig.vidWidth, h = cls.glconfig.vidHeight;
	int cw = g_smallchar_width, lineH = g_smallchar_height + 6;
	int cols = w / cw - 8;
	int x = 4 * cw, y = h / 3;
	int len = strlen( error ), pos;
	char line[ 256 ];

	CL_DevFill( 0, 0, w, h, colorErrorBg );
	CL_DevText( x, y, "ERROR", colorErrorText, 1.0f );
	y += lineH * 2;

	// wrap the message
	for ( pos = 0; pos < len && y < h - lineH * 4; pos += cols, y += lineH ) {
		Q_strncpyz( line, error + pos, ( cols + 1 < (int)sizeof( line ) ) ? cols + 1 : (int)sizeof( line ) );
		CL_DevText( x, y, line, colorText, 1.0f );
	}

	y += lineH;
	CL_DevClip( va( "log: %s", Dev_LogPath() ), cols, line, sizeof( line ) );
	CL_DevText( x, y, line, colorDim, 1.0f );
	CL_DevText( x, y + lineH, "(shown for 10 s or until the next map loads)", colorDim, 1.0f );
}

static void CL_DevDrawOverlay( void ) {
	devStats_t stats;
	devNote_t notes[ 3 ];
	char lines[ 10 ][ 128 ];
	int numLines = 0, n, i, widest = 0;
	int cw = g_smallchar_width, lineH = g_smallchar_height + 2;
	int x, y;

	Dev_GetStats( &stats );

	Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "fps %4.0f   frame %5.2f ms  p99 %5.2f", stats.fps,
		stats.frameP50, stats.frameP99 );
	Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "input->swap %4.1f ms (max %4.1f)", stats.inputAgeMsec,
		stats.inputAgeMaxMsec );
	Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "mouse %s", stats.mouseSource );
	if ( stats.tickRate > 0 ) {
		Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "tick %5.1f/s (sv_fps %s)", stats.tickRate, Cvar_VariableString( "sv_fps" ) );
	} else {
		Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "tick -- (no local server)" );
	}
	if ( clc.state == CA_ACTIVE && cl.snap.valid ) {
		Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "ping %3d ms   ents %d", cl.snap.ping, cl.snap.numEntities );
	} else {
		Com_sprintf( lines[ numLines++ ], sizeof( lines[0] ), "not in game" );
	}

	n = Dev_RecentNotes( notes, 3, 0 );
	for ( i = 0; i < n; i++ ) {
		CL_DevClip( notes[ i ].text, 48, lines[ numLines++ ], sizeof( lines[0] ) );
	}

	for ( i = 0; i < numLines; i++ ) {
		int len = strlen( lines[ i ] );
		if ( len > widest ) {
			widest = len;
		}
	}

	x = cls.glconfig.vidWidth - ( widest + 2 ) * cw - 4;
	y = 4;
	CL_DevFill( x, y, ( widest + 2 ) * cw, numLines * lineH + 6, colorPanel );
	for ( i = 0; i < numLines; i++ ) {
		const float *color = i < numLines - n ? colorText :
			( notes[ i - ( numLines - n ) ].level == DEV_NOTE_ERROR ? colorErrorText : colorWarnText );
		CL_DevText( x + cw, y + 3 + i * lineH, lines[ i ], color, 1.0f );
	}
}

/*
=================
CL_DevDraw

Called at the end of SCR_DrawScreenField, on top of everything.
=================
*/
void CL_DevDraw( void ) {
	const char *error;

	if ( !cls.rendererStarted || !cls.whiteShader ) {
		return;
	}

	error = uivm ? NULL : Dev_RecentError( ERROR_PANEL_MSEC );
	if ( error && clc.state != CA_ACTIVE ) {
		CL_DevDrawErrorPanel( error );
	}
	if ( dev_overlay->integer ) {
		CL_DevDrawOverlay();
	}
	if ( dev_toasts->integer ) {
		CL_DevDrawToasts();
	}
}

/*
=================
sens_test
=================
*/
void CL_DevSensSample( float rawCounts, float yawDegrees ) {
	if ( sensTest.active ) {
		sensTest.counts += rawCounts;
		sensTest.degrees += yawDegrees;
	}
}

static void CL_DevSensReport( void ) {
	double counts = fabs( sensTest.counts );
	double degrees = fabs( sensTest.degrees );
	float sens = Cvar_VariableValue( "sensitivity" );
	float yaw = Cvar_VariableValue( "m_yaw" );
	double expected = sens * yaw > 0 ? 360.0 / ( sens * yaw ) : 0;

	Com_Printf( "sens_test: %.0f counts, %.2f degrees turned\n", counts, degrees );

	if ( degrees < 1.0 ) {
		Com_Printf( "sens_test: no turn measured; turn while the test is running\n" );
		return;
	}

	Com_Printf( "  measured  %.1f counts/360\n", counts * 360.0 / degrees );
	Com_Printf( "  expected  %.1f counts/360 (360 / (sensitivity %g * m_yaw %g))\n", expected, sens, yaw );

	if ( sens_dpi->value > 0 ) {
		double cmPer360 = ( counts * 360.0 / degrees ) / sens_dpi->value * 2.54;
		Com_Printf( "  cm/360    %.2f at %g dpi\n", cmPer360, sens_dpi->value );
		Com_Printf( "  mouse moved %.2f cm according to the counts; compare with a ruler to detect OS acceleration\n",
			counts / sens_dpi->value * 2.54 );
	} else {
		Com_Printf( "  set sens_dpi <your mouse dpi> to also get cm/360\n" );
	}

	if ( Cvar_VariableValue( "cl_mouseAccel" ) != 0 || Cvar_VariableIntegerValue( "m_filter" ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: sens_test: cl_mouseAccel or m_filter is on; counts/360 won't match the converter\n" );
	}
	if ( cl.cgameSensitivity != 1.0f ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: sens_test: zoom sensitivity scale %.3f was active\n", cl.cgameSensitivity );
	}
}

static void CL_DevSensTest_f( void ) {
	const char *arg = Cmd_Argv( 1 );
	qboolean start = !Q_stricmp( arg, "start" ) || ( !arg[0] && !sensTest.active );

	if ( start ) {
		Com_Memset( &sensTest, 0, sizeof( sensTest ) );
		sensTest.active = qtrue;
		Com_Printf( "sens_test: started. Turn exactly 360 degrees (or move the mouse a measured distance), then run sens_test again.\n" );
		return;
	}

	if ( !sensTest.active ) {
		Com_Printf( "usage: sens_test [start|stop]\n" );
		return;
	}

	sensTest.active = qfalse;
	CL_DevSensReport();
}

void CL_DevInit( void ) {
	dev_overlay = Cvar_Get( "dev_overlay", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( dev_overlay, "Corner panel: FPS, frame time, tick rate, ping, entities, last warnings" );
	// opt-in: nothing in the console font shows on screen unless asked for;
	// dev builds stream warnings to the launching terminal instead
	dev_toasts = Cvar_Get( "dev_toasts", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( dev_toasts, "Show warnings and errors as small toasts in the bottom-right for 5 seconds" );
	sens_dpi = Cvar_Get( "sens_dpi", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( sens_dpi, "Mouse DPI, used by sens_test to report cm/360" );

	Cmd_AddCommand( "sens_test", CL_DevSensTest_f );
}

void CL_DevOnRendererStarted( void ) {
	Dev_SetGpuInfo( va( "%s / %s / %s", cls.glconfig.vendor_string, cls.glconfig.renderer_string,
		cls.glconfig.version_string ) );
	Dev_SuppressSpikes( 120 );
}
