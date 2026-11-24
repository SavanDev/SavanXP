/*
Xash3D sound output for SavanXP.

The engine mixes 16-bit stereo at SOUND_DMA_SPEED into a ring (snd.buffer)
and learns how much of it has been played from snd.samplepos. An output
thread wakes every few milliseconds, takes the frames the wall clock says are
due, resamples them to the device rate and sends them through the SDK's audio
link: straight to /dev/audio0 when it is free, through audiod when the
daemon holds it.

The thread exists because of audiod's pipe. It holds about 40 ms of sound and
a send that does not fit is cut short, so the engine's frame, which often
lasts longer than that, cannot feed it from SNDDMA_Submit. Small sends at a
steady pace always fit.

Playback never passes what the engine has painted. A frame slower than the
mix-ahead becomes a short gap instead of a replay of stale samples.
*/

#include "common.h"
#include "platform/platform.h"
#include "sound.h"

#if XASH_SOUND == SOUND_SAVANXP

#include <pthread.h>
#include <stdlib.h>
#include "savanxp/audio.h"
#include "savanxp/libc.h"

#define SAVANXP_SOUND_PERIOD_MS 10u
// The longest stretch one wakeup plays. A longer stall is dropped, not caught up.
#define SAVANXP_SOUND_MAX_DELTA_MS 100u
#define SAVANXP_SOUND_DEFAULT_FRAMES 0x8000

static struct
{
	int fd;
	uint32_t rate;
	struct sx_audio_server_link link;
	pthread_t thread;
	pthread_mutex_t lock; // recursive: the engine nests BeginPainting and Submit
	qboolean lock_ready;
	volatile qboolean running;
	uint32_t last_ms;
	uint32_t frame_remainder;
	uint64_t position; // ring position in frames, 32.32 fixed point
	uint64_t step;     // source frames per device frame, 32.32 fixed point
	int16_t *out;
	size_t out_frames;
} sxsnd = { .fd = -1 };

static int SavanXP_RingFrames( void )
{
	int frames = (int)s_samplecount.value;
	int ring = 1;

	if( frames <= 0 )
		frames = SAVANXP_SOUND_DEFAULT_FRAMES;
	// The engine masks positions in the ring, so its size is a power of two.
	while( ring < frames )
		ring <<= 1;
	return ring;
}

static void SavanXP_CloseOutput( void )
{
	sx_audio_server_link_close( &sxsnd.link );
	if( sxsnd.fd >= 0 )
		savanxp_close( sxsnd.fd );
	sxsnd.fd = -1;
	free( sxsnd.out );
	sxsnd.out = NULL;
	sxsnd.out_frames = 0;
}

// Called with the lock held. Returns the device frames written to sxsnd.out.
static size_t SavanXP_Render( size_t due )
{
	const int16_t *in = (const int16_t *)snd.buffer;
	const uint32_t ring = (uint32_t)snd.samples >> 1;
	const uint32_t read = (uint32_t)( sxsnd.position >> 32 );
	const uint32_t painted = (uint32_t)snd.paintedtime & ( ring - 1 );
	const uint32_t available = ( painted + ring - read ) & ( ring - 1 );
	uint64_t position = sxsnd.position;
	size_t frames;

	for( frames = 0; frames < due; frames++ )
	{
		const uint32_t i0 = (uint32_t)( position >> 32 );
		const uint32_t i1 = ( i0 + 1 ) & ( ring - 1 );
		const int64_t frac = (int64_t)( position & 0xffffffffu );

		// Interpolation reads the next frame too, so it must be painted.
		if((( i0 + ring - read ) & ( ring - 1 )) + 1 >= available )
			break;
		sxsnd.out[frames * 2] = (int16_t)( in[i0 * 2] + ((( in[i1 * 2] - in[i0 * 2] ) * frac ) >> 32 ));
		sxsnd.out[frames * 2 + 1] = (int16_t)( in[i0 * 2 + 1] + ((( in[i1 * 2 + 1] - in[i0 * 2 + 1] ) * frac ) >> 32 ));
		position += sxsnd.step;
		if(( position >> 32 ) >= ring )
			position -= (uint64_t)ring << 32;
	}

	sxsnd.position = position;
	snd.samplepos = (int)( position >> 32 ) * 2;
	return frames;
}

static void *SavanXP_SoundThread( void *arg )
{
	(void)arg;
	while( sxsnd.running )
	{
		uint32_t now, delta;
		uint64_t numerator;
		size_t due, frames;
		long sent;

		sleep_ms( SAVANXP_SOUND_PERIOD_MS );
		now = (uint32_t)uptime_ms();
		delta = now - sxsnd.last_ms;
		sxsnd.last_ms = now;
		if( delta > SAVANXP_SOUND_MAX_DELTA_MS )
			delta = SAVANXP_SOUND_MAX_DELTA_MS;
		numerator = (uint64_t)delta * sxsnd.rate + sxsnd.frame_remainder;
		sxsnd.frame_remainder = (uint32_t)( numerator % 1000u );
		due = (size_t)( numerator / 1000u );
		if( due > sxsnd.out_frames )
			due = sxsnd.out_frames;
		if( due == 0 )
			continue;

		pthread_mutex_lock( &sxsnd.lock );
		frames = SavanXP_Render( due );
		pthread_mutex_unlock( &sxsnd.lock );
		if( frames == 0 )
			continue;

		sent = sx_audio_server_output( &sxsnd.link, sxsnd.fd, sxsnd.out, frames * 4, sxsnd.rate );
		if( sent < 0 )
		{
			char message[80];

			Q_snprintf( message, sizeof( message ), S_ERROR "Audio: output failed (%ld), sound is off\n", sent );
			Sys_PrintLog( message );
			break;
		}
	}
	return NULL;
}

