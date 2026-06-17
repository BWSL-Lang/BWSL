#pragma once

// Overridable at build time via -DVERSION=\"1.2.3\" (see Makefile's
// BWSL_VERSION / build.bat's BWSL_VERSION env var, used by the release
// pipeline to inject the tag version automatically).
#ifndef VERSION
#define VERSION "0.0.0-dev"
#endif
