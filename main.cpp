#include "input.hpp"
#include "output.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace shopping {
using u8 = unsigned char;
constexpr size_t shard_count = 128;

// Word-at-a-time hash (the IDs are opaque strings of 1 to ~40 bytes). Only needs to be
// consistent inside one process; both the low bits (tables) and high bits (shards) are mixed.
inline u64 fast_mix(u64 a, u64 b) {
    __uint128_t p = (__uint128_t)a * b;
    return u64(p) ^ u64(p >> 64);
}
inline size_t hash_bytes(const char* p, size_t n) {
    constexpr u64 K = 0x9e3779b97f4a7c15ULL;
    constexpr u64 L = 0xa0761d6478bd642fULL;
    auto load8 = [](const char* q) { u64 a; std::memcpy(&a, q, 8); return a; };
    auto load4 = [](const char* q) { u32 a; std::memcpy(&a, q, 4); return a; };
    u64 a, b;
    if (n <= 16) {
        if (n >= 8) {
            a = load8(p); b = load8(p + n - 8);
        } else if (n >= 4) {
            a = (u64(load4(p)) << 32) | load4(p + n - 4);
            b = n * K;
        } else if (n) {
            a = u64(u8(p[0])) | u64(u8(p[n >> 1])) << 8 | u64(u8(p[n - 1])) << 16;
            b = n * K;
        } else { a = 0; b = 0; }
        return fast_mix(a ^ K, b ^ L ^ n);
    }
    u64 h = n * K;
    for (size_t offset = 0; offset + 16 < n; offset += 16)
        h = fast_mix(h ^ load8(p + offset), L ^ load8(p + offset + 8));
    return fast_mix(h ^ load8(p + n - 16), K ^ load8(p + n - 8));
}
struct Hash {
    size_t operator()(View v) const noexcept { return hash_bytes(v.data(), v.size()); }
};

size_t shard_for(size_t hash) {
    return (hash >> 32) % shard_count;
}

template<class T>
using Shards = std::array<std::vector<T>, shard_count>;

template<class T>
using Dictionary = std::unordered_map<View, T, Hash>;

// Open-addressing index. Each probe slot holds (hash tag, entry position), so a probe only
// dereferences the entry (and then the key bytes) when the 32-bit tag already matches.
// Matches still compare the full ID, so collisions never merge distinct IDs.
template<class T>
class StringIndex {
public:
    struct Entry {
        View key;
        size_t hash;
        T value;
    };

    const T* find(View key, size_t hash) const {
        if (slots_.empty()) return nullptr;
        u64 slot = slot_for(key, hash);
        return slots_[slot] == empty ? nullptr : &entries_[u32(slots_[slot])].value;
    }

    std::pair<T*, bool> insert(View key, size_t hash, T value) {
        if ((entries_.size() + 1) * 2 > slots_.size()) grow();
        u64 slot = slot_for(key, hash);
        if (slots_[slot] != empty) return {&entries_[u32(slots_[slot])].value, false};
        slots_[slot] = (u64(hash >> 32) << 32) | u64(entries_.size());
        entries_.push_back({key, hash, value});
        return {&entries_.back().value, true};
    }

    std::vector<Entry>& entries() { return entries_; }

    // Software prefetch pipeline for batched lookups: slot, then entry, then key bytes.
    void prefetch_slot(size_t hash) const {
        if (!slots_.empty()) __builtin_prefetch(&slots_[hash & (slots_.size() - 1)]);
    }
    void prefetch_entry(size_t hash) const {
        if (slots_.empty()) return;
        u64 value = slots_[hash & (slots_.size() - 1)];
        if (value != empty) __builtin_prefetch(&entries_[u32(value)]);
    }
    void prefetch_key(size_t hash) const {
        if (slots_.empty()) return;
        u64 value = slots_[hash & (slots_.size() - 1)];
        if (value != empty) __builtin_prefetch(entries_[u32(value)].key.data());
    }

private:
    static constexpr u64 empty = ~u64(0);

    static bool same(View a, View b) {
        return a.size() == b.size() && (a.data() == b.data() || std::memcmp(a.data(), b.data(), a.size()) == 0);
    }

