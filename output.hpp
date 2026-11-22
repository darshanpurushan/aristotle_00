#pragma once

#include "input.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

namespace shopping {

inline void checked(arrow::Status status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

template<class Value>
Value checked(arrow::Result<Value> result) {
    if (!result.ok()) throw std::runtime_error(result.status().ToString());
    return std::move(result).ValueUnsafe();
}

inline std::unique_ptr<parquet::arrow::FileWriter> open_parquet(
    const fs::path& path, const std::shared_ptr<arrow::Schema>& schema) {
    parquet::WriterProperties::Builder options;
    // user_id / campaign_id are (nearly) unique, so dictionary encoding only costs time before
    // it falls back to plain; statistics are never read; compression buys little on random IDs.
    options.compression(parquet::Compression::UNCOMPRESSED);
    options.disable_statistics();
    options.disable_dictionary("user_id");
    options.disable_dictionary("campaign_id");
    options.disable_dictionary("channel");
    auto sink = checked(arrow::io::FileOutputStream::Open(path.string()));
    return checked(parquet::arrow::FileWriter::Open(
        *schema, arrow::default_memory_pool(), std::move(sink), options.build()));
}

inline const auto user_schema = arrow::schema({
    arrow::field("user_id", arrow::utf8(), false),
    arrow::field("day", arrow::date32(), false),
    arrow::field("sessions", arrow::int64(), false),
    arrow::field("events", arrow::int64(), false),
    arrow::field("revenue_cents", arrow::int64(), false),
    arrow::field("top_category", arrow::utf8())
});
inline const auto campaign_schema = arrow::schema({
    arrow::field("campaign_id", arrow::utf8(), false),
    arrow::field("channel", arrow::utf8(), false),
    arrow::field("day", arrow::date32(), false),
    arrow::field("sessions", arrow::int64(), false),
    arrow::field("purchasing_sessions", arrow::int64(), false),
    arrow::field("revenue_cents", arrow::int64(), false),
    arrow::field("assisted_sessions", arrow::int64(), false)
});

template<class Builder>
std::shared_ptr<arrow::Array> finish(Builder& builder) {
    return checked(builder.Finish());
}

class UserWriter {
public:
    explicit UserWriter(fs::path path) : path_(std::move(path)) {}

    void append(View user, int day, i64 sessions, i64 events, i64 revenue, const View* category) {
        checked(users_.Append(user));
        checked(days_.Append(day));
        checked(sessions_.Append(sessions));
        checked(events_.Append(events));
        checked(revenues_.Append(revenue));
        checked(category ? categories_.Append(*category) : categories_.AppendNull());
        if (users_.length() == 65'536) flush();
    }

    void close(bool require_file) {
        if (require_file || users_.length()) flush();
        if (writer_) checked(writer_->Close());
    }

private:
    void flush() {
        if (!writer_) writer_ = open_parquet(path_, user_schema);
        auto length = users_.length();
        auto batch = arrow::RecordBatch::Make(user_schema, length, {
            finish(users_), finish(days_), finish(sessions_), finish(events_),
            finish(revenues_), finish(categories_)
        });
        checked(writer_->WriteRecordBatch(*batch));
    }

    fs::path path_;
    std::unique_ptr<parquet::arrow::FileWriter> writer_;
    arrow::StringBuilder users_, categories_;
    arrow::Date32Builder days_;
    arrow::Int64Builder sessions_, events_, revenues_;
};

struct CampaignTotal {
    i64 sessions = 0;
    i64 purchasing = 0;
    i64 revenue = 0;
    i64 assists = 0;

    CampaignTotal& operator+=(const CampaignTotal& other) {
        sessions += other.sessions;
        purchasing += other.purchasing;
        revenue += other.revenue;
        assists += other.assists;
        return *this;
    }
};

struct Campaign {
    View id, channel;
};

inline void write_campaigns(const fs::path& path, const std::vector<Campaign>& campaigns,
                            const std::vector<CampaignTotal>& totals, int first_day) {
    arrow::StringBuilder ids, channels;
    arrow::Date32Builder days;
    arrow::Int64Builder sessions, purchasing, revenue, assists;
    for (size_t row = 0; row < totals.size(); ++row) {
        const CampaignTotal& total = totals[row];
        if (!total.sessions && !total.assists) continue;
        const Campaign& campaign = campaigns[row % campaigns.size()];
        checked(ids.Append(campaign.id));
        checked(channels.Append(campaign.channel));
        checked(days.Append(first_day + row / campaigns.size()));
        checked(sessions.Append(total.sessions));
        checked(purchasing.Append(total.purchasing));
        checked(revenue.Append(total.revenue));
        checked(assists.Append(total.assists));
    }
    auto writer = open_parquet(path, campaign_schema);
    auto length = ids.length();
    auto batch = arrow::RecordBatch::Make(campaign_schema, length, {
        finish(ids), finish(channels), finish(days), finish(sessions),
        finish(purchasing), finish(revenue), finish(assists)
    });
    checked(writer->WriteRecordBatch(*batch));
    checked(writer->Close());
}
}
