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
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "core/mem/heap/histogram.hh"
#include <mem.hh>
#include <osv/bootargs.hh>
#include <osv/mem/fault.hh>
#include <osv/sched.hh>

#include "modules/miniext/miniext.hh"
#include "modules/mininet/mininet.hh"

#include "duckdb.hpp"

// The benchmark runner's main(), renamed by miniosv.mk. C++ linkage, because
// after -Dmain=... it is an ordinary function.
int duckdb_benchmark_main(int argc, char **argv);
//! The CLI's main() takes const char **, hence the wrapper below.
int duckdb_shell_main(int argc, const char **argv);

// Profiling only: cumulative time DuckDB's threads spent blocked inside
// mininet::get(), defined in http/mininet_client.cpp next to the call site.
namespace duckdb {
uint64_t MininetWaitNs();
uint64_t MininetWaitCalls();
} // namespace duckdb

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
#ifndef MININET_TLS
#define MININET_TLS 1
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

// One row per column, pipe-separated, matching the format TPC-H's own answer
// files use (extension/tpch/dbgen -- TPCH_ANSWERS_SF*) so it can be compared
// against tpch_answers() without a bespoke parser on either side.
std::string format_pipe(duckdb::MaterializedQueryResult &r)
{
	std::string out;
	duckdb::idx_t cols = r.ColumnCount();
	for (duckdb::idx_t c = 0; c < cols; c++) {
		if (c) out += '|';
		out += r.names[c];
	}
	out += '\n';
	duckdb::idx_t rows = r.RowCount();
	for (duckdb::idx_t i = 0; i < rows; i++) {
		for (duckdb::idx_t c = 0; c < cols; c++) {
			if (c) out += '|';
			out += r.GetValue(c, i).ToString();
		}
		out += '\n';
	}
	return out;
}

std::string trim_trailing(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
		s.pop_back();
	}
	return s;
}

std::vector<std::vector<std::string>> split_pipe_rows(const std::string &s)
{
	std::vector<std::vector<std::string>> rows;
	std::vector<std::string> row;
	std::string field;
	auto flush_field = [&] {
		row.push_back(field);
		field.clear();
	};
	auto flush_row = [&] {
		flush_field();
		rows.push_back(row);
		row.clear();
	};
	for (char c : s) {
		if (c == '|') {
			flush_field();
		} else if (c == '\n') {
			flush_row();
		} else {
			field += c;
		}
	}
	if (!field.empty() || !row.empty()) {
		flush_row();
	}
	return rows;
}

// Field-by-field, numeric-tolerant: exact for flags/names, tolerant for
// numbers. Needed because the canned answers (baked from this DuckDB
// version's own dbgen) and the parquet `just setup apps/bench/duckdb-tpch`
// uploaded (generated by whatever `nix run nixpkgs#duckdb` resolves to) do
// not always agree on l_quantity's column type -- SUM(l_quantity) prints
// "37734107" from one dbgen generation and "37734107.00" from the other,
// same value. A real mismatch (wrong row, wrong flag, a sum off by more than
// float noise) still fails this.
bool answers_match(const std::string &want, const std::string &got)
{
	auto w = split_pipe_rows(want);
	auto g = split_pipe_rows(got);
	if (w.size() != g.size()) {
		return false;
	}
	for (size_t r = 0; r < w.size(); r++) {
		if (w[r].size() != g[r].size()) {
			return false;
		}
		for (size_t c = 0; c < w[r].size(); c++) {
			const std::string &a = w[r][c];
			const std::string &b = g[r][c];
			if (a == b) {
				continue;
			}
			char *ea, *eb;
			double da = strtod(a.c_str(), &ea);
			double db = strtod(b.c_str(), &eb);
			bool a_numeric = ea != a.c_str() && *ea == '\0';
			bool b_numeric = eb != b.c_str() && *eb == '\0';
			if (!a_numeric || !b_numeric) {
				return false;
			}
			double tol = 1e-6 * std::max(1.0, std::fabs(da));
			if (std::fabs(da - db) > tol) {
				return false;
			}
		}
	}
	return true;
}