    u64 slot_for(View key, size_t hash) const {
        u64 mask = slots_.size() - 1;
        u64 slot = hash & mask;
        const u32 tag = u32(hash >> 32);
        for (;;) {
            u64 value = slots_[slot];
            if (value == empty) return slot;
            if (u32(value >> 32) == tag) {
                const auto& entry = entries_[u32(value)];
                if (entry.hash == hash && same(entry.key, key)) return slot;
            }
            slot = (slot + 1) & mask;
        }
    }

    void grow() {
        slots_.assign(slots_.empty() ? 16 : slots_.size() * 2, empty);
        u64 mask = slots_.size() - 1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            u64 slot = entries_[i].hash & mask;
            while (slots_[slot] != empty) slot = (slot + 1) & mask;
            slots_[slot] = (u64(entries_[i].hash >> 32) << 32) | u64(i);
        }
    }

    std::vector<u64> slots_;
    std::vector<Entry> entries_;
};

View setting(const char* name, View fallback) {
    const char* value = std::getenv(name);
    return value && *value ? View(value) : fallback;
}

class Progress {
public:
    void log(const char* stage) {
        if (!std::getenv("PROFILE")) return;
        auto now = std::chrono::steady_clock::now();
        std::cerr << stage << ": " << std::chrono::duration<double>(now - last_).count() << " s\n";
        last_ = now;
    }

private:
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

struct CanonicalUser {
    View id;
    size_t hash;
    bool linked;
};

class Identities {
public:
    explicit Identities(const std::vector<Mapping>& links, unsigned threads) {
        std::vector<Chunk> chunks;
        for (const auto& file : links) file.add_chunks(chunks);
        struct Edge { u32 a, b; };
        struct Endpoint { View id; size_t hash; u32* node; };
        std::vector<std::deque<Edge>> edges(threads);
        std::vector<Shards<Endpoint>> endpoints(threads);
        parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
            auto add = [&](View id, u32* node) {
                size_t hash = Hash{}(id);
                endpoints[worker][shard_for(hash)].push_back({id, hash, node});
            };
            for_lines(chunks[index], [&](View line) {
                std::array<View, 2> row;
                if (!fields(line, row) || row[0].empty() || row[1].empty()) return;
                Edge& edge = edges[worker].emplace_back();
                add(row[0], &edge.a);
                add(row[1], &edge.b);
            });
        });
        parallel(shard_count, threads, [&](size_t shard, unsigned) {
            auto& index = nodes_[shard];
            for (auto& source : endpoints) {
                for (const auto& entry : source[shard]) {
                    auto [person, inserted] = index.insert(entry.id, entry.hash, {u32(index.entries().size()), {}, 0});
                    *entry.node = person->node * shard_count + shard;
                }
                std::vector<Endpoint>().swap(source[shard]);
            }
        });

        // Translate shard-local IDs to a dense integer space for union-find.
        std::array<u32, shard_count> offsets{};
        for (size_t shard = 0; shard < shard_count; ++shard) {
            offsets[shard] = parent_.size();
            for (const auto& entry : nodes_[shard].entries()) {
                parent_.push_back(parent_.size());
                size_.push_back(1);
                minimum_.emplace_back(entry.key, entry.hash);
            }
        }
        auto global = [&](u32 node) { return offsets[node % shard_count] + node / shard_count; };
        for (const auto& source : edges) {
            for (const Edge& edge : source) {
                u32 a = root(global(edge.a)), b = root(global(edge.b));
                if (a == b) continue;
                if (size_[a] < size_[b]) std::swap(a, b);
                parent_[b] = a;
                size_[a] += size_[b];
                if (minimum_[b].first < minimum_[a].first) minimum_[a] = minimum_[b];
            }
        }
        for (u32 node = 0; node < parent_.size(); ++node) parent_[node] = root(node);
        parallel(shard_count, threads, [&](size_t shard, unsigned) {
            for (auto& entry : nodes_[shard].entries()) {
                auto [name, hash] = minimum_[parent_[offsets[shard] + entry.value.node]];
                entry.value.name = name;
                entry.value.hash = hash;
            }
        });
        std::vector<u32>().swap(parent_);
        std::vector<u32>().swap(size_);
        std::vector<std::pair<View, size_t>>().swap(minimum_);
    }

    CanonicalUser canonical(View user) const { return canonical(user, Hash{}(user)); }
    void prefetch_slot(size_t hash) const { nodes_[shard_for(hash)].prefetch_slot(hash); }
    void prefetch_entry(size_t hash) const { nodes_[shard_for(hash)].prefetch_entry(hash); }
    void prefetch_key(size_t hash) const { nodes_[shard_for(hash)].prefetch_key(hash); }

    CanonicalUser canonical(View user, size_t hash) const {
        auto person = nodes_[shard_for(hash)].find(user, hash);
        if (!person) return {user, hash, false};
        return {person->name, person->hash, true};
    }

