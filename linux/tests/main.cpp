// The one TU that implements doctest's main for the Linux suite; every
// other file in tests/ just includes the vendored header.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "vendor/doctest.h"