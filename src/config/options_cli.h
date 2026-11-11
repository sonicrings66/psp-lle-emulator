#pragma once

#include "options.h"

// The ONLY file in this project allowed to call std::getenv() or read
// argv. Every other file receives configuration exclusively through an
// EmuOptions sub-struct passed in by a constructor. A standing check
// (`rg "getenv" src/` from the project root) must return only
// options_cli.cpp -- that's what makes a PSP_WINDOW-style silently-gated
// behavior structurally impossible elsewhere in the tree.
//
// Both an env-var pass (for compatibility with existing ad hoc test
// invocations and CI scripts) and a plain argv pass are supported; argv
// wins on conflict. See options_cli.cpp for the exact names recognized.

namespace beta::config {

// Parses process environment variables into an EmuOptions. Call this
// once, in main(), before anything else is constructed.
EmuOptions parseOptionsFromEnvironment();

// Overlays argv onto an already-built EmuOptions (e.g. the result of
// parseOptionsFromEnvironment()). Unrecognized arguments are ignored
// (left for a future CLI-help pass to report, not this function's job).
void applyCommandLineArgs(EmuOptions& options, int argc, char** argv);

} // namespace beta::config
