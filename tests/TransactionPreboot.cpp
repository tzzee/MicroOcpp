// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Version.h>

#if MO_ENABLE_V201

#include <MicroOcpp.h>
#include <MicroOcpp/Core/Connection.h>
#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Core/Configuration.h>
#include <MicroOcpp/Core/FilesystemUtils.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Model/Transactions/TransactionService.h>
#include <MicroOcpp/Model/Variables/VariableService.h>
#include <MicroOcpp/Operations/CustomOperation.h>
#include <MicroOcpp/Debug.h>
#include <catch2/catch.hpp>
#include <string>
#include <vector>
#include <cmath>
#include "./helpers/testHelper.h"

#define BASE_TIME "2023-01-01T00:00:00.000Z"

using namespace MicroOcpp;

namespace {

struct TxEventLog {
    bool started = false;
    bool ended = false;
    bool startedTimeValid = false;
};

/**
 * @brief Start MO (v201) with TxStartPoint/TxStopPoint = Authorized and register test handlers
 * @param loopback connection used by MO
 * @param log receives which TransactionEvents reached the server and whether the Started timestamp was valid
 */
void initialize(LoopbackConnection& loopback, TxEventLog& log) {
    mocpp_initialize(loopback,
            ChargerCredentials("test-runner1234"),
            makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail),
            false,
            ProtocolVersion(2,0,1));
    mocpp_set_timer(custom_timer_cb);

    //allow transactions before BootNotification is accepted (as with offline charging)
    declareConfiguration<bool>(MO_CONFIG_EXT_PREFIX "PreBootTransactions", false)->setBool(true);

    getOcppContext()->getModel().getVariableService()->declareVariable<const char*>("TxCtrlr", "TxStartPoint", "")->setString("Authorized");
    getOcppContext()->getModel().getVariableService()->declareVariable<const char*>("TxCtrlr", "TxStopPoint", "")->setString("Authorized");

    getOcppContext()->getOperationRegistry().registerOperation("BootNotification", [] () {
        return new Ocpp16::CustomOperation("BootNotification",
            [] (JsonObject) {}, //ignore req
            [] () {
                auto conf = makeJsonDoc("UnitTests", 1024);
                (*conf)["currentTime"] = BASE_TIME;
                (*conf)["interval"] = 3600;
                (*conf)["status"] = "Accepted";
                return conf;
            });});

    getOcppContext()->getOperationRegistry().registerOperation("TransactionEvent", [&log] () {
        return new Ocpp16::CustomOperation("TransactionEvent",
            [&log] (JsonObject request) {
                const char *eventType = request["eventType"] | "_Undefined";
                if (!strcmp(eventType, "Started")) {
                    log.started = true;
                    Timestamp t;
                    log.startedTimeValid = t.setTime(request["timestamp"] | "_Undefined") && t >= MIN_TIME;
                } else if (!strcmp(eventType, "Ended")) {
                    log.ended = true;
                }
            },
            [] () {
                auto doc = makeJsonDoc("UnitTests", 2 * JSON_OBJECT_SIZE(1));
                (*doc)["idTokenInfo"]["status"] = "Accepted";
                return doc;
            });});
}

} //namespace

TEST_CASE( "Transaction started before the clock is set" ) {
    printf("\nRun %s\n",  "Transaction started before the clock is set");

    //clean state
    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    LoopbackConnection loopback;
    loopback.setConnected(false); //no BootNotification, so the clock stays unset

    TxEventLog log;
    initialize(loopback, log);

    SECTION("Same power cycle: sent once the clock is set, with a corrected timestamp") {

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
        loop();

        auto tx = getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction();
        REQUIRE( tx != nullptr );
        REQUIRE( tx->started );
        REQUIRE( getOcppContext()->getModel().getClock().now() < MIN_TIME );

        loopback.setConnected(true); //BootNotification sets the clock, then the queued TransactionEvent goes out
        loop();

        REQUIRE( getOcppContext()->getModel().getClock().now() >= MIN_TIME );
        REQUIRE( log.started );
        REQUIRE( log.startedTimeValid );

        tx = getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction();
        REQUIRE( tx != nullptr );
        REQUIRE( !tx->silent );

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->endAuthorization();
        loop();

        REQUIRE( log.ended );
    }

    SECTION("Previous power cycle: sent with the current time") {

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
        loop();

        auto tx = getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction();
        REQUIRE( tx != nullptr );
        REQUIRE( tx->started );

        //power cut before the clock was ever set
        mocpp_deinitialize();
        mo_setPowerCycleIdForTest(mo_getPowerCycleId() + 1);

        loopback.setConnected(true);
        initialize(loopback, log);
        loop();

        //NOEX (2026-10-24): the boot time of the Started event cannot be determined anymore. It is not discarded (the
        //charging data must reach the CSMS), but sent with the current time
        REQUIRE( log.started );
        REQUIRE( log.startedTimeValid );
    }

    mocpp_deinitialize();
}

