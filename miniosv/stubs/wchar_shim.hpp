// miniOSv: wide-character shim for DuckDB.
//
// libc++ is built narrow-only for this kernel (scripts/build-libcxx.sh), so
// std::wstring, std::wstringstream and std::char_traits<wchar_t> do not exist.
// Nothing in the image uses wide characters at runtime, but DuckDB does not
// need to *call* them to fail to build: the vendored fmt names std::wstring in
// non-dependent positions (e.g. `template <typename T> std::wstring
// to_wstring(const T&)`) and explicitly instantiates its wchar_t machinery in
// third_party/fmt/format.cc. Both need these types complete at parse time.
//
// This shim supplies exactly enough for that code to compile. It is
// force-included with -include on DuckDB translation units only, so the rest
// of the kernel keeps libc++'s narrow-only view.
//
// Providing the whole char_traits surface rather than the four typedefs is
// what lets format.cc build unmodified -- otherwise its wchar_t instantiations
// have to be #ifdef'd out, which means patching upstream source. Nothing below
// is ever executed.
//
// Extending namespace std with new names is formally UB. The alternative is
// carrying patches against upstream fmt and <sstream>, which is worse to
// maintain and no better defined in practice.

#pragma once

#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace std {

// libc++'s basic_string only static_asserts that traits_type::char_type
// matches, so the typedefs alone make it instantiable. The member functions
// exist because fmt's explicit instantiations reference them.
template <>
struct char_traits<wchar_t> {
    using char_type  = wchar_t;
    using int_type   = long;
    using off_type   = long long;
    using pos_type   = long long;
    using state_type = int;

    static void assign(char_type &a, const char_type &b) noexcept { a = b; }
    static bool eq(char_type a, char_type b) noexcept { return a == b; }
    static bool lt(char_type a, char_type b) noexcept { return a < b; }

    static size_t length(const char_type *s) noexcept
    {
        size_t n = 0;
        while (s[n]) {
            n++;
        }
        return n;
    }

    static int compare(const char_type *a, const char_type *b, size_t n) noexcept
    {
        for (size_t i = 0; i < n; i++) {
            if (a[i] < b[i]) {
                return -1;
            }
            if (b[i] < a[i]) {
                return 1;
            }
        }
        return 0;
    }

    static const char_type *find(const char_type *s, size_t n,
                                 const char_type &c) noexcept
    {
        for (size_t i = 0; i < n; i++) {
            if (s[i] == c) {
                return s + i;
            }
        }
        return nullptr;
    }

    static char_type *move(char_type *d, const char_type *s, size_t n) noexcept
    {
        return static_cast<char_type *>(memmove(d, s, n * sizeof(char_type)));
    }

    static char_type *copy(char_type *d, const char_type *s, size_t n) noexcept
    {
        return static_cast<char_type *>(memcpy(d, s, n * sizeof(char_type)));
    }

    static char_type *assign(char_type *s, size_t n, char_type c) noexcept
    {
        for (size_t i = 0; i < n; i++) {
            s[i] = c;
        }
        return s;
    }

    static int_type not_eof(int_type c) noexcept { return c == eof() ? 0 : c; }
    static char_type to_char_type(int_type c) noexcept
    {
        return static_cast<char_type>(c);
    }
    static int_type to_int_type(char_type c) noexcept
    {
        return static_cast<int_type>(c);
    }
    static bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
    static int_type eof() noexcept { return static_cast<int_type>(-1); }
};

using wstring = basic_string<wchar_t>;
using wstringstream = basic_stringstream<wchar_t>;
// catch.hpp names this in a StringMaker specialisation.
using wstring_view = basic_string_view<wchar_t>;

} // namespace std