private:
    struct Person {
        u32 node;
        View name;
        size_t hash;
    };

    u32 root(u32 node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    std::array<StringIndex<Person>, shard_count> nodes_;
    std::vector<u32> parent_, size_;
    std::vector<std::pair<View, size_t>> minimum_;
};

struct Dimensions {
    Mapping products_file, campaigns_file;
    Dictionary<u32> product_category, campaign_index;
    std::vector<View> categories{"unknown"};
    std::vector<Campaign> campaigns{{"organic", "organic"}};

    explicit Dimensions(const fs::path& data)
        : products_file(data / "products.csv"), campaigns_file(data / "campaigns.csv") {
        Dictionary<u32> category_index;
        std::vector<Chunk> chunks;
        products_file.add_chunks(chunks);
        for (Chunk chunk : chunks) {
            for_lines(chunk, [&](View line) {
                std::array<View, 2> row;
                if (!fields(line, row)) throw std::runtime_error("Invalid products.csv row");
                auto [entry, inserted] = category_index.try_emplace(row[1], categories.size());
                if (inserted) categories.push_back(row[1]);
                product_category.emplace(row[0], entry->second);
            });
        }
        chunks.clear();
        campaigns_file.add_chunks(chunks);
        for (Chunk chunk : chunks) {
            for_lines(chunk, [&](View line) {
                std::array<View, 2> row;
                if (!fields(line, row)) throw std::runtime_error("Invalid campaigns.csv row");
                campaign_index.emplace(row[0], campaigns.size());
                campaigns.push_back({row[0], row[1]});
            });
        }
    }
};

struct RawEvent {
    size_t hash;
    u64 location;
};
struct RoutedEvent {
    View user;
    size_t hash;
    i64 ts;
    u32 price, category;
};
struct RoutedClick {
    View user;
    View id;
    size_t hash;
    i64 ts;
    u32 campaign;
};
// Sort keys pack (user index, time offset) into one word: 24 bits of user, 40 bits of ms.
constexpr int key_shift = 40;
constexpr u64 key_mask = (u64(1) << key_shift) - 1;
constexpr u64 max_users = (u64(1) << 24) - 1;
struct Event {
    u64 key;
    u32 price, category;
    u32 user() const { return u32(key >> key_shift); }
};
struct Click {
    u64 key;
    View id;
    u32 campaign;
    u32 user() const { return u32(key >> key_shift); }
};

View event_id(View line) {
#ifdef __AVX2__
    if (line.size() >= 32) {
        const char* data = line.data();
        auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
        unsigned commas = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(',')));
        if (commas) {
            unsigned length = __builtin_ctz(commas);
            unsigned quotes = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('"')));
            quotes &= (1u << length) - 1;
            if (!quotes) return View(data, length);
            if (length >= 2 && quotes == (1u | (1u << (length - 1)))) {
                return View(data + 1, length - 2);
            }
            return {};
        }
    }
#endif
    View id = line.substr(0, line.find(','));
    return unquote(id) ? id : View{};
}

// Parse errors are excluded before considering ingest time. Validation failures in
// the winning copy are handled later and therefore suppress later valid copies.
View earlier_event(View left, View right) {
    std::array<View, 7> a, b;
    bool valid_left = fields(left, a);
    bool valid_right = fields(right, b);
    if (!valid_right) return left;
    if (!valid_left || b[3] < a[3]) return right;
    return left;
}

struct EventScan {
    // A chunk is split at the first newline at or after 2 MiB. Its last row
    // can be longer, but every row start still fits in 22 bits.
    static constexpr unsigned offset_bits = 22;
    static constexpr u64 offset_mask = (u64{1} << offset_bits) - 1;
    std::vector<Chunk> chunks;
    std::vector<u64> losers;
};

