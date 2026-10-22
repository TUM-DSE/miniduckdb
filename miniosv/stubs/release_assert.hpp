// DuckDB's D_ASSERT as a release build has it: nothing. Upstream gets there with
// NDEBUG, which turns assert() off; the kernel's assert.h stays live whenever
// _KERNEL is defined, so here D_ASSERT is redefined after assert.hpp instead.
// Its argument is never evaluated, as with NDEBUG, so helpers that only exist
// under D_ASSERT_IS_ENABLED are not needed either.
#pragma once

#include "duckdb/common/assert.hpp"

#undef D_ASSERT
#define D_ASSERT(condition) ((void)0)
