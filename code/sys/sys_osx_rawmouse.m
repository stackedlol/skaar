/*
===========================================================================
Raw mouse motion on macOS via GCMouse (GameController framework).

SDL 2 reads relative motion from NSEvent deltas, which include the system
pointer acceleration; GCMouse reports the device's raw counts. Like SDL 3,
this is only enabled on macOS 14+: on 12/13 GCMouse motion and buttons were
unreliable on MacBooks. Only motion is taken from here; buttons and the wheel
still come through SDL. See docs/feel.md for sources.

This file is part of Skaar, a fork of ioquake3. GPLv2 or later.
===========================================================================
*/
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <os/lock.h>
#include <stdatomic.h>
#include <time.h>

#include "sys_rawmouse.h"

static os_unfair_lock	accumLock = OS_UNFAIR_LOCK_INIT;
static double			accumX, accumY;
static long long		newestMotionUsec;

static atomic_int		miceAttached;
static atomic_int		capturing;
static qboolean			initialized;

static dispatch_queue_t	handlerQueue;
static id				connectObserver, disconnectObserver;

// Same clock as Dev_Microseconds() (CLOCK_MONOTONIC)
static long long NowUsec( void ) {
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void AttachMouse( GCMouse *mouse ) API_AVAILABLE( macos( 14.0 ) ) {
	if( !mouse.mouseInput ) {
		return;
	}
	mouse.handlerQueue = handlerQueue;
	mouse.mouseInput.mouseMovedHandler = ^( GCMouseInput *input, float deltaX, float deltaY ) {
		(void)input;
		if( !atomic_load( &capturing ) ) {
			return;
		}
		os_unfair_lock_lock( &accumLock );
		accumX += deltaX;
		accumY -= deltaY;	// GCMouse is +y up; the engine is +y down
		newestMotionUsec = NowUsec();
		os_unfair_lock_unlock( &accumLock );
	};
	atomic_fetch_add( &miceAttached, 1 );
}

static void DetachMouse( GCMouse *mouse ) API_AVAILABLE( macos( 14.0 ) ) {
	if( mouse.mouseInput ) {
		mouse.mouseInput.mouseMovedHandler = nil;
	}
	if( atomic_fetch_sub( &miceAttached, 1 ) <= 0 ) {
		atomic_store( &miceAttached, 0 );
	}
}

qboolean RawMouse_Init( void ) {
	if( initialized ) {
		return qtrue;
	}
	if( @available( macOS 14.0, * ) ) {
		dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
			DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0 );
		NSNotificationCenter *center = [NSNotificationCenter defaultCenter];

		handlerQueue = dispatch_queue_create( "org.skaar.rawmouse", attr );
		connectObserver = [center addObserverForName:GCMouseDidConnectNotification object:nil queue:nil
			usingBlock:^( NSNotification *note ) { AttachMouse( note.object ); }];
		disconnectObserver = [center addObserverForName:GCMouseDidDisconnectNotification object:nil queue:nil
			usingBlock:^( NSNotification *note ) { DetachMouse( note.object ); }];

		for( GCMouse *mouse in [GCMouse mice] ) {
			AttachMouse( mouse );
		}
		initialized = qtrue;
		return qtrue;
	}
	return qfalse;
}

void RawMouse_Shutdown( void ) {
	if( !initialized ) {
		return;
	}
	if( @available( macOS 14.0, * ) ) {
		NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
		[center removeObserver:connectObserver];
		[center removeObserver:disconnectObserver];
		connectObserver = disconnectObserver = nil;
		for( GCMouse *mouse in [GCMouse mice] ) {
			if( mouse.mouseInput ) {
				mouse.mouseInput.mouseMovedHandler = nil;
			}
		}
		// MRC: dispatch_queue_create returns +1, and Init creates a new queue
		// on every vid_restart
		if( handlerQueue ) {
			dispatch_release( handlerQueue );
			handlerQueue = NULL;
		}
	}
	atomic_store( &miceAttached, 0 );
	atomic_store( &capturing, 0 );
	initialized = qfalse;
}

qboolean RawMouse_Active( void ) {
	return initialized && atomic_load( &miceAttached ) > 0;
}

void RawMouse_SetCapture( qboolean capture ) {
	atomic_store( &capturing, capture ? 1 : 0 );
	if( !capture ) {
		os_unfair_lock_lock( &accumLock );
		accumX = accumY = 0;
		os_unfair_lock_unlock( &accumLock );
	}
}

void RawMouse_Consume( int *dx, int *dy, long long *newestUsec ) {
	os_unfair_lock_lock( &accumLock );
	// whole counts out, fractions stay for the next frame
	*dx = (int)accumX;
	*dy = (int)accumY;
	accumX -= *dx;
	accumY -= *dy;
	*newestUsec = newestMotionUsec;
	newestMotionUsec = 0;
	os_unfair_lock_unlock( &accumLock );
}

const char *RawMouse_Description( void ) {
	if( !initialized ) {
		return "unavailable (needs macOS 14+)";
	}
	return RawMouse_Active() ? "GCMouse (raw counts)" : "GCMouse (no mouse attached yet)";
}