// Needs no identities, so it can overlap with loading the links.
EventScan index_events(const std::vector<Mapping>& files, unsigned threads, Progress& progress) {
    EventScan scan;
    auto& chunks = scan.chunks;
    for (const auto& file : files) file.add_chunks(chunks);
    constexpr unsigned location_bits = 42;
    constexpr u64 location_mask = (u64{1} << location_bits) - 1;
    if (chunks.size() >= (u64{1} << (location_bits - EventScan::offset_bits))) {
        throw std::runtime_error("Too many event chunks");
    }
    std::vector<Shards<RawEvent>> raw(threads);
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        Chunk chunk = chunks[index];
        for_lines(chunk, [&](View line) {
            View id = event_id(line);
            if (id.empty()) return;
            size_t hash = Hash{}(id);
            u64 location = (u64(index) << EventScan::offset_bits) | u64(line.data() - chunk.start);
            raw[worker][shard_for(hash)].push_back({hash, location});
        });
    });
    progress.log("index events");

    auto line_at = [&](u64 location) {
        Chunk chunk = chunks[location >> EventScan::offset_bits];
        const char* start = chunk.start + (location & EventScan::offset_mask);
        return View(start, line_end(start, chunk.end) - start);
    };
    std::vector<std::vector<u64>> discarded(threads);
    parallel(shard_count, threads, [&](size_t shard, unsigned worker) {
        size_t row_count = 0;
        for (const auto& source : raw) row_count += source[shard].size();
        if (!row_count) return;
        size_t capacity = 2;
        while (capacity < row_count + row_count / 2) capacity *= 2;
        // A fingerprint filters probes. Matching fingerprints still compare
        // full IDs, so the packed table never treats a collision as a duplicate.
        std::vector<u64> index(capacity);
        for (auto& source : raw) {
            for (const RawEvent& candidate : source[shard]) {
                size_t slot = candidate.hash & (capacity - 1);
                u64 tag = candidate.hash & ~location_mask;
                for (;;) {
                    auto& previous = index[slot];
                    if (!previous) {
                        previous = tag | (candidate.location + 1);
                        break;
                    }
                    if ((previous & ~location_mask) == tag) {
                        u64 previous_location = (previous & location_mask) - 1;
                        View left = line_at(previous_location);
                        View right = line_at(candidate.location);
                        if (event_id(left) == event_id(right)) {
                            View winner = earlier_event(left, right);
                            if (winner.data() == left.data()) {
                                discarded[worker].push_back(candidate.location);
                            } else {
                                discarded[worker].push_back(previous_location);
                                previous = tag | (candidate.location + 1);
                            }
                            break;
                        }
                    }
                    slot = (slot + 1) & (capacity - 1);
                }
            }
        }
        for (auto& source : raw) std::vector<RawEvent>().swap(source[shard]);
    });
    for (auto& worker : discarded) {
        scan.losers.insert(scan.losers.end(), worker.begin(), worker.end());
        std::vector<u64>().swap(worker);
    }
    std::sort(scan.losers.begin(), scan.losers.end());
    progress.log("deduplicate events");
    return scan;
}