namespace {

struct SentEvent {
    std::string eventType;
    Timestamp timestamp;
    std::vector<std::pair<std::string, Timestamp>> meterValues; //(context, timestamp)
};

/**
 * @brief Start MO (v201) like initialize(), sample the energy register at the begin and end of a transaction, and
 *        record every TransactionEvent with the timestamps of its meter values
 */
void initializeWithMeter(LoopbackConnection& loopback, std::vector<SentEvent>& sent) {
    mocpp_initialize(loopback,
            ChargerCredentials("test-runner1234"),
            makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail),
            false,
            ProtocolVersion(2,0,1));
    mocpp_set_timer(custom_timer_cb);

    declareConfiguration<bool>(MO_CONFIG_EXT_PREFIX "PreBootTransactions", false)->setBool(true);

    auto vs = getOcppContext()->getModel().getVariableService();
    vs->declareVariable<const char*>("TxCtrlr", "TxStartPoint", "")->setString("Authorized");
    vs->declareVariable<const char*>("TxCtrlr", "TxStopPoint", "")->setString("Authorized");
    vs->declareVariable<const char*>("SampledDataCtrlr", "TxStartedMeasurands", "")->setString("Energy.Active.Import.Register");
    vs->declareVariable<const char*>("SampledDataCtrlr", "TxEndedMeasurands", "")->setString("Energy.Active.Import.Register");
    vs->declareVariable<const char*>("SampledDataCtrlr", "TxUpdatedMeasurands", "")->setString("");

    setEnergyMeterInput([] () {return (int) (mtime / 1000);});

    getOcppContext()->getOperationRegistry().registerOperation("BootNotification", [] () {
        return new Ocpp16::CustomOperation("BootNotification",
            [] (JsonObject) {}, //ignore req
            [] () {
                auto conf = makeJsonDoc("UnitTests", 1024);
                (*conf)["currentTime"] = BASE_TIME;
                (*conf)["interval"] = 3600;
                (*conf)["status"] = "Accepted";
                return conf;
            });});

    getOcppContext()->getOperationRegistry().registerOperation("TransactionEvent", [&sent] () {
        return new Ocpp16::CustomOperation("TransactionEvent",
            [&sent] (JsonObject request) {
                SentEvent ev;
                ev.eventType = request["eventType"] | "_Undefined";
                ev.timestamp.setTime(request["timestamp"] | "_Undefined");
                for (JsonObject mv : request["meterValue"].as<JsonArray>()) {
                    Timestamp t;
                    t.setTime(mv["timestamp"] | "_Undefined");
                    ev.meterValues.emplace_back(mv["sampledValue"][0]["context"] | "Sample.Periodic", t);
                }
                sent.push_back(ev);
            },
            [] () {
                auto doc = makeJsonDoc("UnitTests", 2 * JSON_OBJECT_SIZE(1));
                (*doc)["idTokenInfo"]["status"] = "Accepted";
                return doc;
            });});
}

const SentEvent *findEvent(const std::vector<SentEvent>& sent, const char *eventType) {
    for (const auto& ev : sent) {
        if (ev.eventType == eventType) {
            return &ev;
        }
    }
    return nullptr;
}

/**
 * @brief Start and stop a transaction while the clock is not set (no BootNotification), 60 seconds apart
 */
void runPrebootTransaction() {
    getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
    loop();
    REQUIRE( getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction() != nullptr );
    mtime += 60000;
    loop();
    getOcppContext()->getModel().getTransactionService()->getEvse(1)->endAuthorization();
    loop();
    REQUIRE( getOcppContext()->getModel().getClock().now() < MIN_TIME );
}

