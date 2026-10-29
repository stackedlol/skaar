/*
===========================================================================
Config versioning (sk_cfgVersion) and migrations.

Archived cvars outlive their defaults: when we change a default on purpose,
players who already have a config keep the old value forever. Each row
below says "in version N, if this cvar (or bind) still has its old default,
move it to the new one". Values the player chose themselves are left alone,
because they don't match the old default.

A fresh install has no config, starts at the current version and migrates
nothing. An existing config without sk_cfgVersion is version 0.
How to add a row: docs/config-migrations.md.

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#include "q_shared.h"
#include "qcommon.h"

#define CFG_VERSION 3	// bump with every new batch of rows

typedef struct {
	int			version;	// rows run when upgrading from below this version
	const char	*cvar;
	const char	*oldValues;	// old default(s), space separated; only these get changed
	const char	*newValue;
} cfgCvarMigration_t;

typedef struct {
	int			version;
	const char	*key;
	const char	*oldBinding;	// "" = only if the key is unbound
	const char	*newBinding;
} cfgBindMigration_t;

static const cfgCvarMigration_t cvarMigrations[] = {
	// 2026-10-22: reticle became a small pastel-yellow "+" (game/shared/reticle.h)
	{ 1, "cg_crosshairColor", "232323 ebe6db", "f7e08a" },
	{ 1, "cg_crosshairSize", "10", "4" },
	{ 1, "cg_crosshairGap", "4", "0" },
	// the GL2 auto-exposure pulled the light range to mid-grey
	{ 1, "r_toneMap", "1", "0" },
	{ 1, "r_autoExposure", "1", "0" },
	// neutral team names instead of Quake lore
	{ 1, "g_redTeam", "Stroggs", "Red" },
	{ 1, "g_blueTeam", "Pagans", "Blue" },
	// game modules are native only
	{ 1, "vm_game", "2", "0" },
	{ 1, "vm_cgame", "2", "0" },
	{ 1, "vm_ui", "2", "0" },
	// 2026-10-29: start in a centered, resizable 854x480 window; fullscreen is a toggle
	{ 2, "r_mode", "-2", "-1" },
	{ 2, "r_customwidth", "1600", "854" },
	{ 2, "r_customheight", "900 1024", "480" },
	{ 2, "r_allowResize", "0", "1" },
	{ 2, "r_centerWindow", "0", "1" },
	// 2026-10-29: no console-font toasts on screen by default, dev builds included
	{ 3, "dev_toasts", "1", "0" },
};

#ifndef DEDICATED	// the dedicated server has no key bindings
static const cfgBindMigration_t bindMigrations[] = {
	{ 1, "r", "map_restart", "sk_restart" },
	{ 1, "i", "", "sk_insights" },
	{ 1, "3", "", "weapon 3" },
	{ 1, "5", "", "weapon 5" },
	{ 1, "6", "", "weapon 6" },
	{ 1, "7", "", "weapon 7" },
	{ 1, "MWHEELUP", "", "weapprev" },
	{ 1, "MWHEELDOWN", "", "weapnext" },
};
#endif

static cvar_t *sk_cfgVersion;

static qboolean Cfg_ValueIn( const char *value, const char *list ) {
	char token[ MAX_CVAR_VALUE_STRING ];
	const char *p = list;

	while ( *p ) {
		int n = 0;
		while ( *p == ' ' ) {
			p++;
		}
		while ( *p && *p != ' ' && n < (int)sizeof( token ) - 1 ) {
			token[ n++ ] = *p++;
		}
		token[ n ] = '\0';
		if ( n && !Q_stricmp( value, token ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void Cfg_Set( int version, const char *cvar, const char *from, const char *to ) {
	Com_Printf( "config v%d: %s \"%s\" -> \"%s\"\n", version, cvar, from, to );
	Cvar_Set( cvar, to );
}

// Migrations that depend on several cvars at once
static void Cfg_MigrateSpecial( int from ) {
	if ( from < 1 ) {
		// the old default window (1600x900, windowed) became fullscreen at the desktop resolution
		if ( !strcmp( Cvar_VariableString( "r_mode" ), "-1" ) && !strcmp( Cvar_VariableString( "r_fullscreen" ), "0" ) &&
			!strcmp( Cvar_VariableString( "r_customwidth" ), "1600" ) &&
			!strcmp( Cvar_VariableString( "r_customheight" ), "900" ) ) {
			Cfg_Set( 1, "r_mode", "-1", "-2" );
			Cfg_Set( 1, "r_fullscreen", "0", "1" );
		}
	}
}

#ifndef DEDICATED
char	*Key_GetBinding( int keynum );
void	Key_SetBinding( int keynum, const char *binding );
int		Key_StringToKeynum( char *str );

static void Cfg_MigrateBinds( int from ) {
	size_t i;

	for ( i = 0; i < ARRAY_LEN( bindMigrations ); i++ ) {
		const cfgBindMigration_t *m = &bindMigrations[ i ];
		char key[ 32 ];
		const char *current;
		int keynum;

		if ( from >= m->version ) {
			continue;
		}
		Q_strncpyz( key, m->key, sizeof( key ) );
		keynum = Key_StringToKeynum( key );
		if ( keynum < 0 ) {
			continue;
		}
		current = Key_GetBinding( keynum );
		if ( !current ) {
			current = "";
		}
		if ( Q_stricmp( current, m->oldBinding ) ) {
			continue;	// the player bound it to something else
		}
		Com_Printf( "config v%d: bind %s \"%s\" -> \"%s\"\n", m->version, m->key, current, m->newBinding );
		Key_SetBinding( keynum, m->newBinding );
	}
}
#endif

/*
=================
Com_MigrateConfig

Called after the player's config has been executed. hadConfig: the config
file existed (otherwise this is a fresh install at the current version).
=================
*/
void Com_MigrateConfig( qboolean hadConfig ) {
	int from;
	size_t i;

	sk_cfgVersion = Cvar_Get( "sk_cfgVersion", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( sk_cfgVersion, "Config format version; older configs are migrated at startup (docs/config-migrations.md)" );

	if ( !hadConfig ) {
		Cvar_Set( "sk_cfgVersion", va( "%i", CFG_VERSION ) );
		return;
	}

	from = sk_cfgVersion->integer;
	if ( from >= CFG_VERSION ) {
		return;
	}

	Com_Printf( "config: migrating from v%d to v%d\n", from, CFG_VERSION );
	for ( i = 0; i < ARRAY_LEN( cvarMigrations ); i++ ) {
		const cfgCvarMigration_t *m = &cvarMigrations[ i ];
		const char *current;

		if ( from >= m->version ) {
			continue;
		}
		current = Cvar_VariableString( m->cvar );
		if ( current[0] && Cfg_ValueIn( current, m->oldValues ) ) {
			Cfg_Set( m->version, m->cvar, current, m->newValue );
		}
	}
	Cfg_MigrateSpecial( from );
#ifndef DEDICATED
	Cfg_MigrateBinds( from );
#endif

	Cvar_Set( "sk_cfgVersion", va( "%i", CFG_VERSION ) );
}
