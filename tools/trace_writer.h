// tools/trace_writer.h — ProfileHooks that record kiln's zones and intervals and write them as a
// Chrome trace (chrome://tracing, ui.perfetto.dev) plus a summary per name. For kiln-cook --trace and
// the examples' KILN_TRACE; a host with a profiler of its own forwards the hooks there instead.
#pragma once

#include "kiln/containers.h"
#include "kiln/core.h"
#include "kiln/log.h"
#include "kiln/profile.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace kiln::cli {

class TraceWriter {
public:
    TraceWriter() noexcept : t0_(profile_now_ns()) {}
    TraceWriter(TraceWriter const&)            = delete;
    TraceWriter& operator=(TraceWriter const&) = delete;

    /// Valid while this writer lives.
    [[nodiscard]] ProfileHooks hooks() noexcept {
        return ProfileHooks{
            .zone_begin = &zone_begin, .zone_end = &zone_end, .interval = &interval, .user = this};
    }

    /// Writes the Chrome trace JSON. False when the file cannot be written.
    bool write(char const* path) noexcept {
        std::lock_guard<std::mutex> const lock(mutex_);
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        std::fputs("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n", f);
        char line[1024];
        bool first = true;
        for (u32 t = 1; t <= threads_.load(); ++t) {
            format(line, sizeof line,
                   "%s{\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"name\":\"thread_name\",\"args\":{\"name\":"
                   "\"thread %u\"}}",
                   first ? "" : ",\n", t, t);
            std::fputs(line, f);
            first = false;
        }
        for (Event const& e : events_) {
            char asset[512];
            escape(StrView(strings_.data() + e.assetOff, e.assetLen), asset, sizeof asset);
            double const ts = double(e.ns - t0_) / 1000.0;
            if (e.ph == 'b' || e.ph == 'e')
                format(line, sizeof line,
                       ",\n{\"ph\":\"%c\",\"cat\":\"wait\",\"id\":%u,\"pid\":1,\"tid\":%u,\"ts\":%.3f,"
                       "\"name\":\"%s\","
                       "\"args\":{\"asset\":\"%s\"}}",
                       e.ph, e.id, e.tid, ts, e.name, asset);
            else
                format(line, sizeof line,
                       ",\n{\"ph\":\"%c\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"name\":\"%s\",\"args\":{"
                       "\"asset\":\"%s\"}}",
                       e.ph, e.tid, ts, e.name, asset);
            std::fputs(first ? line + 2 : line, f);
            first = false;
        }
        std::fputs("\n]}\n", f);
        return std::fclose(f) == 0;
    }

    /// One log line per event name: count, total and longest time. Zones of one name on several
    /// threads add up, so a total can exceed the wall time.
    void log_summary() noexcept {
        std::lock_guard<std::mutex> const lock(mutex_);
        struct Row {
            char const* name;
            u32 count;
            u64 totalNs, maxNs;
        };
        Vec<Row> rows(default_allocator(), Tag::Io);
        auto add = [&](char const* name, u64 ns) {
            for (Row& r : rows)
                if (r.name == name) {
                    ++r.count;
                    r.totalNs += ns;
                    r.maxNs = max(r.maxNs, ns);
                    return;
                }
            rows.push_back(Row{name, 1, ns, ns});
        };
        // Zones nest per thread: a stack of open zones per thread. An interval's b and e are adjacent.
        Vec<Vec<usize>> open(default_allocator(), Tag::Io);
        open.resize(threads_.load() + 1);
        for (Vec<usize>& v : open)
            v.init(default_allocator(), Tag::Io);
        for (usize i = 0; i < events_.size(); ++i) {
            Event const& e = events_[i];
            if (e.ph == 'b' && i + 1 < events_.size()) {
                add(e.name, events_[i + 1].ns - e.ns);
                ++i;
            } else if (e.ph == 'B') {
                open[e.tid].push_back(i);
            } else if (e.ph == 'E' && !open[e.tid].empty()) {
                Event const& begin = events_[open[e.tid].back()];
                open[e.tid].pop_back();
                add(begin.name, e.ns - begin.ns);
            }
        }
        for (Row const& r : rows)
            KILN_INFO("trace", "%-18s %6u x  total %9.1f ms  max %8.1f ms", r.name, r.count,
                      double(r.totalNs) / 1e6, double(r.maxNs) / 1e6);
    }

private:
    struct Event {
        char ph; ///< B / E: a zone on `tid`; b / e: an interval `id`
        char const* name;
        u32 tid, id;
        u32 assetOff, assetLen;
        u64 ns;
    };

    static u32 thread_index(TraceWriter* w) noexcept {
        thread_local TraceWriter* owner = nullptr;
        thread_local u32 index          = 0;
        if (owner != w) {
            owner = w;
            index = w->threads_.fetch_add(1) + 1;
        }
        return index;
    }
    void add(char ph, char const* name, StrView asset, u32 tid, u32 id, u64 ns, u64 endNs = 0) noexcept {
        std::lock_guard<std::mutex> const lock(mutex_);
        u32 const off = u32(strings_.size());
        strings_.append(Span<char const>(asset.data, asset.size));
        events_.push_back(Event{ph, name, tid, id, off, u32(asset.size), ns});
        if (ph == 'b') events_.push_back(Event{'e', name, tid, id, off, u32(asset.size), endNs});
    }
    static void zone_begin(void* user, char const* name, StrView asset) {
        auto* w = static_cast<TraceWriter*>(user);
        w->add('B', name, asset, thread_index(w), 0, profile_now_ns());
    }
    static void zone_end(void* user, char const* name, StrView asset) {
        auto* w = static_cast<TraceWriter*>(user);
        w->add('E', name, asset, thread_index(w), 0, profile_now_ns());
    }
    static void interval(void* user, char const* name, StrView asset, u64 beginNs, u64 endNs) {
        auto* w      = static_cast<TraceWriter*>(user);
        u32 const id = w->ids_.fetch_add(1) + 1;
        u32 const t  = thread_index(w);
        w->add('b', name, asset, t, id, beginNs, endNs);
    }
    static void escape(StrView s, char* out, usize cap) noexcept {
        usize n = 0;
        for (usize i = 0; i < s.size && n + 3 < cap; ++i) {
            char const c = s.data[i];
            if (c == '"' || c == '\\') out[n++] = '\\';
            out[n++] = (u8(c) < 0x20) ? ' ' : c;
        }
        out[n] = 0;
    }

    u64 t0_;
    std::mutex mutex_;
    Vec<Event> events_{default_allocator(), Tag::Io};
    Vec<char> strings_{default_allocator(), Tag::Io};
    std::atomic<u32> threads_{0};
    std::atomic<u32> ids_{0};
};

} // namespace kiln::cli
