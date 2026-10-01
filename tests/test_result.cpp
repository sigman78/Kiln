#include "kiln_test.h"

#include "kiln/log.h"
#include "kiln/result.h"

#include <cstring>
#include <utility>

using namespace kiln;

namespace {

// Helpers for KILN_TRY / KILN_TRY_ASSIGN.
Status may_fail(bool fail) {
    if (fail) return make_status(Code::InvalidArgument, 7);
    return kOk;
}

Status uses_try_status(bool fail) {
    KILN_TRY(may_fail(fail));
    return kOk;
}

Result<int> compute(bool fail) {
    if (fail) return Code::NotFound;
    return 42;
}

Status uses_try_result(bool fail) {
    KILN_TRY(compute(fail));
    return kOk;
}

Result<int> uses_try_assign(bool fail) {
    KILN_TRY_ASSIGN(auto v, compute(fail));
    return v + 1;
}

// Non-trivially-destructible payload.
struct Tracked {
    static int liveCount;
    int value;
    explicit Tracked(int v) : value(v) { ++liveCount; }
    Tracked(Tracked const& o) noexcept : value(o.value) { ++liveCount; }
    Tracked(Tracked&& o) noexcept : value(o.value) {
        ++liveCount;
        o.value = -1;
    }
    ~Tracked() { --liveCount; }
    Tracked& operator=(Tracked const&) = delete;
    Tracked& operator=(Tracked&&)      = delete;
};
int Tracked::liveCount = 0;

// Captures what diagf delivers to a DiagSink.
struct Captured {
    bool called       = false;
    u32 code          = 0;
    Severity severity = Severity::Info;
    Status status     = kOk;
    char asset[64]    = {};
    char where[64]    = {};
    char message[256] = {};
};

void capture_fn(void* user, Diagnostic const& d) {
    Captured* c = static_cast<Captured*>(user);
    c->called   = true;
    c->code     = d.code;
    c->severity = d.severity;
    c->status   = d.status;
    // StrViews in `d` are only valid for the duration of this callback: copy now.
    format(c->asset, sizeof c->asset, "%.*s", KILN_SV(d.asset));
    format(c->where, sizeof c->where, "%.*s", KILN_SV(d.where));
    format(c->message, sizeof c->message, "%.*s", KILN_SV(d.message));
}

} // namespace

KILN_TEST(Result, StatusOkFailed) {
    KILN_CHECK(kOk.ok());
    KILN_CHECK(!kOk.failed());
    KILN_CHECK(kOk.code == Code::Ok);

    Status s = make_status(Code::NotFound, 3);
    KILN_CHECK(!s.ok());
    KILN_CHECK(s.failed());
    KILN_CHECK(s.code == Code::NotFound);
    KILN_CHECK_EQ(s.detail, u16(3));
}

KILN_TEST(Result, StatusEquality) {
    Status a = make_status(Code::IoError, 5);
    Status b = make_status(Code::IoError, 5);
    Status c = make_status(Code::IoError, 6);
    Status d = make_status(Code::Timeout, 5);
    KILN_CHECK(a == b);
    KILN_CHECK(a != c);
    KILN_CHECK(a != d);
}

KILN_TEST(Result, CodeNameAllValuesNonNullAndKnown) {
    for (u16 i = 0; i < u16(Code::Count); ++i) {
        Code c           = Code(i);
        char const* name = code_name(c);
        KILN_REQUIRE(name != nullptr);
        KILN_CHECK(std::strcmp(name, "?") != 0);
    }
}

KILN_TEST(Result, SeverityNames) {
    KILN_CHECK(std::strcmp(severity_name(Severity::Info), "info") == 0);
    KILN_CHECK(std::strcmp(severity_name(Severity::Warning), "warning") == 0);
    KILN_CHECK(std::strcmp(severity_name(Severity::Error), "error") == 0);
}

KILN_TEST(Result, IntSuccess) {
    Result<int> r = 5;
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r.failed());
    KILN_CHECK_EQ(r.value(), 5);
    KILN_CHECK_EQ(*r, 5);
}