// How many cpus actually did work, from the only side that cannot be argued
// with: each cpu's idle thread. A cpu whose idle thread consumed none of the
// wall clock was busy the whole step; one whose idle thread consumed all of it
// did nothing.
//
// This is the measurement the ladder was missing. `par_t1` -> `par_tall`
// reports a 1.40x speedup here against Linux's 11.6x on the same 32 vCPUs, so
// either the threads are not on 32 cpus or the cpus are not doing the work,
// and idle time tells those apart.
struct idle_sample {
	std::vector<uint64_t> ns;
};

idle_sample sample_idle()
{
	idle_sample out;
	out.ns.reserve(sched::cpus.size());
	for (auto *c : sched::cpus) {
		out.ns.push_back(c->idle_thread
		                     ? (uint64_t)c->idle_thread->thread_clock().count()
		                     : 0);
	}
	return out;
}

// "busy" is a cpu that spent under half the step idle. Half rather than some
// small fraction because the question is how many cpus carried the work, not
// how many were touched by it.
void report_idle(const char *name, const idle_sample &before,
                 const idle_sample &after, double wall_ms)
{
	unsigned busy = 0;
	double idle_ms_total = 0;
	for (size_t i = 0; i < before.ns.size() && i < after.ns.size(); i++) {
		double idle_ms = (double)(after.ns[i] - before.ns[i]) / 1e6;
		idle_ms_total += idle_ms;
		if (idle_ms < wall_ms / 2) {
			busy++;
		}
	}
	size_t n = before.ns.size();
	// Wall time times cpu count, less the idle, is the cpu time the step
	// actually consumed -- so divided by wall time it is the parallelism
	// achieved, on the same footing as the Linux arm's user_ms/real.
	double cpu_ms = wall_ms * (double)n - idle_ms_total;
	printf("CPUS: name=%s busy=%u of %zu parallelism=%.2f idle_ms=%.0f\n", name,
	       busy, n, wall_ms > 0 ? cpu_ms / wall_ms : 0.0, idle_ms_total);
}

// Broadcast TLB invalidation, as the workload actually paid for it.
//
// Every free of a mapping takes one global mutex and then blocks until all 31
// other cpus have answered an IPI, so the cost is the slowest cpu's interrupt
// latency and only one thread machine-wide can be paying it at a time. Summed
// wall time inside those calls is therefore directly comparable to the
// thread-time a query spends outside its reads -- which at sf=10 is 46 s
// against Linux's 8, and is the whole of the 2x.
//
// Printed by both executables because the question is the same either way:
// how much of this run went into it.
void print_tlb_stats()
{
	auto s = mem::mapping::tlb_shootdown_stats();
	printf("TLB STATS: shootdowns=%llu pages=%llu all=%llu ms_total=%.1f us_max=%.1f "
	       "us_avg=%.2f\n",
	       (unsigned long long)s.count, (unsigned long long)s.pages,
	       (unsigned long long)s.all, (double)s.ns_total / 1e6,
	       (double)s.ns_max / 1e3,
	       s.count ? (double)s.ns_total / (double)s.count / 1e3 : 0.0);

	// The other half of the memory bill. A workload that keeps touching
	// freshly mapped memory pays one of these per page it has not seen, and
	// unlike the shootdown above they are concurrent -- so ms_total here is
	// summed over cpus and can exceed the wall clock.
	auto f = mem::vm_fault_stats();
	printf("FAULT STATS: faults=%llu sigsegv=%llu ms_total=%.1f us_max=%.1f "
	       "ns_avg=%.0f\n",
	       (unsigned long long)f.count, (unsigned long long)f.sigsegv,
	       (double)f.ns_total / 1e6, (double)f.ns_max / 1e3,
	       f.count ? (double)f.ns_total / (double)f.count : 0.0);
}

