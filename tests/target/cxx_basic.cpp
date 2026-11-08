// arc_os target program (M0-04): the C++ runtime under musl (static libstdc++).
//
// Exceptions, std::thread, std::mutex/condition_variable, std::filesystem and
// iostream: the C++ features llama.cpp relies on. Exit status 0 = pass.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; failures++; } \
} while (0)

static void thrower(int depth) {
    if (depth == 0) throw std::runtime_error("deep");
    thrower(depth - 1);
}

int main() {
    // Exceptions across frames, and a standard library exception
    try {
        thrower(10);
        CHECK(false);
    } catch (const std::runtime_error &e) {
        CHECK(std::string(e.what()) == "deep");
    }
    try {
        std::vector<int> v(3);
        (void)v.at(5);
        CHECK(false);
    } catch (const std::out_of_range &) {
    }

    // Threads, mutex, condition variable, atomics
    std::mutex m;
    std::condition_variable cv;
    bool go = false;
    std::atomic<long> sum{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < 4; i++) {
        pool.emplace_back([&, i] {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return go; });
            lk.unlock();
            for (int k = 0; k < 1000; k++) sum += i;
        });
    }
    {
        std::lock_guard<std::mutex> lk(m);
        go = true;
    }
    cv.notify_all();
    for (auto &t : pool) t.join();
    CHECK(sum == 1000L * (0 + 1 + 2 + 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(std::thread::hardware_concurrency() >= 1);

    // Filesystem and file streams
    fs::path dir = fs::temp_directory_path() / "arc_cxx_test";
    fs::remove_all(dir);
    CHECK(fs::create_directories(dir / "a" / "b"));
    {
        std::ofstream out(dir / "a" / "data.txt");
        out << "hello " << 42 << "\n";
    }
    CHECK(fs::exists(dir / "a" / "data.txt"));
    CHECK(fs::file_size(dir / "a" / "data.txt") == 9);
    int entries = 0;
    for (auto &e : fs::recursive_directory_iterator(dir)) {
        (void)e;
        entries++;
    }
    CHECK(entries == 3);
    std::ifstream in(dir / "a" / "data.txt");
    std::string word;
    int n = 0;
    in >> word >> n;
    CHECK(word == "hello" && n == 42);
    fs::rename(dir / "a" / "data.txt", dir / "moved.txt");
    CHECK(fs::exists(dir / "moved.txt"));
    CHECK(fs::remove_all(dir) == 4);

    std::ostringstream os;
    os << "cxx_basic: " << (failures ? "FAIL" : "PASS") << " (" << failures << " failures)";
    std::cout << os.str() << std::endl;
    return failures ? 1 : 0;
}
