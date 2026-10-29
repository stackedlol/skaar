/*
===========================================================================
macOS app and window integration for the SDL client.

SDL2's app delegate activates the Dock, sleeps 300 ms and then activates the
game on every launch (a workaround for unbundled apps' menu bar). Skaar is
always a bundle, so Sys_OSX_InitApp drops the Dock dance and the sleep.

The green title bar button toggles the game's own fullscreen (a borderless
window over the display, see GLimp_FullscreenFlag) instead of zooming or
entering a fullscreen Space, so it behaves like Cmd+Ctrl+F and Cmd+Tab out of
fullscreen stays instant. Sys_OSX_SetFullscreen hides the menu bar and Dock
over it.

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#	include "SDL_syswm.h"
#else
#	include <SDL.h>
#	include <SDL_syswm.h>
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

@interface SkaarWindowActions : NSObject
- (void)toggleFullscreen:(id)sender;
@end

@implementation SkaarWindowActions
- (void)toggleFullscreen:(id)sender {
	Cbuf_AddText( "toggle r_fullscreen\n" );
}
@end

void Sys_OSX_InitWindow( SDL_Window *window ) {
	static SkaarWindowActions *actions;
	SDL_SysWMinfo info;
	NSButton *zoom;

	SDL_VERSION( &info.version );
	if ( !window || !SDL_GetWindowWMInfo( window, &info ) || info.subsystem != SDL_SYSWM_COCOA ) {
		return;
	}
	if ( !actions ) {
		actions = [[SkaarWindowActions alloc] init];
	}
	zoom = [info.info.cocoa.window standardWindowButton:NSWindowZoomButton];
	[zoom setTarget:actions];
	[zoom setAction:@selector(toggleFullscreen:)];
}

// The borderless fullscreen window covers the display, but the menu bar and
// Dock still draw over it. Hide them; macOS applies this only while Skaar is
// the active app, so Cmd+Tab brings them back for the other app instantly.
void Sys_OSX_SetFullscreen( qboolean fullscreen ) {
	static qboolean current;

	if ( fullscreen == current ) {
		return;
	}
	current = fullscreen;
	[NSApp setPresentationOptions:fullscreen ?
		NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar :
		NSApplicationPresentationDefault];
}

// Replaces -[SDLAppDelegate applicationDidFinishLaunching:] (SDL 2.32: Dock
// activation, SDL_Delay(300), activate, register user defaults)
static void Sys_OSX_DidFinishLaunching( id self, SEL cmd, NSNotification *note ) {
	if ( !SDL_GetHintBoolean( SDL_HINT_MAC_BACKGROUND_APP, SDL_FALSE ) ) {
		[NSApp activateIgnoringOtherApps:YES];
	}
	[NSClassFromString( @"SDLApplication" ) performSelector:@selector(registerUserDefaults)];
}

void Sys_OSX_InitApp( void ) {
	Class delegate = NSClassFromString( @"SDLAppDelegate" );
	Class app = NSClassFromString( @"SDLApplication" );
	SEL sel = @selector(applicationDidFinishLaunching:);

	// keep SDL's version if this SDL lacks what the replacement calls
	if ( !delegate || !app || !class_getInstanceMethod( delegate, sel ) ||
			!class_getClassMethod( app, @selector(registerUserDefaults) ) ) {
		return;
	}
	class_replaceMethod( delegate, sel, (IMP)Sys_OSX_DidFinishLaunching, "v@:@" );
}
