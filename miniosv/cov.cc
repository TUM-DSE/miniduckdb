// MININET_COV=1 builds: the LLVM profile on the console when the program
// exits, zero runs collapsed and base64'd. scripts/cov/decode.py reverses it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" uint64_t __llvm_profile_get_size_for_buffer();
extern "C" int __llvm_profile_write_buffer(char *);

// The profile runtime is built fortified; the kernel's libc has no _chk entry.
extern "C" void *__memset_chk(void *d, int c, size_t n, size_t cap) {
	if (n > cap) {
		abort();
	}
	return memset(d, c, n);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

extern "C" void mininet_cov_dump() {
	std::vector<unsigned char> raw(__llvm_profile_get_size_for_buffer());
	if (__llvm_profile_write_buffer(reinterpret_cast<char *>(raw.data())) != 0) {
		printf("COV: write failed\n");
		return;
	}
	// A zero byte never stands alone: 0x00 is followed by the run's length.
	std::vector<unsigned char> rle;
	for (size_t i = 0; i < raw.size();) {
		if (raw[i] != 0) {
			rle.push_back(raw[i++]);
			continue;
		}
		size_t j = i;
		while (j < raw.size() && raw[j] == 0 && j - i < 0xffffffffu) {
			j++;
		}
		uint32_t n = j - i;
		rle.push_back(0);
		for (int k = 0; k < 4; k++) {
			rle.push_back(n >> (8 * k));
		}
		i = j;
	}
	printf("COV BEGIN raw=%zu rle=%zu\n", raw.size(), rle.size());
	// Twice: the console log is read in 64 KB snapshots and misses a few
	// lines per pass; the decoder merges what each pass got.
	for (int pass = 0; pass < 2; pass++) {
		unsigned lineno = 0;
		std::string line;
		for (size_t i = 0; i < rle.size(); i += 3) {
			size_t left = rle.size() - i;
			uint32_t v = rle[i] << 16 | (left > 1 ? rle[i + 1] : 0) << 8 | (left > 2 ? rle[i + 2] : 0);
			line += B64[v >> 18 & 63];
			line += B64[v >> 12 & 63];
			line += left > 1 ? B64[v >> 6 & 63] : '=';
			line += left > 2 ? B64[v & 63] : '=';
			if (line.size() >= 76 || left <= 3) {
				printf("COV:%u:%s\n", lineno++, line.c_str());
				line.clear();
				// EC2's console log keeps 64 KB and is read every 5 s: stay under 4 KB/s.
				if (lineno % 16 == 0) {
					usleep(320000);
				}
			}
		}
		printf("COV END lines=%u\n", lineno);
	}
}