// The aggregate over DuckDB's HTTP request log, in the arithmetic
// competitors/duckdb-linux/scripts/instance.py's http_profile() uses, so the
// two `HTTP STATS:` lines are the same measurement rather than two adjacent
// ones. Epoch microseconds rather than timestamp arithmetic: date parts of a
// TIMESTAMP_TZ want ICU, which is not linked into this image.
//
// `window_ms` is first-request-start to last-request-end, so ms_sum/window_ms
// is the mean number of requests in flight. That is the number a
// latency-bound query turns on and the one a per-request average cannot show:
// a thousand 40 ms requests are four seconds at ten in flight and one at
// forty.
constexpr const char *HTTP_STATS_SQL =
    "WITH r AS ("
    "  SELECT request.type AS type,"
    "         epoch_us(request.start_time) AS t0_us,"
    "         request.duration_ms AS ms"
    "  FROM duckdb_logs_parsed('HTTP')"
    ") "
    "SELECT count(*), "
    "count(*) FILTER (WHERE type = 'GET'), "
    "count(*) FILTER (WHERE type = 'HEAD'), "
    "round(avg(ms), 2), "
    "round(quantile_cont(ms, 0.5), 2), "
    "round(quantile_cont(ms, 0.9), 2), "
    "round(quantile_cont(ms, 0.99), 2), "
    "min(ms), max(ms), sum(ms)::DOUBLE, "
    "round((max(t0_us + ms * 1000) - min(t0_us)) / 1000.0, 2) "
    "FROM r";

void print_http_stats(duckdb::Connection &con, int qn)
{
	auto r = con.Query(HTTP_STATS_SQL);
	if (r->HasError()) {
		printf("HTTP STATS: FAILED %s\n", r->GetError().c_str());
		return;
	}
	if (r->RowCount() != 1) {
		printf("HTTP STATS: FAILED %llu rows\n", (unsigned long long)r->RowCount());
		return;
	}
	auto col = [&](size_t c) { return r->GetValue((duckdb::idx_t)c, 0).ToString(); };
	auto num = [&](size_t c) {
		auto v = r->GetValue((duckdb::idx_t)c, 0);
		return v.IsNull() ? 0.0 : v.GetValue<double>();
	};
	double sum_ms = num(9), window_ms = num(10);
	printf("HTTP STATS: q=%d n=%s get=%s head=%s ms_avg=%s ms_p50=%s ms_p90=%s "
	       "ms_p99=%s ms_min=%s ms_max=%s ms_sum=%s window_ms=%s concurrency=%.2f\n",
	       qn, col(0).c_str(), col(1).c_str(), col(2).c_str(), col(3).c_str(),
	       col(4).c_str(), col(5).c_str(), col(6).c_str(), col(7).c_str(),
	       col(8).c_str(), col(9).c_str(), col(10).c_str(),
	       window_ms > 0 ? sum_ms / window_ms : 0.0);
}