KILN_TEST(Result, IntFailure) {
    Result<int> r = Code::Unknown;
    KILN_REQUIRE(r.failed());
    KILN_CHECK(!r.ok());
    KILN_CHECK(r.code() == Code::Unknown);
}

KILN_TEST(Result, IntFailureFromStatus) {
    Result<int> r = make_status(Code::Busy, 9);
    KILN_REQUIRE(r.failed());
    KILN_CHECK(r.status() == make_status(Code::Busy, 9));
}

KILN_TEST(Result, ValueOr) {
    Result<int> a = 5;
    KILN_CHECK_EQ(a.value_or(99), 5);
    Result<int> b = Code::Unknown;
    KILN_CHECK_EQ(b.value_or(99), 99);
}

KILN_TEST(Result, VoidResultOk) {
    Result<void> r;
    KILN_CHECK(r.ok());
    KILN_CHECK(r.code() == Code::Ok);
}

KILN_TEST(Result, VoidResultFailed) {
    Result<void> r = Code::IoError;
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::IoError);
}

KILN_TEST(Result, TrackedSuccessConstructsAndDestroys) {
    int before = Tracked::liveCount;
    {
        Result<Tracked> r(Tracked(5));
        KILN_REQUIRE(r.ok());
        KILN_CHECK_EQ(r.value().value, 5);
        KILN_CHECK_EQ(Tracked::liveCount, before + 1);
    }
    KILN_CHECK_EQ(Tracked::liveCount, before);
}

KILN_TEST(Result, TrackedFailureNeverConstructs) {
    int before = Tracked::liveCount;
    {
        Result<Tracked> r = Code::NotFound;
        KILN_CHECK(r.failed());
        KILN_CHECK_EQ(Tracked::liveCount, before);
    }
    KILN_CHECK_EQ(Tracked::liveCount, before);
}

KILN_TEST(Result, TrackedMoveKeepsBothAlive) {
    // Result<T>'s move constructor mirrors std::optional: it does not clear the
    // source's engaged/ok state, it just moves the payload. Both `a` and `b`
    // hold a live (destructible) Tracked until they go out of scope.
    int before = Tracked::liveCount;
    {
        Result<Tracked> a(Tracked(1));
        KILN_CHECK_EQ(Tracked::liveCount, before + 1);

        Result<Tracked> b(std::move(a));
        KILN_REQUIRE(b.ok());
        KILN_CHECK_EQ(b.value().value, 1);
        KILN_REQUIRE(a.ok());
        KILN_CHECK_EQ(a.value().value, -1); // moved-from sentinel
        KILN_CHECK_EQ(Tracked::liveCount, before + 2);
    }
    KILN_CHECK_EQ(Tracked::liveCount, before);
}

KILN_TEST(Result, TrackedCopy) {
    int before = Tracked::liveCount;
    {
        Result<Tracked> a(Tracked(3));
        Result<Tracked> b(a);
        KILN_CHECK_EQ(a.value().value, 3);
        KILN_CHECK_EQ(b.value().value, 3);
        KILN_CHECK_EQ(Tracked::liveCount, before + 2);
    }
    KILN_CHECK_EQ(Tracked::liveCount, before);
}

KILN_TEST(Result, TryStatusPropagatesFailure) {
    Status s = uses_try_status(true);
    KILN_CHECK(s.failed());
    KILN_CHECK(s.code == Code::InvalidArgument);
    KILN_CHECK_EQ(s.detail, u16(7));
}

KILN_TEST(Result, TryStatusPassesThrough) {
    Status s = uses_try_status(false);
    KILN_CHECK(s.ok());
}

KILN_TEST(Result, TryResultPropagatesFailure) {
    Status s = uses_try_result(true);
    KILN_CHECK(s.failed());
    KILN_CHECK(s.code == Code::NotFound);
}

KILN_TEST(Result, TryResultPassesThrough) {
    Status s = uses_try_result(false);
    KILN_CHECK(s.ok());
}

