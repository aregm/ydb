#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/kqp/counters/kqp_counters.h>

#include <ydb/public/lib/experimental/ydb_experimental.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {

void CreateSampleTables(TKikimrRunner& kikimr) {
    kikimr.GetTestClient().CreateTable("/Root", R"(
        Name: "FourShard"
        Columns { Name: "Key", Type: "Uint64" }
        Columns { Name: "Value1", Type: "String" }
        Columns { Name: "Value2", Type: "String" }
        KeyColumnNames: ["Key"],
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 100 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 200 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 300 } } } }
    )");

    TTableClient tableClient{kikimr.GetDriver()};
    auto session = tableClient.CreateSession().GetValueSync().GetSession();

    auto result = session.ExecuteDataQuery(R"(
        REPLACE INTO `/Root/FourShard` (Key, Value1, Value2) VALUES
            (1u,   "Value-001",  "1"),
            (2u,   "Value-002",  "2"),
            (101u, "Value-101",  "101"),
            (102u, "Value-102",  "102"),
            (201u, "Value-201",  "201"),
            (202u, "Value-202",  "202"),
            (301u, "Value-301",  "301"),
            (302u, "Value-302",  "302")
    )", TTxControl::BeginTx().CommitTx()).GetValueSync();

    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

    session.Close();
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(KqpFlowControl) {

#if !defined(MKQL_RUNTIME_VERSION) || MKQL_RUNTIME_VERSION >= 9u

void DoFlowControlTest(ui64 limit, bool hasBlockedByCapacity) {
    NKikimrConfig::TAppConfig appCfg;
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetChannelBufferSize(limit);
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetMinChannelBufferSize(limit);
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetScanBufferSize(limit);
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetMkqlHeavyProgramMemoryLimit(200ul << 20);
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetQueryMemoryLimit(20ul << 30);

    TKikimrRunner kikimr{appCfg, "", KikimrDefaultUtDomainRoot};

    CreateSampleTables(kikimr);
    NExperimental::TStreamQueryClient db(kikimr.GetDriver());

    Y_DEFER {
        NYql::NDq::GetDqExecutionSettingsForTests().Reset();
    };

    NYql::NDq::GetDqExecutionSettingsForTests().FlowControl.MaxOutputChunkSize = limit;
    NYql::NDq::GetDqExecutionSettingsForTests().FlowControl.InFlightBytesOvercommit = 1.0f;

    auto settings = NExperimental::TExecuteStreamQuerySettings()
        .ProfileMode(NExperimental::EStreamQueryProfileMode::Full);

    auto result = db.ExecuteStreamQuery(R"(
            $r = (select * from `/Root/FourShard` where Key > 201);

            SELECT l.Key as key, l.Text as text, r.Value1 as value
            FROM `/Root/EightShard` AS l JOIN $r AS r ON l.Key = r.Key
            ORDER BY key, text, value
        )", settings).GetValueSync();

    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

    TVector<TString> profiles;
    CompareYson(R"([
            [[202u];["Value2"];["Value-202"]];
            [[301u];["Value1"];["Value-301"]];
            [[302u];["Value2"];["Value-302"]]
        ])", StreamResultToYson(result, &profiles));

    UNIT_ASSERT_EQUAL(1, profiles.size());

    NYql::NDqProto::TDqExecutionStats stats;
    google::protobuf::TextFormat::ParseFromString(profiles[0], &stats);
    UNIT_ASSERT(stats.IsInitialized());

    ui32 blockedByCapacity = 0;
    for (const auto& stage : stats.GetStages()) {
        for (const auto& ca : stage.GetComputeActors()) {
            for (const auto& task : ca.GetTasks()) {
                for (const auto& output : task.GetOutputChannels()) {
                    blockedByCapacity += output.GetBlockedByCapacity();
                }
            }
        }
    }

    UNIT_ASSERT_EQUAL(hasBlockedByCapacity, blockedByCapacity > 0);
}

Y_UNIT_TEST(FlowControl_Unlimited) {
    DoFlowControlTest(100ul << 20, false);
}

Y_UNIT_TEST(FlowControl_BigLimit) {
    DoFlowControlTest(1ul << 10, false);
}

Y_UNIT_TEST(FlowControl_SmallLimit) {
    DoFlowControlTest(1ul, true);
}

//Y_UNIT_TEST(SlowClient) {
void SlowClient() {
    NKikimrConfig::TAppConfig appCfg;
    appCfg.MutableTableServiceConfig()->MutableResourceManager()->SetChannelBufferSize(1);

    TKikimrRunner kikimr(appCfg);

    {
        TTableClient tableClient{kikimr.GetDriver()};
        auto session = tableClient.CreateSession().GetValueSync().GetSession();
        auto value = std::string(1000, 'a');

        for (int q = 0; q < 100; ++q) {
            TStringBuilder query;
            query << "REPLACE INTO [/Root/KeyValue] (Key, Value) VALUES (" << q << ", \"" << value << "\")";

            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
    }

    NExperimental::TStreamQueryClient db(kikimr.GetDriver());

    auto it = db.ExecuteStreamQuery("SELECT Key, Value FROM `/Root/KeyValue`").GetValueSync();
    auto part = it.ReadNext().GetValueSync();

    auto counters = kikimr.GetTestServer().GetRuntime()->GetAppData(0).Counters;
    TKqpCounters kqpCounters(counters);

    UNIT_ASSERT_EQUAL(kqpCounters.RmComputeActors->Val(), 2);

    Cerr << "-- got value and go sleep...\n";
    ::Sleep(TDuration::Seconds(3));
    Cerr << "-- go on...\n";

    UNIT_ASSERT_EQUAL(kqpCounters.RmComputeActors->Val(), 2);

    // consume 990 elements
    int remains = 990;
    while (remains > 0) {
        if (part.HasResultSet()) {
            part.ExtractResultSet();
            --remains;
            ::Sleep(TDuration::MilliSeconds(10));
            Cerr << "-- remains: " << remains << Endl;
        }
        part = it.ReadNext().GetValueSync();
        UNIT_ASSERT(!part.EOS());
    }

    UNIT_ASSERT_EQUAL(kqpCounters.RmComputeActors->Val(), 2);

    while (!part.EOS()) {
        part = it.ReadNext().GetValueSync();
    }

    UNIT_ASSERT_EQUAL(kqpCounters.RmComputeActors->Val(), 0);
    UNIT_ASSERT_EQUAL(kqpCounters.RmMemory->Val(), 0);
}

#endif

} // suite

} // namespace NKikimr::NKqp