// TPC-H over the S3 bucket mininet is pointed at, read as views over
// read_parquet(), instead of the local-disk benchmark runner mk-tpch.sh
// feeds (that one exercises miniext, not the network). Scheme (http/https)
// follows MININET_TLS -- both dial the same bucket. The query
// text comes from the tpch extension's own PRAGMA -- no local query files to
// ship -- and so do the canned answers, when the scale factor has one
// (0.01, 0.1, 1; see extension/tpch/dbgen/dbgen.cpp).
//     tpch --sf 1 6          run Q06 at sf=1
//     tpch --sf 1 1 6 10     run Q01, Q06, Q10 at sf=1
//     tpch --sf 1            run all 22
int run_tpch(int argc, char **argv)
{
	double sf = 1.0;
	// 0 leaves DuckDB's own default: one thread per CPU. Worth being able to
	// set, because mininet's workers poll without yielding, so on a small
	// instance the workers and DuckDB's threads together can outnumber the
	// cores.
	int threads = 0;
	// DuckDB sizes max_memory from sysconf(_SC_PHYS_PAGES), which reports the
	// whole machine. A fixed cap on both arms is a better comparison than each
	// side guessing from its own view of it. Null leaves DuckDB's default.
	const char *memlimit = nullptr;
	// Turn on DuckDB's own HTTP request log and report its shape per query.
	// mininet already counts requests, but it counts them below httpfs; this
	// counts them where the Linux arm can count them too, so the two stacks'
	// per-request latency and in-flight depth become the same measurement
	// rather than two measurements of adjacent things. It costs a formatted
	// log record per request, so it is off unless asked for -- and the
	// numbers a run with it on produces are only comparable to the Linux
	// arm's `httplog` pass, not to a clean run on either side.
	bool http_log = false;
	std::vector<int> queries;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--sf") == 0 && i + 1 < argc) {
			sf = atof(argv[++i]);
		} else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			threads = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--memlimit") == 0 && i + 1 < argc) {
			memlimit = argv[++i];
		} else if (strcmp(argv[i], "--httplog") == 0) {
			http_log = true;
		} else if (argv[i][0] == '-') {
			// Refuse rather than fall through to the query parse below, where
			// atoi() would read "--memlimit 2GB" as a request to run Q02 and
			// the run would look ordinary while answering a different question.
			printf("FAIL: unknown option '%s'\n", argv[i]);
			return 1;
		} else {
			int n = atoi(argv[i]);
			if (n >= 1 && n <= 22) {
				queries.push_back(n);
			}
		}
	}
	if (queries.empty()) {
		for (int q = 1; q <= 22; q++) {
			queries.push_back(q);
		}
	}

	if (MININET_HOST[0] == '\0') {
		printf("FAIL: tpch needs a network -- build with "
		       "make app=duckdb MININET_HOST=<bucket>.s3.<region>.amazonaws.com\n");
		return 1;
	}

	// sf=1, not sf=1.000000: this has to match the tpch/sf<N>/ prefix `just
	// setup apps/bench/duckdb-tpch` uploaded to.
	char sf_str[32];
	if (sf == (double)(long long)sf) {
		snprintf(sf_str, sizeof(sf_str), "%lld", (long long)sf);
	} else {
		snprintf(sf_str, sizeof(sf_str), "%g", sf);
	}

	const char *scheme = MININET_TLS ? "https" : "http";
	printf("tpch: sf=%s, %zu quer%s, bucket %s (%s)\n", sf_str, queries.size(),
	       queries.size() == 1 ? "y" : "ies", MININET_HOST, scheme);

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);

	if (threads > 0) {
		char tsql[64];
		snprintf(tsql, sizeof(tsql), "SET threads=%d", threads);
		auto tr = con.Query(tsql);
		if (tr->HasError()) {
			printf("FAIL: %s: %s\n", tsql, tr->GetError().c_str());
			return 1;
		}
	}
	if (memlimit) {
		char msql[64];
		snprintf(msql, sizeof(msql), "SET memory_limit='%s'", memlimit);
		auto mr = con.Query(msql);
		if (mr->HasError()) {
			printf("FAIL: %s: %s\n", msql, mr->GetError().c_str());
			return 1;
		}
	}
	// What DuckDB settled on, not what was asked for: the reason to print it
	// is to catch the guest's idea of the machine disagreeing with DuckDB's.
	{
		auto mr = con.Query("SELECT current_setting('memory_limit')");
		if (!mr->HasError() && mr->RowCount() == 1) {
			printf("memory: limit=%s\n", mr->GetValue(0, 0).ToString().c_str());
		}
	}
	printf("cpus: hw_concurrency=%u duckdb_threads=%llu workers=%d conns=%d\n",
	       std::thread::hardware_concurrency(), (unsigned long long)db.NumberOfThreads(),
	       MININET_WORKERS, MININET_CONNS);

	static const char *tables[] = {"customer", "lineitem", "nation",  "orders",
	                                "part",     "partsupp", "region", "supplier"};
	for (const char *t : tables) {
		char sql[512];
		snprintf(sql, sizeof(sql),
		         "CREATE VIEW %s AS SELECT * FROM "
		         "read_parquet('%s://%s/tpch/sf%s/%s.parquet');",
		         t, scheme, MININET_HOST, sf_str, t);
		auto r = con.Query(sql);
		if (r->HasError()) {
			printf("FAIL: view %s: %s\n", t, r->GetError().c_str());
			return 1;
		}
	}

	// Turned on after the views exist, so the parquet footers the schema
	// needed are not counted against the first query. The Linux arm creates
	// its views in an earlier process, so a handful of footer reads land
	// inside its query where they land outside ours; the printed counts are
	// what makes that visible rather than something to be assumed away.
	//
	// Order: storage first, because the default sink is stdout and a
	// formatted record per request over a serial console would be both noise
	// and a slowdown; enable last, because every SET between the two is
	// itself a log record.
	if (http_log) {
		static const char *const on[] = {
		    "SET logging_storage='memory'",
		    "SET logging_level='debug'",
		    "SET logging_mode='ENABLE_SELECTED'",
		    "SET enabled_log_types='HTTP'",
		    "SET enable_logging=true",
		};
		for (const char *stmt : on) {
			auto r = con.Query(stmt);
			if (r->HasError()) {
				printf("FAIL: %s: %s\n", stmt, r->GetError().c_str());
				return 1;
			}
		}
		printf("httplog: on (query timings carry a log record per request)\n");
	}

	int ok = 0, checked = 0, matched = 0;
	double total_ms = 0, total_net_ms = 0;
	for (int qn : queries) {
		char pragma[32];
		snprintf(pragma, sizeof(pragma), "PRAGMA tpch(%d)", qn);

		// Each query's own requests, not the run's: without this a second
		// query's stats would include the first's.
		if (http_log) {
			auto tr = con.Query("CALL truncate_duckdb_logs()");
			if (tr->HasError()) {
				printf("httplog: truncate failed: %s\n", tr->GetError().c_str());
			}
		}

		uint64_t net0 = duckdb::MininetWaitNs();
		uint64_t calls0 = duckdb::MininetWaitCalls();
		auto idle0 = sample_idle();
		auto t0 = std::chrono::steady_clock::now();
		auto r = con.Query(pragma);
		auto t1 = std::chrono::steady_clock::now();
		auto idle1 = sample_idle();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		double net_ms = (duckdb::MininetWaitNs() - net0) / 1e6;
		uint64_t net_calls = duckdb::MininetWaitCalls() - calls0;
		total_ms += ms;
		total_net_ms += net_ms;

		if (r->HasError()) {
			printf("Q%02d: FAIL %.1f ms: %s\n", qn, ms, r->GetError().c_str());
			continue;
		}
		ok++;

		// Best-effort correctness: tpch_answers() only knows sf 0.01/0.1/1, so
		// anything else is timed but unchecked -- same coverage mk-tpch.sh's
		// answers/ directory has.
		const char *match = "unchecked";
		char asql[128];
		snprintf(asql, sizeof(asql),
		         "SELECT answer FROM tpch_answers() WHERE query_nr = %d AND scale_factor = %s",
		         qn, sf_str);
		auto ares = con.Query(asql);
		if (!ares->HasError() && ares->RowCount() == 1) {
			checked++;
			std::string want = trim_trailing(ares->GetValue(0, 0).ToString());
			std::string got = trim_trailing(format_pipe(*r));
			bool eq = answers_match(want, got);
			match = eq ? "yes" : "no";
			if (eq) {
				matched++;
			} else {
				printf("Q%02d mismatch\n  want: %s\n  got:  %s\n", qn,
				       want.c_str(), got.c_str());
			}
		}

		printf("Q%02d: %.1f ms, %llu rows, match=%s, net_ms=%.1f, net_calls=%llu\n", qn, ms,
		       (unsigned long long)r->RowCount(), match, net_ms, (unsigned long long)net_calls);

		// How many cpus the query itself used. The ladder measures this for
		// a step with no I/O in it; here most threads are waiting on S3 most
		// of the time, so the number to read is `parallelism` against what
		// the same run reports as time inside HTTP -- the two together say
		// whether the compute stage or the wire is the constraint.
		{
			char name[16];
			snprintf(name, sizeof(name), "q%02d", qn);
			report_idle(name, idle0, idle1, ms);
		}

		// After the answer check, not before: the check runs a query of its
		// own, and tpch_answers() is local, so it adds no requests -- but
		// printing last keeps the `Qnn:` line adjacent to its own stats.
		if (http_log) {
			print_http_stats(con, qn);
		}
	}

	printf("\nTPCH SUMMARY: ok=%d total=%zu ms=%.1f net_ms=%.1f checked=%d matched=%d\n",
	       ok, queries.size(), total_ms, total_net_ms, checked, matched);

	mininet::conn_stats cs = mininet::stats();
	printf("CONN STATS: requests=%llu reused=%llu\n",
	       (unsigned long long)cs.requests_served, (unsigned long long)cs.requests_reused);

	unsigned long long reqs = cs.requests_served ? cs.requests_served : 1;
	unsigned long long polls = cs.poll_iters ? cs.poll_iters : 1;
	unsigned long long conns = cs.conns_established ? cs.conns_established : 1;
	unsigned long long drains = cs.recv_drains ? cs.recv_drains : 1;
	unsigned long long txc = cs.tx_calls ? cs.tx_calls : 1;
	unsigned long long tn = cs.ttfb_count ? cs.ttfb_count : 1;
	unsigned long long xn = cs.xfer_count ? cs.xfer_count : 1;
	printf("REQ STATS: queue_us_avg=%llu wire_us_avg=%llu bytes=%llu mb_per_s=%.1f\n",
	       (unsigned long long)(cs.queue_wait_us_total / reqs),
	       (unsigned long long)(cs.wire_us_total / reqs),
	       (unsigned long long)cs.body_bytes_total,
	       cs.wire_us_total ? (double)cs.body_bytes_total / (double)cs.wire_us_total : 0.0);
	printf("POLL STATS: iters=%llu gap_us_avg=%.2f gap_us_max=%llu gaps_over_1ms=%llu\n",
	       (unsigned long long)cs.poll_iters, (double)cs.poll_gap_us_total / (double)polls,
	       (unsigned long long)cs.poll_gap_us_max, (unsigned long long)cs.poll_gaps_over_1ms);
	printf("SETUP STATS: conns=%llu failed=%llu syn_retries=%llu setup_us_avg=%llu\n",
	       (unsigned long long)cs.conns_established, (unsigned long long)cs.conns_failed,
	       (unsigned long long)cs.syn_retries, (unsigned long long)(cs.setup_us_total / conns));
	printf("RECV STATS: drains=%llu drain_avg=%llu queue_max=%llu\n",
	       (unsigned long long)cs.recv_drains,
	       (unsigned long long)(cs.recv_drain_bytes / drains),
	       (unsigned long long)cs.recv_queue_max);
	printf("BUF STATS: cap_max=%llu calls=%llu retried=%llu\n",
	       (unsigned long long)cs.body_cap_max, (unsigned long long)cs.body_cap_calls,
	       (unsigned long long)cs.requests_retried);
	printf("TX STATS: calls=%llu tx_ns_avg=%llu tx_ns_max=%llu\n",
	       (unsigned long long)cs.tx_calls, (unsigned long long)(cs.tx_ns_total / txc),
	       (unsigned long long)cs.tx_ns_max);
	unsigned long long wn = cs.wake_count ? cs.wake_count : 1;
	printf("WAKE STATS: n=%llu ns_avg=%llu us_max=%.1f ms_total=%.1f\n",
	       (unsigned long long)cs.wake_count,
	       (unsigned long long)(cs.wake_ns_total / wn),
	       (double)cs.wake_ns_max / 1e3, (double)cs.wake_ns_total / 1e6);
	printf("LATENCY STATS: ttfb_us_avg=%llu n=%llu xfer_us_avg=%llu n=%llu\n",
	       (unsigned long long)(cs.ttfb_us_total / tn), (unsigned long long)cs.ttfb_count,
	       (unsigned long long)(cs.xfer_us_total / xn), (unsigned long long)cs.xfer_count);
	printf("DROP STATS: imissed=%llu ierrors=%llu rx_nombuf=%llu misrouted=%llu "
	       "tx_alloc_fail=%llu tx_burst_fail=%llu ipackets=%llu ibytes=%llu\n",
	       (unsigned long long)cs.nic_imissed, (unsigned long long)cs.nic_ierrors,
	       (unsigned long long)cs.nic_rx_nombuf, (unsigned long long)cs.misrouted_drops,
	       (unsigned long long)cs.tx_alloc_fail, (unsigned long long)cs.tx_burst_fail,
	       (unsigned long long)cs.nic_ipackets, (unsigned long long)cs.nic_ibytes);

	print_tlb_stats();

	bool complete = ok == (int)queries.size() && matched == checked;
	printf("%s: tpch sf=%s ok=%d/%zu\n", complete ? "COMPLETE" : "INCOMPLETE",
	       sf_str, ok, queries.size());
	return complete ? 0 : 1;
}

