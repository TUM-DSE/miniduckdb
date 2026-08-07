// DuckDB on miniOSv: the application entry point.
//
// A placeholder for now -- it brings the engine up, mounts the data disk with
// miniext and runs one query, which is enough to prove the build links and the
// filesystem is reachable. The serial-console REPL is Step 9.

#include <cstdio>

#include "duckdb.hpp"
#include "modules/miniext/miniext.hh"

extern "C" void osv_app_main()
{
    printf("\n######## DuckDB on miniOSv ########\n\n");

    // NVMe controller 1 is the --emulated-nvme data disk; 0 is the boot image.
    int rc = miniext::mount(1, "/db");
    if (rc < 0) {
        printf("miniext: mount failed (%d); continuing in memory only\n", rc);
    }

    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    auto result = con.Query("SELECT 42 AS answer");
    if (result->HasError()) {
        printf("query failed: %s\n", result->GetError().c_str());
    } else {
        printf("SELECT 42 -> %s\n", result->ToString().c_str());
    }

    printf("\n######## DuckDB on miniOSv: engine up ########\n");
    while (true) {
        asm volatile("" ::: "memory");
    }
}