// The event files stay mapped until the end of the run, so routed records point straight
// into them and no per-event copy or interning of user names is needed.
void route_events(const EventScan& scan, unsigned threads, i64 start, i64 end,
                  const Identities& people, const Dimensions& dimensions, const FastTimestamp& parser,
                  std::vector<Shards<RoutedEvent>>& routed) {
    const auto& chunks = scan.chunks;
    const auto& losers = scan.losers;
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        Chunk chunk = chunks[index];
        u64 base = u64(index) << EventScan::offset_bits;
        auto skip = std::lower_bound(losers.begin(), losers.end(), base);
        struct Pending {
            View user;
            size_t hash;
            i64 ts;
            u32 price, category;
        };
        constexpr size_t batch = 16;
        Pending pending[batch];
        size_t held = 0;
        auto flush = [&] {
            for (size_t i = 0; i < held; ++i) people.prefetch_entry(pending[i].hash);
            for (size_t i = 0; i < held; ++i) people.prefetch_key(pending[i].hash);
            for (size_t i = 0; i < held; ++i) {
                const Pending& p = pending[i];
                auto person = people.canonical(p.user, p.hash);
                routed[worker][shard_for(person.hash)].push_back({person.id, person.hash, p.ts, p.price, p.category});
            }
            held = 0;
        };
        for_lines(chunk, [&](View line) {
            u64 location = base | u64(line.data() - chunk.start);
            if (skip != losers.end() && *skip == location) {
                ++skip;
                return;
            }
            std::array<View, 7> row;
            if (!fields(line, row) || row[0].empty() || row[1].empty() || row[5].empty()) return;
            i64 ts = parser.parse(row[2]);
            if (ts < start || ts >= end || parser.ingest(row[3]) - ts > day_ms) return;
            u32 price = 0, category = 0;
            if (row[4] == "purchase") {
                i64 value = decimal(row[6], 10'000'000);
                if (value < 1) return;
                price = value;
                auto product = dimensions.product_category.find(row[5]);
                if (product != dimensions.product_category.end()) category = product->second;
            } else if (row[4] != "view" && row[4] != "cart") {
                return;
            }
            size_t hash = Hash{}(row[1]);
            people.prefetch_slot(hash);
            pending[held++] = {row[1], hash, ts, price, category};
            if (held == batch) flush();
        });
        flush();
    });
}

void route_clicks(const std::vector<Mapping>& files, unsigned threads, i64 start, i64 end,
                  const Identities& people, const Dimensions& dimensions, const FastTimestamp& parser,
                  std::vector<Shards<RoutedClick>>& routed) {
    std::vector<Chunk> chunks;
    for (const auto& file : files) file.add_chunks(chunks);
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        struct Pending {
            View user, id;
            size_t hash;
            i64 ts;
            u32 campaign;
        };
        constexpr size_t batch = 16;
        Pending pending[batch];
        size_t held = 0;
        auto flush = [&] {
            for (size_t i = 0; i < held; ++i) people.prefetch_entry(pending[i].hash);
            for (size_t i = 0; i < held; ++i) people.prefetch_key(pending[i].hash);
            for (size_t i = 0; i < held; ++i) {
                const Pending& p = pending[i];
                auto person = people.canonical(p.user, p.hash);
                // Exact duplicate clicks contribute identically to both the winner and the union.
                routed[worker][shard_for(person.hash)].push_back({person.id, p.id, person.hash, p.ts, p.campaign});
            }
            held = 0;
        };
        for_lines(chunks[index], [&](View line) {
            std::array<View, 4> row;
            if (!fields(line, row) || row[0].empty() || row[1].empty()) return;
            i64 ts = parser.parse(row[3]);
            if (ts < start - week_ms || ts >= end) return;
            auto campaign = dimensions.campaign_index.find(row[2]);
            if (campaign == dimensions.campaign_index.end()) return;
            size_t hash = Hash{}(row[1]);
            people.prefetch_slot(hash);
            pending[held++] = {row[1], row[0], hash, ts, campaign->second};
            if (held == batch) flush();
        });
        flush();
    });
}

// Small bump allocator for the person names of one shard, so the shard's lookup table and
// output rows read hot, compact memory instead of the (cold) mapped input files.
class NameArena {
public:
    View copy(View text) {
        if (text.size() > block_left_) {
            size_t size = std::max<size_t>(text.size(), 64 * 1024);
            blocks_.push_back(std::make_unique<char[]>(size));
            block_ = blocks_.back().get();
            block_left_ = size;
        }
        char* destination = block_;
        std::memcpy(destination, text.data(), text.size());
        block_ += text.size();
        block_left_ -= text.size();
        return View(destination, text.size());
    }

private:
    std::vector<std::unique_ptr<char[]>> blocks_;
    char* block_ = nullptr;
    size_t block_left_ = 0;
};

struct Session {
    i64 start;
    i64 revenue;
    int day;
};

class Aggregation {
public:
    Aggregation(const Dimensions& dimensions, int first_day, int days, i64 start, fs::path output)
        : dimensions_(dimensions), first_day_(first_day), start_(start), width_(dimensions.campaigns.size()),
          campaign_totals_(size_t(days) * width_), category_revenue_(dimensions.categories.size()),
          covered_(width_), user_rows_(std::move(output)) {}

