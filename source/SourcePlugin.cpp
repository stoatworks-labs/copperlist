/**
	The FF_SOURCE registration, and nothing else.

	**This file is listed directly in the source target, not in the shared
	object library.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a static archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Copperlist.bundle/Contents/MacOS/Copperlist | grep plugMain

	`CP01`: four characters, unique across the fleet.
*/
#include "Copperlist.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< copperlist::CopperlistPlugin >,           // Create method
	"CP01",                                                  // Plugin unique ID of maximum length 4
	"SW Copperlist",                                         // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"An Amiga cracktro, emulated chip by chip.\n\n"
	"Copper bars are colour-register writes at the start of a scanline, so they are one line tall per "
	"step and two that cross cannot blend. The stars are one sprite per layer, moved down the screen by "
	"the copper. The scroller and the bobs are blits at whole-pixel offsets, the cube is the blitter's "
	"line mode, and the palette is 32 registers of 12 bits. None of that is drawn to look right: the "
	"demo is written against an emulated copper, blitter and Denise, so the look cannot cheat.\n\n"
	"Fields run at 50 Hz (PAL) or 60 Hz (NTSC) from the host clock; a 60 fps composition repeats "
	"fields and never interpolates them.",                  // Plugin description
	"Copperlist FFGL source"                                 // About
);

extern "C" const char* CopperlistSourceBuildStamp()
{
	return "copperlist " COPPERLIST_VERSION " source, built " __DATE__ " " __TIME__;
}
