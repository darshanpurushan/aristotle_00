#include "input.hpp"
#include "output.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <tuple>
#include <unordered_map>

namespace shopping {
constexpr size_t shard_count = 128;
using Hash = std::hash<View>;

size_t shard_for(size_t hash) {
    return (hash >> 32) % shard_count;
}

template<class T>
using Shards = std::array<std::vector<T>, shard_count>;

template<class T>
using Dictionary = std::unordered_map<View, T, Hash>;

// Separate the compact probe array from the keys. Hash matches still compare
// the full ID, so collisions never merge distinct IDs.
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
        u32 position = slots_[slot_for(key, hash)];
        return position == absent ? nullptr : &entries_[position].value;
    }

    std::pair<T*, bool> insert(View key, size_t hash, T value) {
        if ((entries_.size() + 1) * 3 > slots_.size() * 2) grow();
        size_t slot = slot_for(key, hash);
        u32 position = slots_[slot];
        if (position != absent) return {&entries_[position].value, false};
        slots_[slot] = entries_.size();
        entries_.push_back({key, hash, value});
        return {&entries_.back().value, true};
    }

    std::vector<Entry>& entries() { return entries_; }

private:
    size_t slot_for(View key, size_t hash) const {
        size_t mask = slots_.size() - 1;
        size_t slot = hash & mask;
        for (;;) {
            u32 position = slots_[slot];
            if (position == absent) return slot;
            const auto& entry = entries_[position];
            if (entry.hash == hash && entry.key == key) return slot;
            slot = (slot + 1) & mask;
        }
    }

    void grow() {
        slots_.assign(slots_.empty() ? 16 : slots_.size() * 2, absent);
        size_t mask = slots_.size() - 1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            size_t slot = entries_[i].hash & mask;
            while (slots_[slot] != absent) slot = (slot + 1) & mask;
            slots_[slot] = i;
        }
    }

    std::vector<u32> slots_;
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

class StandaloneUsers {
public:
    View retain(View id, size_t hash) {
        auto [value, inserted] = index_.insert(id, hash, {});
        if (inserted) {
            auto* bytes = static_cast<char*>(storage_.allocate(id.size(), alignof(char)));
            std::memcpy(bytes, id.data(), id.size());
            *value = View(bytes, id.size());
            index_.entries().back().key = *value;
        }
        return *value;
    }

private:
    std::pmr::monotonic_buffer_resource storage_;
    StringIndex<View> index_;
};

class Identities {
public:
    explicit Identities(const std::vector<Mapping>& links) {
        std::vector<Chunk> chunks;
        for (const auto& file : links) file.add_chunks(chunks);
        for (Chunk chunk : chunks) {
            for_lines(chunk, [&](View line) {
                std::array<View, 2> row;
                if (!fields(line, row) || row[0].empty() || row[1].empty()) return;
                u32 a = root(intern(row[0])), b = root(intern(row[1]));
                if (a == b) return;
                if (size_[a] < size_[b]) std::swap(a, b);
                parent_[b] = a;
                size_[a] += size_[b];
                if (minimum_[b].first < minimum_[a].first) minimum_[a] = minimum_[b];
            });
        }
        for (auto& entry : nodes_.entries()) entry.value = root(entry.value);
    }

    CanonicalUser canonical(View user) const {
        size_t hash = Hash{}(user);
        auto node = nodes_.find(user, hash);
        if (!node) return {user, hash, false};
        auto [name, name_hash] = minimum_[*node];
        return {name, name_hash, true};
    }

private:
    u32 intern(View id) {
        size_t hash = Hash{}(id);
        auto [node, inserted] = nodes_.insert(id, hash, parent_.size());
        if (inserted) {
            parent_.push_back(*node);
            size_.push_back(1);
            minimum_.emplace_back(id, hash);
        }
        return *node;
    }

    u32 root(u32 node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    StringIndex<u32> nodes_;
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
    i64 ts;
    u32 price, category;
};
struct RoutedClick {
    View user;
    View id;
    i64 ts;
    u32 campaign;
};
struct Event {
    i64 ts;
    u32 user, price, category;
};
struct Click {
    i64 ts;
    View id;
    u32 user, campaign;
};

View event_id(View line) {
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

void route_events(const std::vector<Mapping>& files, unsigned threads, i64 start, i64 end,
                  const Identities& people, const Dimensions& dimensions,
                  std::vector<Shards<RoutedEvent>>& routed,
                  std::vector<StandaloneUsers>& standalone, Progress& progress) {
    std::vector<Chunk> chunks;
    for (const auto& file : files) file.add_chunks(chunks);
    std::vector<Shards<RawEvent>> raw(threads);
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        Chunk chunk = chunks[index];
        for_lines(chunk, [&](View line) {
            View id = event_id(line);
            if (id.empty()) return;
            size_t hash = Hash{}(id);
            u64 location = (u64(index) << 32) | u32(line.data() - chunk.start);
            raw[worker][shard_for(hash)].push_back({hash, location});
        });
    });
    progress.log("index events");