    void person(View name, const Event* event, const Event* end, const Click* click, const Click* click_end) {
        sessions_.clear();
        size_t day_begin = 0;
        i64 day_events = 0, day_revenue = 0;
        auto emit_day = [&] {
            const View* top = nullptr;
            i64 highest = 0;
            for (u32 category : used_categories_) {
                i64 amount = category_revenue_[category];
                const View& label = dimensions_.categories[category];
                if (amount > highest || (amount == highest && top && label < *top)) {
                    top = &label;
                    highest = amount;
                }
                category_revenue_[category] = 0;
            }
            used_categories_.clear();
            user_rows_.append(name, sessions_[day_begin].day + first_day_, sessions_.size() - day_begin,
                              day_events, day_revenue, top);
            day_begin = sessions_.size();
            day_events = day_revenue = 0;
        };

        i64 previous = start_ + i64(event->key & key_mask);
        for (; event != end; ++event) {
            i64 ts = start_ + i64(event->key & key_mask);
            if (sessions_.empty() || ts - previous > session_gap) {
                int day = utc_day(ts) - first_day_;
                if (!sessions_.empty() && day != sessions_.back().day) emit_day();
                sessions_.push_back({ts, 0, day});
            }
            previous = ts;
            ++day_events;
            if (event->price) {
                sessions_.back().revenue += event->price;
                day_revenue += event->price;
                i64& total = category_revenue_[event->category];
                if (!total) used_categories_.push_back(event->category);
                total += event->price;
            }
        }
        emit_day();

        for (const Session& session : sessions_) {
            while (click != click_end && click_ts(*click) <= session.start) {
                consider_assist(*click++);
            }
            u32 campaign = 0;
            if (winner_ && session.start - click_ts(*winner_) <= week_ms) campaign = winner_->campaign;
            auto& summary = total(session.day, campaign);
            ++summary.sessions;
            summary.purchasing += session.revenue > 0;
            summary.revenue += session.revenue;
        }
        for (u32 campaign : assisted_campaigns_) covered_[campaign] = 0;
        assisted_campaigns_.clear();
        winner_ = nullptr;
        first_ = last_ = 0;
    }

    void close(bool require_file) { user_rows_.close(require_file); }

    std::vector<CampaignTotal>& totals() { return campaign_totals_; }

private:
    i64 click_ts(const Click& click) const { return start_ - week_ms + i64(click.key & key_mask); }

    CampaignTotal& total(int day, u32 campaign) {
        return campaign_totals_[size_t(day) * width_ + campaign];
    }

    void consider_assist(const Click& click) {
        const i64 ts = click_ts(click);
        if (!winner_ || ts > click_ts(*winner_) ||
            (ts == click_ts(*winner_) && click.id < winner_->id)) winner_ = &click;

        // Clicks arrive in time order, so their covered session intervals move forward.
        while (first_ < sessions_.size() && sessions_[first_].start < ts) ++first_;
        last_ = std::max(last_, first_);
        while (last_ < sessions_.size() && sessions_[last_].start - ts <= week_ms) ++last_;
        size_t& covered = covered_[click.campaign];
        size_t begin = std::max(first_, covered);
        if (begin >= last_) return;
        if (!covered) assisted_campaigns_.push_back(click.campaign);
        covered = last_;
        for (size_t index = begin; index < last_; ++index) {
            ++total(sessions_[index].day, click.campaign).assists;
        }
    }

    const Dimensions& dimensions_;
    int first_day_;
    i64 start_;
    size_t width_;
    std::vector<CampaignTotal> campaign_totals_;
    std::vector<i64> category_revenue_;
    std::vector<u32> used_categories_, assisted_campaigns_;
    std::vector<size_t> covered_;
    std::vector<Session> sessions_;
    const Click* winner_ = nullptr;
    size_t first_ = 0, last_ = 0;
    UserWriter user_rows_;
};

