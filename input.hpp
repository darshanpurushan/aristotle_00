#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
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
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    // Every digit position must hold an ASCII digit (OR the "not a digit" flags together).
    unsigned bad = 0;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) bad |= (unsigned(p[i]) - '0') > 9;
    if (s.size() == 25) {
        for (int i : {20, 21, 23, 24}) bad |= (unsigned(p[i]) - '0') > 9;
    }
    if (bad) return invalid_time;
    int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    int month = (p[5] - '0') * 10 + p[6] - '0';
    int date = (p[8] - '0') * 10 + p[9] - '0';
    int hour = (p[11] - '0') * 10 + p[12] - '0';
    int minute = (p[14] - '0') * 10 + p[15] - '0';
    int second = (p[17] - '0') * 10 + p[18] - '0';
    if (year < 1 || month < 1 || month > 12 || date < 1 ||
        hour > 23 || minute > 59 || second > 59) return invalid_time;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    constexpr int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    constexpr int offsets[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    if (date > lengths[month - 1] + (month == 2 && leap)) return invalid_time;
    int offset = 0;
    if (s.size() == 20) {
        if (s[19] != 'Z') return invalid_time;
    } else {
        int h = (p[20] - '0') * 10 + p[21] - '0';
        int m = (p[23] - '0') * 10 + p[24] - '0';
        if ((s[19] != '+' && s[19] != '-') || s[22] != ':' || h > 23 || m > 59) return invalid_time;
        offset = (h * 60 + m) * (s[19] == '-' ? -1 : 1);
    }
    int y = year - 1;
    int days = y * 365 + y / 4 - y / 100 + y / 400 - 719162 +
               offsets[month - 1] + (month > 2 && leap) + date - 1;
    return ((i64(days) * 24 + hour) * 3600 + (minute - offset) * 60 + second) * 1000;
}

// Cache just the dates that can affect this execution window. The input
// contains arbitrary valid/invalid timestamp strings, so every cache miss
// falls back to the complete strict parser above.
class FastTimestamp {
public:
    FastTimestamp(i64 start, i64 end) {
        int first = int(utc_day(start - week_ms - 2 * day_ms));
        int last = int(utc_day(end + 9 * day_ms));
        for (int day = first; day <= last && day < first + 96; ++day) {
            std::time_t seconds = std::time_t(i64(day) * 86400);
            std::tm tm{};
            gmtime_r(&seconds, &tm);
            char digits[10];
            int year = tm.tm_year + 1900;
            int month = tm.tm_mon + 1;
            int mday = tm.tm_mday;
            digits[0] = '0' + year / 1000 % 10;
            digits[1] = '0' + year / 100 % 10;
            digits[2] = '0' + year / 10 % 10;
            digits[3] = '0' + year % 10;
            digits[4] = '-';
            digits[5] = '0' + month / 10;
            digits[6] = '0' + month % 10;
            digits[7] = '-';
            digits[8] = '0' + mday / 10;
            digits[9] = '0' + mday % 10;
            u64 prefix; u16 suffix;
            std::memcpy(&prefix, digits, 8);
            std::memcpy(&suffix, digits + 8, 2);
            auto slot = slot_for(prefix, suffix);
            while (days_[slot].prefix) slot = (slot + 1) & (table_size - 1);
            days_[slot] = {prefix, suffix, day};
        }
    }

    i64 parse(View s) const {
        if ((s.size() != 20 && s.size() != 25) || s[4] != '-' || s[7] != '-' ||
            s[10] != 'T' || s[13] != ':' || s[16] != ':') return timestamp(s);
        int day = date(s.data());
        if (day == missing) return timestamp(s);
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        unsigned bad = 0;
        for (int i : {11, 12, 14, 15, 17, 18}) bad |= (unsigned(p[i]) - '0') > 9;
        if (bad) return invalid_time;
        int h = (p[11] - '0') * 10 + p[12] - '0';
        int m = (p[14] - '0') * 10 + p[15] - '0';
        int sec = (p[17] - '0') * 10 + p[18] - '0';
        if (h > 23 || m > 59 || sec > 59) return invalid_time;
        if (s.size() == 20) {
            if (s[19] != 'Z') return invalid_time;
        } else {
            if ((s[19] != '+' && s[19] != '-') || s[22] != ':') return invalid_time;
            for (int i : {20,21,23,24}) if ((unsigned(p[i]) - '0') > 9) return invalid_time;
            int offset_hours = (p[20] - '0') * 10 + p[21] - '0';
            int offset_minutes = (p[23] - '0') * 10 + p[24] - '0';
            if (offset_hours > 23 || offset_minutes > 59) return invalid_time;
            int delta = offset_hours * 60 + offset_minutes;
            m += s[19] == '+' ? -delta : delta;
        }
        return (i64(day) * 86400 + h * 3600 + m * 60 + sec) * 1000;
    }

    i64 ingest(View s) const {
        if (s.size() != 20 || s[19] != 'Z') return timestamp(s);
        int day = date(s.data());
        if (day == missing) return timestamp(s);
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        int h = (p[11] - '0') * 10 + p[12] - '0';
        int m = (p[14] - '0') * 10 + p[15] - '0';
        int sec = (p[17] - '0') * 10 + p[18] - '0';
        return (i64(day) * 86400 + h * 3600 + m * 60 + sec) * 1000;
    }

private:
    using u16 = uint16_t;
    struct DateSlot {u64 prefix = 0; u16 suffix = 0; int day = 0;};
    static constexpr unsigned table_size = 128;
    static constexpr int missing = std::numeric_limits<int>::min();
    std::array<DateSlot, table_size> days_{};
    static unsigned slot_for(u64 prefix, u16 suffix) {
        u64 mixed = prefix ^ (u64(suffix) << 48);
        mixed ^= mixed >> 32;
        mixed *= 0x9E3779B97F4A7C15ULL;
        return unsigned(mixed >> 57);
    }
    int date(const char* p) const {
        u64 prefix; u16 suffix;
        std::memcpy(&prefix, p, 8);
        std::memcpy(&suffix, p + 8, 2);
        auto slot = slot_for(prefix, suffix);
        for (;;) {
            const auto& entry = days_[slot];
            if (!entry.prefix) return missing;
            if (entry.prefix == prefix && entry.suffix == suffix) return entry.day;
            slot = (slot + 1) & (table_size - 1);
        }
    }
};

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
    const unsigned active = std::min<size_t>(threads, count);
    workers.reserve(active);
    for (unsigned worker = 0; worker < active; ++worker) {
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