// Where the TPC-H-over-S3 gap actually is, asked without a network.
//
// The matched HTTP-log measurement says the two stacks fetch the same 979
// ranges at the same per-request latency (miniOSv p50 31 ms against Linux's
// 27, and a far shorter tail), and that miniOSv nonetheless keeps only 13.8
// requests in flight where Linux keeps 25.7. With 32 threads on both sides
// and reads issued serially inside each scan task, in-flight depth is
// threads x read/(read + everything-else), so a lower depth at equal latency
// means the work *between* reads is slower -- roughly 47 ms per request
// against Linux's 8. That is CPU, and none of it needs S3 to measure.
//
// The ladder separates the candidates, cheapest first:
//
//   range_scan  vectorized arithmetic over 1e9 values, 32-way, allocating
//               almost nothing. Slow here means cores or scheduling.
//   hash_agg    the same shape plus a per-thread hash table and its merge.
//   dbgen       building three TPC-H tables in memory: the allocator, under
//               the widest size distribution DuckDB ever asks it for.
//   q01/q06     the queries themselves against in-memory tables, so the same
//               plan as the S3 runs with the parquet layer taken out.
//
// Sized for ~2-10 s each on 32 cores, which is long enough to swamp the
// millisecond-resolution console clock and short enough to leave the run
// inside one instance's life.
struct probe_step {
	const char *name;
	//! Run before the timer starts, or null. Setup that is not the thing
	//! being measured -- changing the thread count, mostly.
	const char *pre;
	const char *sql;
};

