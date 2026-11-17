// Runs the fuzz harness on saved inputs, without libFuzzer: tests/fuzz_regressions holds the smallest input found
// for each bug the fuzzer has found, so they stay fixed. Usage: md_mpeg_fuzz_replay <file or directory>...

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(const int argc, char *argv[]) {
	namespace fs = std::filesystem;
	int count = 0;
	for (int i = 1; i < argc; i++) {
		std::vector<fs::path> files;
		if (fs::is_directory(argv[i])) {
			for (const auto &entry : fs::directory_iterator(argv[i])) {
				files.push_back(entry.path());
			}
		} else {
			files.emplace_back(argv[i]);
		}
		for (const auto &file : files) {
			std::ifstream in(file, std::ios::binary);
			const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			std::printf("%s\n", file.string().c_str());
			std::fflush(stdout);
			LLVMFuzzerTestOneInput(data.data(), data.size());
			count++;
		}
	}
	std::printf("%d inputs ran\n", count);
	return count ? 0 : 1;
}