    auto line_at = [&](u64 location) {
        Chunk chunk = chunks[location >> 32];
        const char* start = chunk.start + u32(location);
        return View(start, line_end(start, chunk.end) - start);
    };
    std::vector<std::vector<u64>> discarded(threads);
    parallel(shard_count, threads, [&](size_t shard, unsigned worker) {
        size_t row_count = 0;
        for (const auto& source : raw) row_count += source[shard].size();
        if (!row_count) return;
        size_t capacity = 2;
        while (capacity < row_count + row_count / 2) capacity *= 2;
        std::vector<const RawEvent*> index(capacity);
        for (auto& source : raw) {
            for (const RawEvent& candidate : source[shard]) {
                size_t slot = candidate.hash & (capacity - 1);
                for (;;) {
                    const RawEvent* previous = index[slot];
                    if (!previous) {
                        index[slot] = &candidate;
                        break;
                    }
                    if (previous->hash == candidate.hash) {
                        View left = line_at(previous->location);
                        View right = line_at(candidate.location);
                        if (event_id(left) == event_id(right)) {
                            View winner = earlier_event(left, right);
                            if (winner.data() == left.data()) {
                                discarded[worker].push_back(candidate.location);
                            } else {
                                discarded[worker].push_back(previous->location);
                                index[slot] = &candidate;
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
    std::vector<u64> losers;
    for (auto& worker : discarded) {
        losers.insert(losers.end(), worker.begin(), worker.end());
        std::vector<u64>().swap(worker);
    }
    std::sort(losers.begin(), losers.end());
    progress.log("deduplicate events");

    // Revisit the input in file order. Only actual hash matches require
    // random reads during deduplication, and routing reads whole pages.
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        Chunk chunk = chunks[index];
        u64 base = u64(index) << 32;
        auto skip = std::lower_bound(losers.begin(), losers.end(), base);
        for_lines(chunk, [&](View line) {
            u64 location = base | u32(line.data() - chunk.start);
            if (skip != losers.end() && *skip == location) {
                ++skip;
                return;
            }
            std::array<View, 7> row;
            if (!fields(line, row) || row[0].empty() || row[1].empty() || row[5].empty()) return;
            i64 ts = timestamp(row[2]);
            if (ts < start || ts >= end || timestamp(row[3]) - ts > day_ms) return;
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
            auto [user, hash, linked] = people.canonical(row[1]);
            if (!linked) user = standalone[worker].retain(user, hash);
            routed[worker][shard_for(hash)].push_back({user, ts, price, category});
        });
    });
}

void route_clicks(const std::vector<Mapping>& files, unsigned threads, i64 start, i64 end,
                  const Identities& people, const Dimensions& dimensions,
                  std::vector<Shards<RoutedClick>>& routed) {
    std::vector<Chunk> chunks;
    for (const auto& file : files) file.add_chunks(chunks);
    parallel(chunks.size(), threads, [&](size_t index, unsigned worker) {
        for_lines(chunks[index], [&](View line) {
            std::array<View, 4> row;
            if (!fields(line, row) || row[0].empty() || row[1].empty()) return;
            i64 ts = timestamp(row[3]);
            if (ts < start - week_ms || ts >= end) return;
            auto campaign = dimensions.campaign_index.find(row[2]);
            if (campaign == dimensions.campaign_index.end()) return;
            auto user = people.canonical(row[1]);
            // Exact duplicate clicks contribute identically to both the winner and the union.
            routed[worker][shard_for(user.hash)].push_back({user.id, row[0], ts, campaign->second});
        });
    });
}

struct Session {
    i64 start;
    i64 revenue;
    int day;
};

class Aggregation {
public:
    Aggregation(const Dimensions& dimensions, int first_day, int days, fs::path output)
        : dimensions_(dimensions), first_day_(first_day), width_(dimensions.campaigns.size()),
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

        i64 previous = event->ts;
        for (; event != end; ++event) {
            if (sessions_.empty() || event->ts - previous > session_gap) {
                int day = utc_day(event->ts) - first_day_;
                if (!sessions_.empty() && day != sessions_.back().day) emit_day();
                sessions_.push_back({event->ts, 0, day});
            }
            previous = event->ts;
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
            while (click != click_end && click->ts <= session.start) {
                consider_assist(*click++);
            }
            u32 campaign = 0;
            if (winner_ && session.start - winner_->ts <= week_ms) campaign = winner_->campaign;
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
    CampaignTotal& total(int day, u32 campaign) {
        return campaign_totals_[size_t(day) * width_ + campaign];
    }

    void consider_assist(const Click& click) {
        if (!winner_ || click.ts > winner_->ts ||
            (click.ts == winner_->ts && click.id < winner_->id)) winner_ = &click;

        // Clicks arrive in time order, so their covered session intervals move forward.
        while (first_ < sessions_.size() && sessions_[first_].start < click.ts) ++first_;
        last_ = std::max(last_, first_);
        while (last_ < sessions_.size() && sessions_[last_].start - click.ts <= week_ms) ++last_;
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

void aggregate_shard(size_t shard, std::vector<Shards<RoutedEvent>>& event_shards,
                     std::vector<Shards<RoutedClick>>& click_shards, Aggregation& output) {
    StringIndex<u32> users;
    std::vector<View> names;
    std::vector<Event> events;
    size_t count = 0;
    for (const auto& source : event_shards) count += source[shard].size();
    events.reserve(count);
    for (auto& source : event_shards) {
        for (const RoutedEvent& row : source[shard]) {
            auto [entry, inserted] = users.insert(row.user, Hash{}(row.user), names.size());
            if (inserted) names.push_back(row.user);
            events.push_back({row.ts, *entry, row.price, row.category});
        }
        std::vector<RoutedEvent>().swap(source[shard]);
    }

    std::vector<Click> clicks;
    for (auto& source : click_shards) {
        for (const RoutedClick& row : source[shard]) {
            auto user = users.find(row.user, Hash{}(row.user));
            if (user) clicks.push_back({row.ts, row.id, *user, row.campaign});
        }
        std::vector<RoutedClick>().swap(source[shard]);
    }

    auto order = [](const auto& a, const auto& b) {
        return std::tie(a.user, a.ts) < std::tie(b.user, b.ts);
    };
    std::sort(events.begin(), events.end(), order);
    std::sort(clicks.begin(), clicks.end(), order);
    // A nonempty backing array makes equal empty-range pointers well-defined.
    clicks.push_back({0, {}, absent, 0});

    auto click = clicks.data();
    const auto* event_end = events.empty() ? events.data() : events.data() + events.size();
    for (auto event = events.data(); event != event_end;) {
        u32 user = event->user;
        auto next_event = event + 1;
        while (next_event != event_end && next_event->user == user) ++next_event;
        while (click->user < user) ++click;
        auto next_click = click;
        while (next_click->user == user) ++next_click;
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
    int first_day = utc_day(start);
    int days = utc_day(end - 1) - first_day + 1;

    prepare_table(output / "user_daily");
    prepare_table(output / "campaign_daily");
    auto event_files = files(data / "events");
    auto click_files = files(data / "clicks");
    auto link_files = files(data / "links");
    Dimensions dimensions(data);
    Identities people(link_files);
    progress.log("load links and dimensions");

    std::vector<Shards<RoutedEvent>> event_shards(threads);
    std::vector<Shards<RoutedClick>> click_shards(threads);
    std::vector<StandaloneUsers> standalone(threads);
    route_events(event_files, threads, start, end, people, dimensions, event_shards, standalone, progress);
    // The routed records now reference stable names independent of the event files.
    event_files.clear();
    progress.log("route events");
    route_clicks(click_files, threads, start, end, people, dimensions, click_shards);
    progress.log("route clicks");

    std::vector<std::unique_ptr<Aggregation>> workers;
    for (unsigned i = 0; i < threads; ++i) {
        workers.push_back(std::make_unique<Aggregation>(
            dimensions, first_day, days, output / "user_daily" / ("shopping-" + std::to_string(i) + ".parquet")));
    }
    parallel(shard_count, threads, [&](size_t shard, unsigned worker) {
        aggregate_shard(shard, event_shards, click_shards, *workers[worker]);
    });
    std::vector<CampaignTotal> totals(size_t(days) * dimensions.campaigns.size());
    for (unsigned worker = 0; worker < threads; ++worker) {
        workers[worker]->close(worker == 0);
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
