/*
 * Symbols DuckDB references that miniOSv's libc does not provide.
 *
 * Two kinds live here, and they are treated differently:
 *
 *   - Double-precision hyperbolics and gamma. llvm-libc ships the float and
 *     _Float16 variants (external/llvm-libc-config/entrypoints.txt has sinhf
 *     and sinhf16, but no sinh), while SQL's SINH/COSH/LGAMMA are double.
 *     These are implemented, not stubbed: a query that calls them should
 *     return a number, not halt the machine.
 *
 *   - FILE-based and path-canonicalising calls (fseek/ftell/realpath). DuckDB
 *     reaches these only on paths miniOSv genuinely cannot serve -- there is no
 *     fd table and no notion of a current directory. They fail with errno
 *     rather than panicking: an application should get an error it can report,
 *     and halting the machine on a stray SQL function is a poor trade.
 *     yyjson's yyjson_read_fp and DuckDB's path canonicalisation are the only
 *     callers, and neither is on a path we exercise.
 */

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {

// --- double-precision math llvm-libc omits ---------------------------------
//
// Computed via the float-argument reductions of the standard identities, in
// double throughout. expm1/log1p are present in llvm-libc, and using them
// keeps the small-argument cases from cancelling catastrophically.

double sinh(double x)
{
	// (e^x - e^-x)/2, written so |x| near zero does not lose precision.
	if (x == 0.0) {
		return x;                       // preserves -0.0
	}
	const double e = expm1(fabs(x));
	const double r = 0.5 * (e + e / (e + 1.0));
	return x < 0 ? -r : r;
}

double cosh(double x)
{
	const double e = exp(fabs(x));
	return 0.5 * (e + 1.0 / e);
}

double tanh(double x)
{
	if (x == 0.0) {
		return x;
	}
	const double e = expm1(-2.0 * fabs(x));
	const double r = -e / (e + 2.0);
	return x < 0 ? -r : r;
}

double asinh(double x)
{
	const double a = fabs(x);
	const double r = log1p(a + a * a / (1.0 + sqrt(1.0 + a * a)));
	return x < 0 ? -r : r;
}

double acosh(double x)
{
	if (x < 1.0) {
		errno = EDOM;
		return NAN;
	}
	return log(x + sqrt((x - 1.0) * (x + 1.0)));
}

double atanh(double x)
{
	const double a = fabs(x);
	if (a > 1.0) {
		errno = EDOM;
		return NAN;
	}
	if (a == 1.0) {
		errno = ERANGE;
		return x < 0 ? -INFINITY : INFINITY;
	}
	const double r = 0.5 * log1p(2.0 * a / (1.0 - a));
	return x < 0 ? -r : r;
}

// lgamma via the Lanczos approximation (g = 7, n = 9). Accurate to roughly
// 1e-13 relative, which is well beyond what SQL callers need.
static const double lanczos_g = 7.0;
static const double lanczos_c[9] = {
	0.99999999999980993, 676.5203681218851, -1259.1392167224028,
	771.32342877765313, -176.61502916214059, 12.507343278686905,
	-0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7,
};

double lgamma(double x)
{
	if (x < 0.5) {
		// Reflection: Gamma(x)Gamma(1-x) = pi / sin(pi x)
		const double s = sin(M_PI * x);
		if (s == 0.0) {
			errno = ERANGE;
			return INFINITY;
		}
		return log(M_PI / fabs(s)) - lgamma(1.0 - x);
	}

	const double z = x - 1.0;
	double a = lanczos_c[0];
	for (int i = 1; i < 9; i++) {
		a += lanczos_c[i] / (z + i);
	}
	const double t = z + lanczos_g + 0.5;
	return 0.5 * log(2.0 * M_PI) + (z + 0.5) * log(t) - t + log(a);
}

int signgam;

double tgamma(double x)
{
	if (x == 0.0) {
		errno = ERANGE;
		return x < 0 ? -INFINITY : INFINITY;
	}
	if (x < 0 && x == floor(x)) {
		errno = EDOM;                   // poles at the non-positive integers
		return NAN;
	}
	if (x < 0.5) {
		const double s = sin(M_PI * x);
		return M_PI / (s * tgamma(1.0 - x));
	}
	const double lg = lgamma(x);
	return exp(lg);
}

// --- calls miniOSv cannot serve --------------------------------------------

// There is no fd table and no seekable stream: libc/stdio/llvm_stdio.cc only
// backs the three standard streams, and fseeko/ftello there already return
// ESPIPE. Reached only from yyjson's yyjson_read_fp, which nothing calls.
int fseek(FILE *, long, int)
{
	errno = ESPIPE;
	return -1;
}

long ftell(FILE *)
{
	errno = ESPIPE;
	return -1;
}

// No current directory and no path namespace outside miniext, which DuckDB
// reaches through MiniextFileSystem rather than through libc. DuckDB's
// FileSystem::CanonicalizePath does pure string manipulation and never gets
// here as long as absolute paths are used.
char *realpath(const char *, char *)
{
	errno = ENOSYS;
	return nullptr;
}

} // extern "C"
