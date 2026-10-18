// app_hd_movie.h - the Vita side of the HD replacement movies (gfx.hd_movies; docs/HD_MOVIES.md).
//
// The framework (PortableKit host/movie/hd_movie.hpp) runs the game's movie player unchanged and asks an HdSource for the
// replacement picture of every access unit. On the Vita that source is the hardware H.264 decoder (SceVideodec / SceAvcdec),
// driven in lockstep: the picture of access unit n is decoded when the game decodes unit n, so the replacement can never drift
// from the game's own timing. The decoded frame stays in physically contiguous memory as 4:2:0; the GPU's texture unit converts
// it to RGB while the renderer draws it (a GXM YUV420 texture), so nothing is converted or copied by the CPU.
#pragma once

namespace app {

// Registers the hardware source with the framework (portablekit::movie::set_hd_source_factory) and the GL binder that lets the
// renderer sample its frames (portablekit::gpu::set_hd_native_binder). Cheap: nothing is allocated or loaded until a
// replacement movie is opened.
void hd_movie_install();

}  // namespace app
