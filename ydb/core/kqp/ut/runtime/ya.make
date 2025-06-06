UNITTEST_FOR(ydb/core/kqp)

FORK_SUBTESTS()

SIZE(MEDIUM)

SRCS(
    kqp_scan_spilling_ut.cpp
    kqp_scan_logging_ut.cpp
    kqp_re2_ut.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/src/client/proto
    ydb/core/kqp
    ydb/core/kqp/counters
    ydb/core/kqp/host
    ydb/core/kqp/provider
    ydb/core/kqp/ut/common
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
