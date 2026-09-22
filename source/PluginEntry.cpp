#include "Millpond.h"

/**
    The one registration.

    This file is listed directly in the Millpond MODULE target, not in
    millpond_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC archive
    the linker is entitled to drop the whole translation unit -- giving a
    bundle that loads, exports `plugMain`, and reports that it contains no
    plugins. The core stays an OBJECT library for the same reason.

        nm -gU Millpond.bundle/Contents/MacOS/Millpond | grep plugMain

    The name is `SW Millpond`, eleven characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class MillpondEffect : public millpond::MillpondPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< MillpondEffect >,                      // Create method
	"MP01",                                               // Plugin unique ID of maximum length 4
	"SW Millpond",                                        // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"The picture as the bed of a pond, under water that obeys the linear wave equations. Drop a pebble, "
	"skim a stone or let it rain: the rings, the still disc inside them, the reflections off the banks, "
	"the caustic net of sunlight and the glints all fall out of the model.",
	"Millpond FFGL effect"                                // About
);

extern "C" const char* MillpondBuildStamp()
{
	return "millpond " MILLPOND_VERSION ", built " __DATE__ " " __TIME__;
}
