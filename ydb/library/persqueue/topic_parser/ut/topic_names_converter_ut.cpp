#include <ydb/library/persqueue/topic_parser/topic_parser.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NPersQueue::NTests {

class TConverterTestWrapper {
public:
    TConverterTestWrapper(bool firstClass, const TString& pqRoot, const TString& localDc)
        : Factory(firstClass, pqRoot, localDc)
    {}

    void SetConverter(const TString& topic, const TString& dc, const TString& database) {
        DiscoveryConverter = Factory.MakeDiscoveryConverter(topic, {}, dc, database);
    }

    void SetConverter(NKikimrPQ::TPQTabletConfig& pqTabletConfig) {
        TopicConverter = Factory.MakeTopicConverter(pqTabletConfig);
    }

    void BasicLegacyModeChecks() {
        UNIT_ASSERT(DiscoveryConverter->IsValid());
        UNIT_ASSERT_VALUES_EQUAL(
                DiscoveryConverter->GetPrimaryPath(),
                TString(TStringBuilder() << "/Root/PQ/" << DiscoveryConverter->FullLegacyName)
        );
        UNIT_ASSERT(!DiscoveryConverter->FullLegacyName.empty());
        UNIT_ASSERT(!DiscoveryConverter->ShortLegacyName.empty());
        UNIT_ASSERT_VALUES_EQUAL(DiscoveryConverter->GetInternalName(), DiscoveryConverter->GetPrimaryPath());
    }

    void BasicFirstClassChecks() {
        UNIT_ASSERT_C(DiscoveryConverter->IsValid(), DiscoveryConverter->GetReason());
        UNIT_ASSERT_VALUES_EQUAL(
                DiscoveryConverter->GetPrimaryPath(),
                DiscoveryConverter->FullModernPath
        );

        UNIT_ASSERT_VALUES_EQUAL(DiscoveryConverter->GetInternalName(), DiscoveryConverter->GetPrimaryPath());
    }

    void SetDatabase(const TString& database) {
        DiscoveryConverter->SetDatabase(database);
    }

    TString GetAccount() {
        return *DiscoveryConverter->GetAccount_();
    }

#define WRAPPER_METHOD(NAME)             \
    TString Get##NAME() const {           \
        return DiscoveryConverter->NAME; \
    }

    WRAPPER_METHOD(ShortLegacyName);
    WRAPPER_METHOD(FullLegacyName);
    WRAPPER_METHOD(Dc);

#undef WRAPPER_METHOD

    TTopicNamesConverterFactory Factory;
    TDiscoveryConverterPtr DiscoveryConverter;
    TTopicConverterPtr TopicConverter;

};

Y_UNIT_TEST_SUITE(DiscoveryConverterTest) {
    Y_UNIT_TEST(FullLegacyNames) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("rt3.dc1--account--topic", "", "");
        UNIT_ASSERT_C(wrapper.DiscoveryConverter->IsValid(), wrapper.DiscoveryConverter->GetReason());
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetShortLegacyName(), "account--topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc1--account--topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetDc(), "dc1");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc1--account--topic");

        wrapper.SetConverter("account--topic", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc1--account--topic");

        wrapper.SetConverter("account--topic", "dc2", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc2--account--topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetDc(), "dc2");
    }

    Y_UNIT_TEST(FullLegacyPath) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("/Root/PQ/rt3.dc1--account--topic", "", "/Root");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetInternalName(), "Root/PQ/rt3.dc1--account--topic");
    }

    Y_UNIT_TEST(MinimalName) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("rt3.dc1--topic", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetShortLegacyName(), "topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc1--topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc1--topic");

        wrapper.SetConverter("topic", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc1--topic");

        wrapper.SetConverter("topic", "dc2", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetFullLegacyName(), "rt3.dc2--topic");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc2--topic");
    }

    Y_UNIT_TEST(FullLegacyNamesWithRootDatabase) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("rt3.dc1--account--topic", "", "/Root");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc1--account--topic");
    }

    Y_UNIT_TEST(WithLogbrokerPath) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("account/topic", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc1--account--topic");

        wrapper.SetConverter("account/topic", "dc2", "/Root");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "Root/PQ/rt3.dc2--account--topic");
    }

    Y_UNIT_TEST(AccountDatabase) {
        TConverterTestWrapper wrapper(false, "/Root/PQ", TString("dc1"));
        wrapper.SetConverter("account/topic", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetAccount(), "account");
        wrapper.SetDatabase("/database");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetSecondaryPath(""), "database/topic");

        wrapper.SetConverter("account2/topic2", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetSecondaryPath("database2"), "database2/topic2");

        wrapper.SetConverter("rt3.dc1--account3--topic3", "", "");

        wrapper.SetConverter("account/dir1/dir2/topic", "dc3", "");
