#include "ampere_startup_requirements.h"
#include "ampere_diagnostics.h"
#include <cstring>
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    using ampere_startup_requirements::Answer;
    using ampere_startup_requirements::Decide;
    using ampere_startup_requirements::Query;
    Query adapter{};
    adapter.success = true; adapter.adapterAnswer = true;
    Check(Decide(adapter) == Answer::eAdapterSupported, "NGX adapter answer becomes Supported without a minimum architecture");
    Query other{};
    other.success = true;
    Check(Decide(other) == Answer::eNative, "other successful answers (driver, OS) stay native");
    Query missing{};
    missing.featureMissing = true;
    Check(Decide(missing) == Answer::eNative, "FeatureNotFound without a prepared provider stays native");
    missing.providerReady = true;
    Check(Decide(missing) == Answer::eReadyProvider, "FeatureNotFound with the prepared provider becomes Supported");
    Query failed{};
    failed.providerReady = true;
    Check(Decide(failed) == Answer::eNative, "other NGX failures stay native even with a prepared provider");
    Query ota{};
    ota.featureMissing = true; ota.slInit = true; ota.eligibleOta = true;
    Check(Decide(ota) == Answer::eEligibleOta, "slInit FeatureNotFound with an eligible pre-scanned OTA provider becomes Supported");
    ota.eligibleOta = false;
    Check(Decide(ota) == Answer::eNative, "slInit FeatureNotFound without an eligible OTA provider stays native");
    ota.eligibleOta = true; ota.slInit = false;
    Check(Decide(ota) == Answer::eNative, "after slInit the OTA candidate alone never answers; the prepared provider does");
    ota.slInit = true; ota.providerReady = true;
    Check(Decide(ota) == Answer::eReadyProvider, "a prepared provider takes precedence over the OTA candidate");
    Query otaFailed{};
    otaFailed.slInit = true; otaFailed.eligibleOta = true;
    Check(Decide(otaFailed) == Answer::eNative, "other NGX failures stay native with an eligible OTA provider");
    Query otaOther{};
    otaOther.success = true; otaOther.slInit = true; otaOther.eligibleOta = true;
    Check(Decide(otaOther) == Answer::eNative, "a successful driver/OS refusal stays native with an eligible OTA provider");
    Check(std::strcmp(ampere_startup_requirements::AnswerName(Answer::eNative), "native") == 0
        && std::strcmp(ampere_startup_requirements::AnswerName(Answer::eReadyProvider), "supported-prepared-provider") == 0
        && std::strcmp(ampere_startup_requirements::AnswerName(Answer::eEligibleOta), "supported-eligible-ota-provider") == 0,
        "answers have stable log names");
    // InvalidParameter is the driver's identifier check (driver 617.42), never the GPU.
    using ampere_startup_requirements::ClassifyResult;
    const auto fromResult = [](uint32_t result, bool slInit, bool eligibleOta, bool providerReady) {
        Query query{};
        ClassifyResult(result, query);
        query.slInit = slInit; query.eligibleOta = eligibleOta; query.providerReady = providerReady;
        return query;
    };
    constexpr uint32_t kInvalidParameter = 0xBAD00005, kPlatformError = 0xBAD00002, kNotInitialized = 0xBAD00007;
    Query rejected = fromResult(kInvalidParameter, true, true, false);
    Check(rejected.identifierRejected && !rejected.featureMissing && !rejected.success,
        "InvalidParameter is an identifier rejection, not a missing feature");
    Check(Decide(rejected) == Answer::eEligibleOta,
        "slInit InvalidParameter with an eligible pre-scanned OTA provider becomes Supported");
    Check(Decide(fromResult(kInvalidParameter, false, true, false)) == Answer::eNative,
        "InvalidParameter after slInit stays native, even with an eligible OTA provider");
    Check(Decide(fromResult(kInvalidParameter, true, false, false)) == Answer::eNative,
        "slInit InvalidParameter without an eligible OTA provider stays native");
    Check(Decide(fromResult(kInvalidParameter, true, true, true)) == Answer::eNative
        && Decide(fromResult(kInvalidParameter, false, false, true)) == Answer::eNative,
        "InvalidParameter with a prepared provider stays native (only a missing feature reaches that answer)");
    Check(Decide(fromResult(kPlatformError, true, true, false)) == Answer::eNative
        && Decide(fromResult(kPlatformError, false, false, true)) == Answer::eNative,
        "PlatformError stays native with an eligible OTA provider or a prepared provider");
    Check(Decide(fromResult(kNotInitialized, true, true, false)) == Answer::eNative
        && Decide(fromResult(kNotInitialized, false, false, true)) == Answer::eNative,
        "NotInitialized stays native with an eligible OTA provider or a prepared provider");
    Check(Decide(fromResult(0xBAD00001, true, true, false)) == Answer::eEligibleOta
        && Decide(fromResult(0xBAD00004, false, false, true)) == Answer::eReadyProvider
        && Decide(fromResult(0xBAD00004, true, false, false)) == Answer::eNative,
        "FeatureNotSupported/FeatureNotFound codes keep the slinitfg.2 answers");
    using ampere_startup_requirements::ProjectIdPassesDriverCheck;
    const auto guid = [](const char* id) { return ProjectIdPassesDriverCheck(id, std::strlen(id)); };
    Check(guid("0123456789abcdef0123456789ABCDEF") && guid("01234567-89ab-cdef-0123-456789abcdef")
        && guid("{01234567-89ab-cdef-0123-456789abcdef}") && guid("[(\"0123456789abcdef0123456789abcdef\")]"),
        "project ID check: 32 hex digits, dashes skipped, nested {} [] () \"\" wrappers stripped");
    Check(!guid("") && !ProjectIdPassesDriverCheck(nullptr, 0) && !guid("0123456789abcdef0123456789abcde")
        && !guid("0123456789abcdef0123456789abcdef0") && !guid("0123456789abcdef0123456789abcdeg")
        && !guid("{0123456789abcdef0123456789abcdef") && !guid("FF7Rebirth") && !guid("1.0.0"),
        "project ID check: empty, null, 31 or 33 digits, a non-hex character or an unmatched wrapper fails");
    Check(guid("00000123456789abcdef0123456789ab") && !guid("00000023456789abcdef0123456789ab")
        && !guid("000-000-23456789abcdef0123456789ab") && guid("aaaaaAAAAAbcdef0123456789abcdef0"),
        "project ID check: a run of 6 identical characters fails, 5 passes; dashes do not end a run; case counts");
    Check(ampere_diagnostics::kStartupWrapperMissingFailure == 177
        && std::strstr(ampere_diagnostics::FailureName(177), "DLSS-G plug-in at device setup") != nullptr
        && std::strcmp(ampere_diagnostics::FailureName(170), "early wrapper allocation could not be established") == 0,
        "a missing wrapper at device setup has its own failure, distinct from 170");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