/*
==================
SNDDMA_Init

Opens the PCM device, allocates the engine's ring and starts the output thread
==================
*/
qboolean SNDDMA_Init( void )
{
	struct savanxp_audio_info info = { 0 };
	long fd = audio_open();
	int ring;

	if( fd < 0 )
	{
		Con_Printf( "Audio: no PCM device (%ld)\n", fd );
		return false;
	}
	if( audio_get_info( (int)fd, &info ) < 0 || info.sample_rate_hz == 0 || info.channels != 2 ||
		info.bits_per_sample != 16 || info.frame_bytes != 4 )
	{
		Con_Printf( "Audio: the PCM device is not 16-bit stereo\n" );
		savanxp_close( (int)fd );
		return false;
	}

	sxsnd.fd = (int)fd;
	sxsnd.rate = info.sample_rate_hz;
	sxsnd.out_frames = (size_t)sxsnd.rate * SAVANXP_SOUND_MAX_DELTA_MS / 1000u + 1;
	sxsnd.out = malloc( sxsnd.out_frames * 2 * sizeof( int16_t ));
	if( !sxsnd.out )
	{
		SavanXP_CloseOutput();
		return false;
	}
	sx_audio_server_link_init( &sxsnd.link );
	sx_audio_server_set_client_name( &sxsnd.link, "xash3d-client" );

	if( !sxsnd.lock_ready )
	{
		pthread_mutexattr_t attr;

		pthread_mutexattr_init( &attr );
		pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
		pthread_mutex_init( &sxsnd.lock, &attr );
		pthread_mutexattr_destroy( &attr );
		sxsnd.lock_ready = true;
	}

	ring = SavanXP_RingFrames();
	snd.format.speed = SOUND_DMA_SPEED;
	snd.format.channels = 2;
	snd.format.width = 2;
	snd.samples = ring * 2; // 16-bit samples, both channels
	snd.buffer = Mem_Calloc( sndpool, snd.samples * 2 );
	snd.samplepos = 0;

	sxsnd.position = 0;
	sxsnd.step = ((uint64_t)SOUND_DMA_SPEED << 32 ) / sxsnd.rate;
	sxsnd.frame_remainder = 0;
	sxsnd.last_ms = (uint32_t)uptime_ms();
	sxsnd.running = true;
	if( pthread_create( &sxsnd.thread, NULL, SavanXP_SoundThread, NULL ) != 0 )
	{
		Con_Printf( S_ERROR "Audio: can't start the output thread\n" );
		sxsnd.running = false;
		SNDDMA_Shutdown();
		return false;
	}

	snd.initialized = true;
	snd.backend_name = "SavanXP";
	Con_Printf( "Audio: SavanXP output at %u Hz, mixing at %d Hz\n", sxsnd.rate, SOUND_DMA_SPEED );
	return true;
}

/*
==============
SNDDMA_BeginPainting

Keeps the output thread out of snd.buffer while the engine paints
===============
*/
void SNDDMA_BeginPainting( void )
{
	if( sxsnd.lock_ready )
		pthread_mutex_lock( &sxsnd.lock );
}

/*
==============
SNDDMA_Submit

The output thread reads the ring by itself; this only lets it go
===============
*/
void SNDDMA_Submit( void )
{
	if( sxsnd.lock_ready )
		pthread_mutex_unlock( &sxsnd.lock );
}

/*
==============
SNDDMA_Shutdown

Stops the output thread, closes the output and frees the ring
===============
*/
void SNDDMA_Shutdown( void )
{
	Con_Printf( "Shutting down audio.\n" );
	snd.initialized = false;
	if( sxsnd.running )
	{
		sxsnd.running = false;
		pthread_join( sxsnd.thread, NULL );
	}
	SavanXP_CloseOutput();
	if( snd.buffer )
	{
		Mem_Free( snd.buffer );
		snd.buffer = NULL;
	}
}

// The engine does not pause the output on SavanXP.
void SNDDMA_Activate( qboolean active )
{
}

qboolean VoiceCapture_Init( void )
{
	return false;
}

qboolean VoiceCapture_Activate( qboolean activate )
{
	return false;
}

qboolean VoiceCapture_Lock( qboolean lock )
{
	return false;
}

void VoiceCapture_Shutdown( void )
{
}

#endif // XASH_SOUND == SOUND_SAVANXP