//        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetSecondaryPath("database3"), "database3/dir1/dir2/topic");
//        UNIT_ASSERT_VALUES_EQUAL(wrapper.GetClientsideName(), "rt3.dc3--account@dir1@dir2--topic");

    }

    Y_UNIT_TEST(FirstClass) {
        TConverterTestWrapper wrapper(true, "", "");
        wrapper.SetConverter("account/stream", "", "/database");
        wrapper.BasicFirstClassChecks();
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "database/account/stream");

        wrapper.SetConverter("/somedb/account/stream", "", "/somedb");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "somedb/account/stream");

        wrapper.SetConverter("/somedb2/account/stream", "", "");
        UNIT_ASSERT_VALUES_EQUAL(wrapper.DiscoveryConverter->GetPrimaryPath(), "somedb2/account/stream");
    }
}

Y_UNIT_TEST_SUITE(TopicNameConverterTest) {
        Y_UNIT_TEST(LegacyStyle) {
            TConverterTestWrapper wrapper(false, "/Root/PQ", "dc1");

            NKikimrPQ::TPQTabletConfig pqConfig;
            pqConfig.SetTopicName("rt3.dc1--account--topic");
            pqConfig.SetTopicPath("/Root/PQ/rt3.dc1--account--topic");
            pqConfig.SetFederationAccount("account");
            pqConfig.SetLocalDC(true);
            pqConfig.SetYdbDatabasePath("");
            wrapper.SetConverter(pqConfig);

            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "account/topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetCluster(), "dc1");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetTopicForSrcId(), "rt3.dc1--account--topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetTopicForSrcIdHash(), "account--topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetInternalName(), "Root/PQ/rt3.dc1--account--topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetClientsideName(), "rt3.dc1--account--topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetModernName(), "topic");

            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetPrimaryPath(), "Root/PQ/rt3.dc1--account--topic");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "account/topic");
        }
        Y_UNIT_TEST(Paths) {
            TConverterTestWrapper wrapper(false, "/Root/PQ", "dc1");
            {
                NKikimrPQ::TPQTabletConfig pqConfig;
                pqConfig.SetTopicName("rt3.dc1--account@path--topic");
                pqConfig.SetTopicPath("/lb/account-database/path/topic");
                pqConfig.SetFederationAccount("account");
                pqConfig.SetDC("dc1");
                pqConfig.SetLocalDC(true);

                pqConfig.SetYdbDatabasePath("/lb/account-database");

                wrapper.SetConverter(pqConfig);

                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetPrimaryPath(), "lb/account-database/path/topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetSecondaryPath(), "Root/PQ/rt3.dc1--account@path--topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetModernName(), "path/topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetClientsideName(), "rt3.dc1--account@path--topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "account/path/topic");
            }
            {
                NKikimrPQ::TPQTabletConfig pqConfig;
                pqConfig.SetTopicName("rt3.dc2--account@path--topic");
                pqConfig.SetLocalDC(false);
                pqConfig.SetTopicPath("/lb/account-database/path/.topic/mirrored-from-dc2");
                pqConfig.SetFederationAccount("account");
                pqConfig.SetYdbDatabasePath("/lb/account-database");

                wrapper.SetConverter(pqConfig);

                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetPrimaryPath(), "lb/account-database/path/.topic/mirrored-from-dc2");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetSecondaryPath(), "Root/PQ/rt3.dc2--account@path--topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetModernName(), "path/.topic/mirrored-from-dc2");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetClientsideName(), "rt3.dc2--account@path--topic");
                UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "account/path/topic");
            }

        }
        Y_UNIT_TEST(FirstClass) {
            TConverterTestWrapper wrapper(true, "", "");

            NKikimrPQ::TPQTabletConfig pqConfig;
            pqConfig.SetTopicName("my-stream");
            pqConfig.SetTopicPath("/lb/database/my-stream");
            pqConfig.SetYdbDatabasePath("/lb/database");
            wrapper.SetConverter(pqConfig);

            //UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "my-stream");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetTopicForSrcId(), "lb/database/my-stream");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetTopicForSrcIdHash(), "lb/database/my-stream");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetModernName(), "my-stream");
            UNIT_ASSERT_VALUES_EQUAL(wrapper.TopicConverter->GetFederationPath(), "my-stream");
        }
}
} // NTests