// One expensive predicate per lineitem row, run twice: once on one thread and
// once on all of them. The ratio is the parallel speedup this kernel actually
// delivers, which is the number the sf=10 gap now turns on -- miniOSv issues
// S3 requests at a flat ~0.37/ms whatever the latency does, against Linux's
// 0.42-0.78, and a fixed rate with 32 threads means fewer threads are really
// running than were asked for.
//
// It has to be a table scan, not range(): range() parallelises to about 1.5
// threads on either stack, so the earlier range_scan and hash_agg steps
// measured single-core speed (1.23x and 1.81x) and could not have seen this.
// lineitem at sf=1 is 6M rows over ~49 row groups, which is enough morsels
// for 32 threads.
#define PROBE_PREDICATE                                                        \
	"SELECT count(*) FROM lineitem WHERE "                                 \
	"ln(1+abs(sin(l_extendedprice))) + ln(1+abs(cos(l_quantity))) + "      \
	"ln(1+abs(sin(l_discount))) + ln(1+abs(cos(l_tax))) + "                \
	"sqrt(abs(l_extendedprice)) + sqrt(abs(l_quantity)) + "                \
	"ln(2+abs(sin(l_tax))) + sqrt(1+abs(l_discount)) > -1"

const probe_step probe_steps[] = {
    // First: everything below reads the tables it builds.
    {"dbgen", nullptr, "CALL dbgen(sf=1)"},
    // Single-core arithmetic, for a per-core speed number that owes nothing
    // to scheduling.
    {"range_scan", nullptr,
     "SELECT count(*) FROM range(1000000000) WHERE range % 7 = 0"},
    {"hash_agg", nullptr,
     "SELECT count(*) FROM (SELECT range % 1000 AS k, sum(range) "
     "FROM range(250000000) GROUP BY k)"},
    // The pair that matters. par_t1 / par_tall is the achieved speedup.
    {"par_t1", "SET threads=1", PROBE_PREDICATE},
    {"par_tall", "RESET threads", PROBE_PREDICATE},
    // The queries themselves, with the parquet layer taken out.
    {"q01_local", nullptr, "PRAGMA tpch(1)"},
    {"q06_local", nullptr, "PRAGMA tpch(6)"},
};

