/*
 * DuckDB on miniOSv: the application entry point.
 *
 * miniOSv links one application into the kernel and enters it through
 * osv_app_main(), so which program runs is decided here rather than by exec'ing
 * a binary. The first word of the boot-disk argument block names the
 * executable and the rest becomes its argv:
 *
 *     scripts/run.py --args "benchmark --list"
 *     scripts/run.py --args "duckdb -c 'SELECT 42'"
 *
 * Both executables are upstream DuckDB programs with their own main(), reached
 * through the thin wrappers below.
 */

#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include "core/mem/heap/histogram.hh"
#include <osv/bootargs.hh>

#include "modules/miniext/miniext.hh"
#include "modules/mininet/mininet.hh"

#include "duckdb.hpp"

// The benchmark runner's main(), renamed by miniosv.mk. C++ linkage, because
// after -Dmain=... it is an ordinary function.
int duckdb_benchmark_main(int argc, char **argv);
//! The CLI's main() takes const char **, hence the wrapper below.
int duckdb_shell_main(int argc, const char **argv);

namespace {

// The endpoint mininet is brought up for, baked in at build time because the
// guest has no resolver. Empty host means "no network"; see miniosv.mk.
#ifndef MININET_HOST
#define MININET_HOST ""
#endif
#ifndef MININET_ADDR
#define MININET_ADDR "0.0.0.0"
#endif
#ifndef MININET_WORKERS
#define MININET_WORKERS 2
#endif
#ifndef MININET_CONNS
#define MININET_CONNS 8
#endif

// NVMe controller 1 is the data disk (run.py --emulated-nvme); 0 is the boot
// disk. Mounting is best-effort so an image with no data disk still starts.
const int DATA_NVME_ID = 1;
const char *MOUNT_POINT = "/db";

struct executable {
	const char *name;
	int (*run)(int argc, char **argv);
};

// Runs SQL against a real on-disk database through MiniextFileSystem. This is
// the scaffolding the benchmark runner and the CLI plug into once their
// LocalFileSystem dependency is settled; see PLAN_duckdb.md.
int run_sql(int argc, char **argv)
{
	const char *database = nullptr;
	std::vector<const char *> statements;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			database = argv[++i];
		} else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			statements.push_back(argv[++i]);
		}
	}

	// No file_system override: DuckDB's default is
	// VirtualFileSystem(FileSystem::CreateLocal()), and CreateLocal() now
	// returns the miniext-backed LocalFileSystem.
	duckdb::DuckDB db(database);
	duckdb::Connection con(db);

	if (statements.empty()) {
		statements.push_back("SELECT 42 AS answer");
	}
	for (const char *sql : statements) {
		printf("\n> %s\n", sql);
		auto result = con.Query(sql);
		if (result->HasError()) {
			printf("Error: %s\n", result->GetError().c_str());
		} else {
			printf("%s\n", result->ToString().c_str());
		}
	}
	return 0;
}

int run_cli(int argc, char **argv)
{
	return duckdb_shell_main(argc, const_cast<const char **>(argv));
}

const executable executables[] = {
	{"sql", run_sql},
	{"benchmark", duckdb_benchmark_main},
	{"duckdb", run_cli},
};

const executable *find_executable(const char *name)
{
	for (const auto &e : executables) {
		if (strcmp(e.name, name) == 0) {
			return &e;
		}
	}
	return nullptr;
}

void usage()
{
	printf("executables:");
	for (const auto &e : executables) {
		printf(" %s", e.name);
	}
	printf("\nset them with: scripts/run.py --args \"<executable> [args...]\"\n");
}

} // namespace

extern "C" void osv_app_main()
{
	printf("\n######## DuckDB on miniOSv ########\n\n");

	int rc = miniext::mount(DATA_NVME_ID, MOUNT_POINT);
	if (rc < 0) {
		printf("miniext: no data disk (%d); continuing without one\n", rc);
	}

	// Best-effort, like the mount above: an image with no NIC still runs
	// everything that does not name an http:// or s3:// path. There is no
	// resolver, so the endpoint is compiled in -- see the build variables in
	// miniosv/miniosv.mk.
	if (MININET_HOST[0] != '\0') {
		mininet::config net {};
		net.host = MININET_HOST;
		net.address = MININET_ADDR;
		net.tls = 1;
		net.workers = MININET_WORKERS;
		net.conns_per_worker = MININET_CONNS;
		net.rx_buffer = 0;
		int nrc = mininet::up(net);
		if (nrc != mininet::OK) {
			printf("mininet: %s; continuing without a network\n", mininet::strerror(nrc));
		} else {
			printf("mininet: serving %s\n", MININET_HOST);
		}
		// Whether signed S3 access is possible at all comes down to this
		// number: SigV4 refuses a request whose x-amz-date is more than 15
		// minutes out. TLS working proves far less than it looks, because
		// certificate validity windows are months wide.
		printf("clock: unix %llu\n", (unsigned long long)time(nullptr));
	}

	std::vector<std::string> words = osv::bootargs_split(osv::bootargs());
	if (words.empty()) {
		printf("no boot arguments.\n");
		usage();
		while (true) {
			asm volatile("" ::: "memory");
		}
	}

	const executable *exe = find_executable(words[0].c_str());
	if (!exe) {
		printf("no executable named '%s'.\n", words[0].c_str());
		usage();
		while (true) {
			asm volatile("" ::: "memory");
		}
	}

	// argv[0] is the executable name, as a program expects.
	std::vector<char *> argv;
	for (auto &w : words) {
		argv.push_back(const_cast<char *>(w.c_str()));
	}
	argv.push_back(nullptr);

	int status = exe->run(static_cast<int>(argv.size()) - 1, argv.data());
	printf("\n%s exited with %d\n", exe->name, status);

	// Prints the allocation histogram when the kernel was built with
	// conf_memory_histogram=1, and nothing otherwise.
	mem::heap::histogram_dump();

	// Do not power off: keep the serial output visible on the console.
	while (true) {
		asm volatile("" ::: "memory");
	}
}
