#pragma once

// Gefüllt von scripts/version.py beim Bauen. Die Vorgaben hier greifen nur,
// wenn das Skript nicht lief — dann steht "unknown" da, und das ist die
// gewünschte Aussage: lieber kein Hash als einer, der einen sauberen Build
// behauptet, den es nicht gab.

#ifndef ZW_FW_VERSION
#define ZW_FW_VERSION "unknown"
#endif

#ifndef ZW_GIT_HASH
#define ZW_GIT_HASH "unknown"
#endif

#ifndef ZW_GIT_DIRTY
#define ZW_GIT_DIRTY 0
#endif

#ifndef ZW_BUILD_DATE
#define ZW_BUILD_DATE "unknown"
#endif