// Stable counting sort on the user index (the high key bits), then a comparison sort of the
// few rows each user has. Much cheaper than sorting the whole shard by comparison.
// Time offsets fit within 31 bits for the generator's 14-day window.
// Fallback to comparison sorting if a fixture uses a longer window.
template<class Row>
void sort_long_group(Row* first, Row* last) {
    const size_t n = last - first;
    constexpr size_t radix = 65536;
    std::array<u32, radix> counts{};
    std::vector<Row> scratch(n);
    for (size_t i = 0; i < n; ++i) ++counts[u32(first[i].key) & 65535];
    u32 running = 0;
    for (u32& count : counts) {u32 old = count; count = running; running += old;}
    for (size_t i = 0; i < n; ++i) scratch[counts[u32(first[i].key) & 65535]++] = first[i];
    counts.fill(0);
    for (size_t i = 0; i < n; ++i) ++counts[u32(scratch[i].key >> 16) & 65535];
    running = 0;
    for (u32& count : counts) {u32 old = count; count = running; running += old;}
    for (size_t i = 0; i < n; ++i) first[counts[u32(scratch[i].key >> 16) & 65535]++] = scratch[i];
}

template<class Row>
void group_by_user(std::vector<Row>& rows, size_t users) {
    if (rows.size() < 2) return;
    std::vector<u32> start(users + 1, 0);
    for (const Row& row : rows) ++start[row.user() + 1];
    for (size_t user = 0; user < users; ++user) start[user + 1] += start[user];
    std::vector<Row> sorted(rows.size());
    // Scatter backwards into each user's range using the exclusive prefix
    // boundaries as cursors. This eliminates a second cursor array.
    for (const Row& row : rows) sorted[--start[row.user() + 1]] = row;
    for (size_t user = 0; user < users; ++user) {
        const size_t begin = start[user + 1];
        const size_t finish = (user + 1 == users) ? sorted.size() : start[user + 2];
        if (finish - begin > 1) {
            if (finish - begin >= 8192) {
                bool small_times = true;
                for (size_t k = begin; k < finish; ++k) {
                    if ((sorted[k].key & key_mask) >= (u64(1) << 32)) { small_times = false; break; }
                }
                if (small_times) {
                    sort_long_group(sorted.data() + begin, sorted.data() + finish);
                    continue;
                }
            }
            std::sort(sorted.begin() + begin, sorted.begin() + finish,
                      [](const Row& a, const Row& b) { return a.key < b.key; });
        }

    }
    rows.swap(sorted);
}

void aggregate_shard(size_t shard, i64 start, std::vector<Shards<RoutedEvent>>& event_shards,
                     std::vector<Shards<RoutedClick>>& click_shards, Aggregation& output) {
    StringIndex<u32> users;
    std::vector<View> names;
    std::vector<Event> events;
    size_t count = 0;
    for (const auto& source : event_shards) count += source[shard].size();
    events.reserve(count);
    NameArena arena;
    constexpr size_t ahead = 12;
    for (auto& source : event_shards) {
        const auto& rows = source[shard];
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i + ahead < rows.size()) __builtin_prefetch(rows[i + ahead].user.data());
            const RoutedEvent& row = rows[i];
            auto [entry, inserted] = users.insert(row.user, row.hash, names.size());
            if (inserted) {
                if (names.size() >= max_users - 1) throw std::runtime_error("Too many people in one shard");
                View stored = arena.copy(row.user);
                users.entries().back().key = stored;
                names.push_back(stored);
            }
            u64 offset = u64(row.ts - start);
            events.push_back({(u64(*entry) << key_shift) | offset, row.price, row.category});
        }
        std::vector<RoutedEvent>().swap(source[shard]);
    }

    const i64 click_base = start - week_ms;
    std::vector<Click> clicks;
    size_t click_count = 0;
    for (const auto& source : click_shards) click_count += source[shard].size();
    clicks.reserve(click_count + 1);
    for (auto& source : click_shards) {
        const auto& rows = source[shard];
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i + ahead < rows.size()) __builtin_prefetch(rows[i + ahead].user.data());
            const RoutedClick& row = rows[i];
            auto user = users.find(row.user, row.hash);
            if (user) clicks.push_back({(u64(*user) << key_shift) | u64(row.ts - click_base), row.id, row.campaign});
        }
        std::vector<RoutedClick>().swap(source[shard]);
    }

    group_by_user(events, names.size());
    group_by_user(clicks, names.size());
    // Sentinel: user index max_users sorts after every real user.
    clicks.push_back({~u64(0), {}, 0});

    auto click = clicks.data();
    const auto* event_end = events.empty() ? events.data() : events.data() + events.size();
    for (auto event = events.data(); event != event_end;) {
        u32 user = event->user();
        auto next_event = event + 1;
        while (next_event != event_end && next_event->user() == user) ++next_event;
        while (click->user() < user) ++click;
        auto next_click = click;
        while (next_click->user() == user) ++next_click;
        output.person(names[user], event, next_event, click, next_click);
        event = next_event;
        click = next_click;
    }
}