/**
 * @brief Every event and meter value carries a valid time before the clock was set, and the Begin/End values are
 *        taken at the times of the Started/Ended events
 */
void requireCorrectedTimes(const std::vector<SentEvent>& sent) {
    auto started = findEvent(sent, "Started");
    auto ended = findEvent(sent, "Ended");
    REQUIRE( started != nullptr );
    REQUIRE( ended != nullptr );

    Timestamp base;
    base.setTime(BASE_TIME);
    REQUIRE( started->timestamp >= MIN_TIME );
    REQUIRE( started->timestamp < base );
    REQUIRE( ended->timestamp - started->timestamp >= 60 );
    REQUIRE( ended->timestamp - started->timestamp <= 70 );

    size_t nBegin = 0, nEnd = 0;
    for (const auto& ev : sent) {
        for (const auto& mv : ev.meterValues) {
            REQUIRE( mv.second >= MIN_TIME );
            if (mv.first == "Transaction.Begin") {
                REQUIRE( std::abs(mv.second - started->timestamp) <= 3 );
                nBegin++;
            } else if (mv.first == "Transaction.End") {
                REQUIRE( std::abs(mv.second - ended->timestamp) <= 3 );
                nEnd++;
            }
        }
    }
    REQUIRE( nBegin >= 1 );
    REQUIRE( nEnd >= 1 );
}

} //namespace

TEST_CASE( "Timestamps taken before the clock is set are corrected within the same power cycle" ) {
    printf("\nRun %s\n",  "Timestamps taken before the clock is set are corrected within the same power cycle");

    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    LoopbackConnection loopback;
    loopback.setConnected(false); //no BootNotification, so the clock stays unset

    std::vector<SentEvent> sent;
    initializeWithMeter(loopback, sent);

    SECTION("Clock set later in the same OCPP run (e.g. a transaction closed at boot after a power cut)") {

        runPrebootTransaction();

        loopback.setConnected(true); //BootNotification sets the clock, then the queued TransactionEvents go out
        loop();
        loop();

        requireCorrectedTimes(sent);
    }

    SECTION("OCPP re-initialized within the same power cycle (e.g. switched from LOCAL operation back to online)") {

        runPrebootTransaction();

        //the product re-initializes OCPP (bootNr changes, the power cycle does not)
        mocpp_deinitialize();
        loopback.setConnected(true);
        initializeWithMeter(loopback, sent);
        loop();
        loop();

        requireCorrectedTimes(sent);
    }

    SECTION("Power cycle in between: the time is unknown, events are sent with the current time") {

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
        loop();
        REQUIRE( getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction() != nullptr );

        mocpp_deinitialize();
        mo_setPowerCycleIdForTest(mo_getPowerCycleId() + 1);
        loopback.setConnected(true);
        initializeWithMeter(loopback, sent);
        loop();
        loop();

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->endAuthorization();
        loop();
        loop();

        auto started = findEvent(sent, "Started");
        auto ended = findEvent(sent, "Ended");
        REQUIRE( started != nullptr );
        REQUIRE( ended != nullptr );
        REQUIRE( started->timestamp >= MIN_TIME );
        REQUIRE( ended->timestamp >= MIN_TIME );

        for (const auto& ev : sent) {
            for (const auto& mv : ev.meterValues) {
                if (mv.first == "Transaction.Begin") {
                    //taken in the previous power cycle before the clock was set: left unknown, not a plausible wrong time
                    REQUIRE( mv.second < MIN_TIME );
                } else {
                    REQUIRE( mv.second >= MIN_TIME );
                }
            }
        }
    }

    mocpp_deinitialize();
}

TEST_CASE( "Timestamps taken before the clock is set are corrected after 2^31 ms of uptime" ) {
    printf("\nRun %s\n",  "Timestamps taken before the clock is set are corrected after 2^31 ms of uptime");

    //NOEX: adjustPrebootTimestamp cast mocpp_tick_ms() to int before dividing, so from 24.86 to 49.7 days of uptime
    //every time taken before the clock was set collapsed to the time the clock was set (2026-11-16 review)
    const unsigned long savedMtime = mtime;
    mtime = 2200000000UL; //about 25.5 days

    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    LoopbackConnection loopback;
    loopback.setConnected(false); //no BootNotification, so the clock stays unset

    std::vector<SentEvent> sent;
    initializeWithMeter(loopback, sent);

    runPrebootTransaction();

    loopback.setConnected(true); //BootNotification sets the clock, then the queued TransactionEvents go out
    loop();
    loop();

    requireCorrectedTimes(sent);

    mocpp_deinitialize();
    mtime = savedMtime;
}

