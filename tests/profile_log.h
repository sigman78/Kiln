// tests/profile_log.h — ProfileHooks that keep every call, for tests of kiln/profile.h.
#pragma once

#include "kiln/containers.h"
#include "kiln/profile.h"

#include <cstring>
#include <mutex>
#include <thread>

namespace kiln::test {

class ProfileLog {
public:
    struct Event {
        char kind; ///< 'B' zone begin, 'E' zone end, 'I' interval
        char const* name;
        char asset[96];
        std::thread::id thread;
        u64 beginNs, endNs;
    };

    [[nodiscard]] ProfileHooks hooks() noexcept {
        return {.zone_begin = &begin, .zone_end = &end, .interval = &interval, .user = this};
    }

    /// Events named `name`, of `kind`; with `asset`, only those for it.
    [[nodiscard]] u32 count(char kind, char const* name, char const* asset = nullptr) {
        std::lock_guard<std::mutex> const lock(mutex_);
        u32 n = 0;
        for (Event const& e : events_)
            n += e.kind == kind && std::strcmp(e.name, name) == 0 &&
                 (!asset || std::strcmp(e.asset, asset) == 0);
        return n;
    }

    /// Every zone_end matches the innermost open zone of its thread, every zone is closed, and no
    /// interval ends before it begins.
    [[nodiscard]] bool well_formed() {
        std::lock_guard<std::mutex> const lock(mutex_);
        Vec<usize> open(default_allocator(), Tag::Test);
        for (usize i = 0; i < events_.size(); ++i) {
            Event const& e = events_[i];
            if (e.kind == 'I' && e.endNs < e.beginNs) return false;
            if (e.kind == 'B') open.push_back(i);
            if (e.kind != 'E') continue;
            usize j = open.size();
            while (j > 0 && events_[open[j - 1]].thread != e.thread)
                --j;
            if (j == 0) return false;
            Event const& b = events_[open[j - 1]];
            if (b.name != e.name || std::strcmp(b.asset, e.asset) != 0) return false;
            for (usize k = j - 1; k + 1 < open.size(); ++k)
                open[k] = open[k + 1];
            open.pop_back();
        }
        return open.empty();
    }

private:
    void add(char kind, char const* name, StrView asset, u64 b, u64 e) {
        Event ev{kind, name, {}, std::this_thread::get_id(), b, e};
        usize const n = asset.size < sizeof ev.asset - 1 ? asset.size : sizeof ev.asset - 1;
        if (n) std::memcpy(ev.asset, asset.data, n);
        std::lock_guard<std::mutex> const lock(mutex_);
        events_.push_back(ev);
    }
    static void begin(void* u, char const* name, StrView asset) {
        static_cast<ProfileLog*>(u)->add('B', name, asset, 0, 0);
    }
    static void end(void* u, char const* name, StrView asset) {
        static_cast<ProfileLog*>(u)->add('E', name, asset, 0, 0);
    }
    static void interval(void* u, char const* name, StrView asset, u64 b, u64 e) {
        static_cast<ProfileLog*>(u)->add('I', name, asset, b, e);
    }

    std::mutex mutex_;
    Vec<Event> events_{default_allocator(), Tag::Test};
};

} // namespace kiln::test
