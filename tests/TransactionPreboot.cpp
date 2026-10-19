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

    SECTION("Previous power cycle: still treated as unrecoverable") {

        getOcppContext()->getModel().getTransactionService()->getEvse(1)->beginAuthorization("mIdToken", false);
        loop();

        auto tx = getOcppContext()->getModel().getTransactionService()->getEvse(1)->getTransaction();
        REQUIRE( tx != nullptr );
        REQUIRE( tx->started );

        //power cut before the clock was ever set
        mocpp_deinitialize();

        loopback.setConnected(true);
        initialize(loopback, log);
        loop();

        //the boot time of the Started event cannot be determined anymore, so it is not sent
        REQUIRE( !log.started );
    }

    mocpp_deinitialize();
}

#endif //MO_ENABLE_V201
