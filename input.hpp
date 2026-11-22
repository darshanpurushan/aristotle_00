#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace shopping {
namespace fs = std::filesystem;
using View = std::string_view;
using i64 = int64_t;
using u32 = uint32_t;
using u64 = uint64_t;
constexpr i64 day_ms = 86'400'000;
constexpr i64 week_ms = 7 * day_ms;
constexpr i64 session_gap = 1'800'000;
constexpr i64 invalid_time = std::numeric_limits<i64>::min();
constexpr u32 absent = std::numeric_limits<u32>::max();

inline i64 utc_day(i64 ms) {
    return ms / day_ms - (ms % day_ms < 0);
}

inline i64 decimal(View field, i64 limit) {
    if (field.empty()) return -1;
    i64 value = 0;
    for (unsigned char character : field) {
        unsigned digit = unsigned(character) - '0';
        if (digit > 9 || value > limit / 10 ||
            (value == limit / 10 && digit > unsigned(limit % 10))) return -1;
        value = value * 10 + digit;
    }
    return value;
}

inline i64 timestamp(View s) {
    if (s.size() != 20 && s.size() != 25) {
        i64 value = decimal(s, std::numeric_limits<i64>::max());
        return value < 0 ? invalid_time : value;
    }
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
        i64 value = decimal(s, std::numeric_limits<i64>::max());
        return value < 0 ? invalid_time : value;
    }
    auto pair = [&](size_t i) {
        unsigned a = unsigned(s[i]) - '0';
        unsigned b = unsigned(s[i + 1]) - '0';
        return a < 10 && b < 10 ? int(a * 10 + b) : -1;
    };
    int year = pair(0) * 100 + pair(2);
    int month = pair(5), date = pair(8), hour = pair(11), minute = pair(14), second = pair(17);
    if (year < 1 || month < 1 || month > 12 || date < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return invalid_time;
    }
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    constexpr int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    constexpr int offsets[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    if (date > lengths[month - 1] + (month == 2 && leap)) return invalid_time;
    int offset = 0;
    if (s.size() == 20) {
        if (s[19] != 'Z') return invalid_time;
    } else {
        int h = pair(20), m = pair(23);
        if ((s[19] != '+' && s[19] != '-') || s[22] != ':' ||
            h < 0 || h > 23 || m < 0 || m > 59) return invalid_time;
        offset = (h * 60 + m) * (s[19] == '-' ? -1 : 1);
    }
    int y = year - 1;
    int days = y * 365 + y / 4 - y / 100 + y / 400 - 719162 +
               offsets[month - 1] + (month > 2 && leap) + date - 1;
    return ((i64(days) * 24 + hour) * 3600 + (minute - offset) * 60 + second) * 1000;
}

inline bool unquote(View& field) {
    if (!field.empty() && field.front() == '"') {
        if (field.size() < 2 || field.back() != '"') return false;
        field.remove_prefix(1);
        field.remove_suffix(1);
    }
    return field.find('"') == View::npos;
}

template <size_t N>
bool scalar_fields(View line, std::array<View, N>& row) {
    for (size_t i = 0; i < N - 1; ++i) {
        size_t comma = line.find(',');
        if (comma == View::npos) return false;
        row[i] = line.substr(0, comma);
        if (!unquote(row[i])) return false;
        line.remove_prefix(comma + 1);
    }
    row.back() = line;
    return line.find(',') == View::npos && unquote(row.back());
}

#ifdef __AVX2__
template <size_t N>
bool short_fields(View line, std::array<View, N>& row) {
    // For short lines a pair of bit masks captures all CSV punctuation.
    // The final load overlaps the preceding block and stays inside the line.
    using Mask = __uint128_t;
    Mask commas = 0, quotes = 0;
    auto scan = [&](const char* pointer, unsigned discard, unsigned position) {
        auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pointer));
        unsigned comma = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(',')));
        unsigned quote = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('"')));
        commas |= Mask(comma >> discard) << position;
        quotes |= Mask(quote >> discard) << position;
    };
    unsigned position = 0;
    for (; position + 32 <= line.size(); position += 32) scan(line.data() + position, 0, position);
    if (position < line.size()) {
        scan(line.data() + line.size() - 32, position + 32 - line.size(), position);
    }
    unsigned previous = 0;
    for (size_t column = 0; column < N; ++column) {
        unsigned next = line.size();
        if (column + 1 < N) {
            if (!commas) return false;
            u64 low = u64(commas);
            next = low ? __builtin_ctzll(low) : 64 + __builtin_ctzll(u64(commas >> 64));
            commas &= commas - 1;
        }
        unsigned start = previous;
        unsigned length = next - start;
        if (length && line[start] == '"') {
            if (length < 2 || line[next - 1] != '"') return false;
            quotes &= ~((Mask(1) << start) | (Mask(1) << (next - 1)));
            ++start;
            length -= 2;
        }
        row[column] = View(line.data() + start, length);
        previous = next + 1;
    }
    return !commas && !quotes;
}
#endif