/**
 * @brief Take Sample.Periodic Ended data every 10 seconds while the clock is not set, and commit it to flash with
 *        Updated events every 20 seconds
 */
static void runPrebootPeriodicSamples() {
    auto vs = getOcppContext()->getModel().getVariableService();
    vs->declareVariable<int>("SampledDataCtrlr", "TxEndedInterval", 0)->setInt(10);
    vs->declareVariable<const char*>("SampledDataCtrlr", "TxUpdatedMeasurands", "")->setString("Energy.Active.Import.Register");
    vs->declareVariable<int>("SampledDataCtrlr", "TxUpdatedInterval", 0)->setInt(20);

    getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
    loop();
    REQUIRE( getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction() != nullptr );
    for (int i = 0; i < 15; i++) {
        loop(); //3 seconds each
    }
    REQUIRE( getOcppContext()->getModel().getClock().now() < MIN_TIME );
}

TEST_CASE( "Periodic Ended data taken before the clock is set is corrected only within its own power cycle" ) {
    printf("\nRun %s\n",  "Periodic Ended data taken before the clock is set is corrected only within its own power cycle");

    //NOEX: Ended data (sampledDataTxEnded) piles up across power cycles. Judged by the power cycle of the Ended event,
    //Sample.Periodic values taken in a previous power cycle were corrected with the current power cycle's base and got
    //a plausible wrong time (2026-11-16 review). Each stored value now carries the power cycle it was taken in

    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    LoopbackConnection loopback;
    loopback.setConnected(false); //no BootNotification, so the clock stays unset

    std::vector<SentEvent> sent;
    initializeWithMeter(loopback, sent);

    Timestamp base;
    base.setTime(BASE_TIME);

    SECTION("Power cycle in between: values of the previous power cycle keep an unknown time") {

        runPrebootPeriodicSamples();

        mocpp_deinitialize();
        mo_setPowerCycleIdForTest(mo_getPowerCycleId() + 1);
        loopback.setConnected(true);
        initializeWithMeter(loopback, sent);
        loop();
        loop();

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->endAuthorization();
        loop();
        loop();

        auto ended = findEvent(sent, "Ended");
        REQUIRE( ended != nullptr );
        size_t nUnknown = 0;
        for (const auto& mv : ended->meterValues) {
            if (mv.first != "Sample.Periodic") {
                continue;
            }
            //either still unknown (taken before the power cycle) or taken after the clock was set, never in between
            REQUIRE( mv.second > Timestamp() ); //sent with a timestamp (not dropped)
            REQUIRE( (mv.second < MIN_TIME || mv.second >= base) );
            if (mv.second < MIN_TIME) {
                nUnknown++;
            }
        }
        REQUIRE( nUnknown >= 1 );
        for (const auto& ev : sent) {
            for (const auto& mv : ev.meterValues) {
                REQUIRE( mv.second > Timestamp() );
                REQUIRE( (mv.second < MIN_TIME || mv.second >= base) );
            }
        }
    }

    SECTION("OCPP re-initialized within the same power cycle: the values are corrected") {

        runPrebootPeriodicSamples();

        mocpp_deinitialize();
        loopback.setConnected(true);
        initializeWithMeter(loopback, sent);
        loop();
        loop();

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->endAuthorization();
        loop();
        loop();

        auto ended = findEvent(sent, "Ended");
        REQUIRE( ended != nullptr );
        size_t nCorrected = 0;
        for (const auto& mv : ended->meterValues) {
            REQUIRE( mv.second >= MIN_TIME );
            if (mv.first == "Sample.Periodic" && mv.second < base) {
                nCorrected++;
            }
        }
        REQUIRE( nCorrected >= 1 );
    }

    mocpp_deinitialize();
}

#endif //MO_ENABLE_V201
