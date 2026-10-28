/*
 * gpu_pvr smoke test: one gouraud triangle (pvr_prim) and one textured,
 * blended quad (direct render), then the frame is written to a PPM.
 *
 *   pvrtest [out.ppm] [--window]
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <SDL.h>

#include <dc/pvr.h>
#include "pvr_host.h"

static void vert( pvr_vertex_t *v, uint32_t flags, float x, float y, float z,
				  float u, float t, uint32_t argb ) {
	memset( v, 0, sizeof( *v ) );
	v->flags = flags;
	v->x = x; v->y = y; v->z = z;
	v->u = u; v->v = t;
	v->argb = argb;
}

int main( int argc, char **argv ) {
	const char *out = "pvrtest.ppm";
	int window = 0, i;

	for ( i = 1; i < argc; i++ ) {
		if ( !strcmp( argv[i], "--window" ) ) {
			window = 1;
		} else {
			out = argv[i];
		}
	}

	if ( window && pvr_host_open( "gpu_pvr test", 1 ) ) {
		return 1;
	}
	if ( pvr_init_defaults() ) {
		fprintf( stderr, "pvr_init failed\n" );
		return 1;
	}

	/* 64x64 RGB565 checkerboard, twiddled layout doesn't matter for a checker
	 * this coarse, so upload it non-twiddled. */
	static uint16_t pixels[64 * 64];
	for ( i = 0; i < 64 * 64; i++ ) {
		int x = i & 63, y = i >> 6;
		pixels[i] = ( ( x >> 3 ) ^ ( y >> 3 ) ) & 1 ? 0xffff : 0xf800;
	}
	pvr_ptr_t tex = pvr_mem_malloc( sizeof( pixels ) );
	pvr_txr_load( pixels, tex, sizeof( pixels ) );

	pvr_set_bg_color( 0.1f, 0.1f, 0.3f );
	pvr_scene_begin();

	/* opaque gouraud triangle through pvr_prim */
	{
		pvr_poly_cxt_t cxt;
		pvr_poly_hdr_t hdr;
		pvr_vertex_t v[3];

		pvr_poly_cxt_col( &cxt, PVR_LIST_OP_POLY );
		cxt.gen.culling = PVR_CULLING_NONE;
		pvr_poly_compile( &hdr, &cxt );

		pvr_list_begin( PVR_LIST_OP_POLY );
		pvr_prim( &hdr, sizeof( hdr ) );
		vert( &v[0], PVR_CMD_VERTEX, 100, 400, 1.0f, 0, 0, 0xffff0000 );
		vert( &v[1], PVR_CMD_VERTEX, 320, 60, 1.0f, 0, 0, 0xff00ff00 );
		vert( &v[2], PVR_CMD_VERTEX_EOL, 540, 400, 1.0f, 0, 0, 0xff0000ff );
		pvr_prim( v, sizeof( v ) );
		pvr_list_finish();
	}

	/* translucent textured quad through direct render */
	{
		pvr_poly_cxt_t cxt;
		pvr_poly_hdr_t *hdr;
		pvr_vertex_t *v;
		pvr_dr_state_t dr;

		pvr_poly_cxt_txr( &cxt, PVR_LIST_TR_POLY,
						  PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
						  64, 64, tex, PVR_FILTER_NONE );
		cxt.gen.culling = PVR_CULLING_NONE;

		pvr_list_begin( PVR_LIST_TR_POLY );
		pvr_dr_init( &dr );
		hdr = (pvr_poly_hdr_t *)pvr_dr_target( dr );
		pvr_poly_compile( hdr, &cxt );
		pvr_dr_commit( hdr );

		v = pvr_dr_target( dr ); vert( v, PVR_CMD_VERTEX, 200, 300, 2.0f, 0, 2, 0x80ffffff ); pvr_dr_commit( v );
		v = pvr_dr_target( dr ); vert( v, PVR_CMD_VERTEX, 200, 140, 2.0f, 0, 0, 0x80ffffff ); pvr_dr_commit( v );
		v = pvr_dr_target( dr ); vert( v, PVR_CMD_VERTEX, 440, 300, 2.0f, 2, 2, 0x80ffffff ); pvr_dr_commit( v );
		v = pvr_dr_target( dr ); vert( v, PVR_CMD_VERTEX_EOL, 440, 140, 2.0f, 2, 0, 0x80ffffff ); pvr_dr_commit( v );
		pvr_dr_finish();
		pvr_list_finish();
	}

	pvr_scene_finish();

	{
		static uint8_t rgb[640 * 480 * 3];
		FILE *f;

		if ( !pvr_host_read_front( rgb, 640 * 3 ) ) {
			fprintf( stderr, "nothing rendered\n" );
			return 1;
		}
		f = fopen( out, "wb" );
		if ( !f ) {
			perror( out );
			return 1;
		}
		fprintf( f, "P6\n640 480\n255\n" );
		fwrite( rgb, 1, sizeof( rgb ), f );
		fclose( f );
		printf( "wrote %s\n", out );
	}

	if ( window ) {
		SDL_Event ev;
		int quit = 0;
		while ( !quit && SDL_WaitEvent( &ev ) ) {
			quit = ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN;
		}
		pvr_host_close();
	}

	pvr_mem_free( tex );
	pvr_shutdown();
	return 0;
}