KILN_TEST(Result, TryAssignPropagatesFailure) {
    Result<int> r = uses_try_assign(true);
    KILN_REQUIRE(r.failed());
    KILN_CHECK(r.code() == Code::NotFound);
}

KILN_TEST(Result, TryAssignPassesThroughValue) {
    Result<int> r = uses_try_assign(false);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r.value(), 43);
}

KILN_TEST(Result, DiagfDeliversToSink) {
    Captured cap;
    DiagSink sink{&capture_fn, &cap};
    Status ret = diagf(&sink, make_status(Code::ParseError, 2), u32(1234), Severity::Warning,
                       "assets/foo.gltf"_sv, "node[3]"_sv, "bad value %d", 7);

    KILN_REQUIRE(cap.called);
    KILN_CHECK_EQ(cap.code, u32(1234));
    KILN_CHECK(cap.severity == Severity::Warning);
    KILN_CHECK(cap.status == make_status(Code::ParseError, 2));
    KILN_CHECK(std::strcmp(cap.asset, "assets/foo.gltf") == 0);
    KILN_CHECK(std::strcmp(cap.where, "node[3]") == 0);
    KILN_CHECK(std::strcmp(cap.message, "bad value 7") == 0);
    KILN_CHECK(ret == make_status(Code::ParseError, 2));
}

KILN_TEST(Result, DiagfNullSinkIsNoOpAndReturnsStatus) {
    Status ret =
        diagf(nullptr, make_status(Code::Busy), 0, Severity::Error, StrView{}, StrView{}, "irrelevant");
    KILN_CHECK(ret.code == Code::Busy);
}

KILN_TEST(Result, DiagfSinkWithNullFnIsNoOp) {
    DiagSink sink{}; // fn == nullptr
    Status ret = diagf(&sink, kOk, 0, Severity::Info, StrView{}, StrView{}, "irrelevant");
    KILN_CHECK(ret.ok());
}

namespace {
Result<int> parse_positive(int v) {
    if (v <= 0) return Code::InvalidArgument;
    return v;
}
} // namespace

KILN_TEST(Result, AndThenChainsOnSuccess) {
    Result<int> r = parse_positive(4).and_then([](int v) -> Result<int> { return v * 10; });
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(*r, 40);
}

KILN_TEST(Result, AndThenForwardsFailureWithoutCalling) {
    bool called   = false;
    Result<int> r = parse_positive(-1).and_then([&](int v) -> Result<int> {
        called = true;
        return v;
    });
    KILN_CHECK(!called);
    KILN_CHECK(r.failed());
    KILN_CHECK_EQ(r.code(), Code::InvalidArgument);
}

KILN_TEST(Result, TransformWrapsValueAndChangesType) {
    Result<u64> r = parse_positive(7).transform([](int v) { return u64(v) * 3; });
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(*r, u64(21));
    Result<u64> f = parse_positive(0).transform([](int v) { return u64(v); });
    KILN_CHECK(f.failed());
}

KILN_TEST(Result, OrElseRecovers) {
    Result<int> r = parse_positive(-5).or_else([](Status s) -> Result<int> {
        return s.code == Code::InvalidArgument ? Result<int>(1) : Result<int>(s);
    });
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(*r, 1);
    Result<int> untouched = parse_positive(9).or_else([](Status) -> Result<int> { return 0; });
    KILN_REQUIRE(untouched.ok());
    KILN_CHECK_EQ(*untouched, 9);
}

KILN_TEST(Result, VoidAndThenTransform) {
    Result<void> okv;
    Result<int> a = okv.and_then([] { return Result<int>(3); });
    KILN_REQUIRE(a.ok());
    KILN_CHECK_EQ(*a, 3);
    Result<void> bad = Code::Busy;
    Result<int> b    = bad.transform([] { return 5; });
    KILN_CHECK(b.failed());
    KILN_CHECK_EQ(b.code(), Code::Busy);
}