void prepare_table(const fs::path& path) {
    fs::create_directories(path);
    for (const auto& entry : fs::directory_iterator(path)) {
        auto name = entry.path().filename().string();
        if (name.starts_with("shopping-") && entry.path().extension() == ".parquet" &&
            entry.is_regular_file()) fs::remove(entry.path());
    }
}

int run() {
    Progress progress;
    fs::path data = setting("DATA_DIR", "/data");
    fs::path output = setting("OUTPUT_DIR", "/output");
    unsigned threads = std::clamp<i64>(decimal(setting("NCPU", "8"), 4096), 1, 128);
    auto [start, end] = read_window(data / "manifest.json");
    FastTimestamp parser(start, end);
    int first_day = utc_day(start);
    int days = utc_day(end - 1) - first_day + 1;

    prepare_table(output / "user_daily");
    prepare_table(output / "campaign_daily");
    auto event_files = files(data / "events");
    auto click_files = files(data / "clicks");
    auto link_files = files(data / "links");
    Dimensions dimensions(data);

    // Link indexing and event deduplication are independent.
    std::unique_ptr<Identities> people_ptr;
    std::exception_ptr link_failure;
    EventScan scan;
    {
        const unsigned link_threads = std::max(1u, threads / 3);
        const unsigned event_threads = std::max(1u, threads - link_threads);
        std::jthread link_loader([&] {
            try { people_ptr = std::make_unique<Identities>(link_files, link_threads); }
            catch (...) { link_failure = std::current_exception(); }
        });
        scan = index_events(event_files, event_threads, progress);
    }
    if (link_failure) std::rethrow_exception(link_failure);
    const Identities& people = *people_ptr;
    progress.log("load links");

    std::vector<Shards<RoutedEvent>> event_shards(threads);
    std::vector<Shards<RoutedClick>> click_shards(threads);
    route_events(scan, threads, start, end, people, dimensions, parser, event_shards);
    progress.log("route events");
    route_clicks(click_files, threads, start, end, people, dimensions, parser, click_shards);
    progress.log("route clicks");

    // Largest shards first, so whale shards do not become the tail of the run.
    std::vector<size_t> order(shard_count);
    std::vector<size_t> weight(shard_count);
    for (size_t shard = 0; shard < shard_count; ++shard) {
        order[shard] = shard;
        for (unsigned w = 0; w < threads; ++w)
            weight[shard] += event_shards[w][shard].size() + click_shards[w][shard].size();
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weight[a] > weight[b]; });

    std::vector<std::unique_ptr<Aggregation>> workers;
    for (unsigned i = 0; i < threads; ++i) {
        workers.push_back(std::make_unique<Aggregation>(
            dimensions, first_day, days, start, output / "user_daily" / ("shopping-" + std::to_string(i) + ".parquet")));
    }
    parallel(shard_count, threads, [&](size_t shard, unsigned worker) {
        aggregate_shard(order[shard], start, event_shards, click_shards, *workers[worker]);
    });
    std::vector<CampaignTotal> totals(size_t(days) * dimensions.campaigns.size());
    // Flush the last batch and write each file's footer on its own thread.
    parallel(threads, threads, [&](size_t worker, unsigned) { workers[worker]->close(worker == 0); });
    for (unsigned worker = 0; worker < threads; ++worker) {
        const auto& partial = workers[worker]->totals();
        for (size_t row = 0; row < totals.size(); ++row) totals[row] += partial[row];
    }
    write_campaigns(output / "campaign_daily" / "shopping-0.parquet", dimensions.campaigns, totals, first_day);
    progress.log("sort, aggregate and write parquet");
    return 0;
}
}

int main() {
    try {
        return shopping::run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
