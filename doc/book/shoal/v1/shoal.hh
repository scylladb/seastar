// Declarations that all of Shoal's source files share.
#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/log.hh>

// snippet: using
// Shoal uses Seastar's names without the seastar:: prefix.
using namespace seastar;
// end snippet: using

extern logger shoal_log;
