// The SWSE version, in one place (console banner, `ver`, `query version`,
// the log's session banner, the DLL's version resource and the plugin API's
// SWSEInterface.swseVersion, which a plugin's minSwseVersion is checked
// against). swse.rc includes this file too: keep it to plain #defines, which
// is all rc.exe understands.
#pragma once

#define SWSE_VER_MAJOR 1
#define SWSE_VER_MINOR 1
#define SWSE_VER_PATCH 1

#define SWSE_VERSION      "1.1.1"
#define SWSE_VERSION_FULL "1.1.1.0"