// No --threads: the ladder owns the thread count, because `par_t1` and
// `par_tall` mean "one thread" and "one per cpu" and a caller-supplied value
// would make the second of those a different question -- and `RESET threads`
// would then undo it rather than restore it.
int run_cpuprobe(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		printf("FAIL: cpuprobe takes no arguments, got '%s'\n", argv[i]);
		return 1;
	}

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	{
		auto mr = con.Query("SELECT current_setting('memory_limit')");
		if (!mr->HasError() && mr->RowCount() == 1) {
			printf("memory: limit=%s\n", mr->GetValue(0, 0).ToString().c_str());
		}
	}
	printf("cpus: hw_concurrency=%u duckdb_threads=%llu workers=%d conns=%d\n",
	       std::thread::hardware_concurrency(), (unsigned long long)db.NumberOfThreads(),
	       MININET_WORKERS, MININET_CONNS);

	int ok = 0;
	for (const auto &step : probe_steps) {
		if (step.pre) {
			auto pr = con.Query(step.pre);
			if (pr->HasError()) {
				printf("PROBE: name=%s ms=0.0 FAILED pre(%s): %s\n", step.name,
				       step.pre, pr->GetError().c_str());
				continue;
			}
		}
		auto idle0 = sample_idle();
		auto t0 = std::chrono::steady_clock::now();
		auto r = con.Query(step.sql);
		auto t1 = std::chrono::steady_clock::now();
		auto idle1 = sample_idle();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		report_idle(step.name, idle0, idle1, ms);
		if (r->HasError()) {
			printf("PROBE: name=%s ms=%.1f FAILED %s\n", step.name, ms,
			       r->GetError().c_str());
			continue;
		}
		// The first cell, so a step that returned instantly because it
		// computed nothing cannot pass for a fast one.
		std::string first = r->RowCount() && r->ColumnCount()
		                        ? r->GetValue(0, 0).ToString()
		                        : "-";
		printf("PROBE: name=%s ms=%.1f rows=%llu first=%s\n", step.name, ms,
		       (unsigned long long)r->RowCount(), first.c_str());
		ok++;
	}

	print_tlb_stats();

	int total = (int)(sizeof(probe_steps) / sizeof(probe_steps[0]));
	printf("%s: cpuprobe ok=%d/%d\n", ok == total ? "COMPLETE" : "INCOMPLETE", ok, total);
	return ok == total ? 0 : 1;
}

const executable executables[] = {
	{"sql", run_sql},
	{"benchmark", duckdb_benchmark_main},
	{"duckdb", run_cli},
	{"tpch", run_tpch},
	{"cpuprobe", run_cpuprobe},
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
		net.tls = MININET_TLS;
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