template <size_t N>
bool fields(View line, std::array<View, N>& row) {
#ifdef __AVX2__
    if (line.size() >= 32 && line.size() <= 128) return short_fields(line, row);
#endif
    return scalar_fields(line, row);
}

struct Chunk {
    const char* start;
    const char* end;
};

inline const char* line_end(const char* start, const char* end) {
    auto newline = static_cast<const char*>(std::memchr(start, '\n', end - start));
    return newline ? newline : end;
}

template<class Function>
void for_lines(Chunk chunk, Function visit) {
    for (auto start = chunk.start; start < chunk.end;) {
        auto end = line_end(start, chunk.end);
        visit(View(start, end - start));
        start = end + (end != chunk.end);
    }
}

class Mapping {
public:
    explicit Mapping(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Cannot open " + path.string());
        struct stat status {};
        if (fstat(fd, &status) != 0) {
            close(fd);
            throw std::runtime_error("Cannot stat " + path.string());
        }
        size_ = status.st_size;
        if (size_) {
            void* address = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (address != MAP_FAILED) data_ = static_cast<const char*>(address);
        }
        close(fd);
        if (size_ && !data_) throw std::runtime_error("Cannot map " + path.string());
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept : data_(std::exchange(other.data_, nullptr)), size_(other.size_) {}
    ~Mapping() {
        if (data_) munmap(const_cast<char*>(data_), size_);
    }

    void add_chunks(std::vector<Chunk>& result) const {
        if (!data_) return;
        auto end = data_ + size_;
        auto start = line_end(data_, end);
        if (start < end) ++start;
        constexpr ptrdiff_t chunk_size = 2 * 1024 * 1024;
        while (start < end) {
            auto split = end;
            if (end - start > chunk_size) {
                split = line_end(start + chunk_size, end);
                if (split < end) ++split;
            }
            result.push_back({start, split});
            start = split;
        }
    }

private:
    const char* data_ = nullptr;
    size_t size_ = 0;
};

inline std::vector<Mapping> files(const fs::path& directory) {
    std::vector<fs::path> paths;
    if (fs::exists(directory)) {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                paths.push_back(entry.path());
            }
        }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<Mapping> result;
    for (const auto& path : paths) result.emplace_back(path);
    return result;
}

template<class Function>
void parallel(size_t count, unsigned threads, Function work) {
    std::atomic<size_t> next = 0;
    std::exception_ptr failure;
    std::mutex failure_mutex;
    std::vector<std::jthread> workers;
    for (unsigned worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (;;) {
                    auto task = next.fetch_add(1, std::memory_order_relaxed);
                    if (task >= count) return;
                    work(task, worker);
                }
            } catch (...) {
                std::lock_guard lock(failure_mutex);
                if (!failure) failure = std::current_exception();
                next.store(count, std::memory_order_relaxed);
            }
        });
    }
    workers.clear();
    if (failure) std::rethrow_exception(failure);
}

inline std::pair<i64, i64> read_window(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    std::string json((std::istreambuf_iterator<char>(input)), {});
    auto read_key = [&](View key) {
        size_t name = json.find(key);
        size_t colon = name == std::string::npos ? name : json.find(':', name + key.size());
        size_t open_quote = colon == std::string::npos ? colon : json.find('"', colon + 1);
        size_t close_quote = open_quote == std::string::npos ? open_quote : json.find('"', open_quote + 1);
        if (close_quote == std::string::npos) throw std::runtime_error("Missing manifest timestamp");
        return timestamp(View(json).substr(open_quote + 1, close_quote - open_quote - 1));
    };
    i64 start = read_key("\"window_start\"");
    i64 end = read_key("\"window_end\"");
    if (start == invalid_time || end == invalid_time || start >= end) {
        throw std::runtime_error("Invalid manifest window");
    }
    return {start, end};
}
}
