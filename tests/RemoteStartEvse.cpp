// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Version.h>

#if MO_ENABLE_V201

#include <MicroOcpp.h>
#include <MicroOcpp/Core/Connection.h>
#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Core/FilesystemUtils.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Model/RemoteControl/RemoteControlService.h>
#include <MicroOcpp/Model/Transactions/TransactionService.h>
#include <MicroOcpp/Model/Variables/VariableService.h>
#include <MicroOcpp/Debug.h>
#include <catch2/catch.hpp>
#include "./helpers/testHelper.h"

using namespace MicroOcpp;

namespace {

/**
 * @brief Remote start without evseId (0), the same as RequestStartTransaction without the field
 * @param idToken idToken of the remote start
 * @return status of the RequestStartTransaction.conf
 */
RequestStartStopStatus remoteStartWithoutEvseId(const char *idToken) {
    char transactionId [MO_TXID_LEN_MAX + 1] = {'\0'};
    return getOcppContext()->getModel().getRemoteControlService()->requestStartTransaction(
            0, 1, IdToken(idToken, IdToken::Type::Central), transactionId, sizeof(transactionId));
}

} //namespace

TEST_CASE( "Remote start without evseId" ) {
    printf("\nRun %s\n",  "Remote start without evseId");

    //clean state
    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    LoopbackConnection loopback;
    mocpp_initialize(loopback,
            ChargerCredentials("test-runner1234"),
            makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail),
            false,
            ProtocolVersion(2,0,1));
    mocpp_set_timer(custom_timer_cb);
    loop();

    auto txService = getOcppContext()->getModel().getTransactionService();

    SECTION("Free station: starts on EVSE 1, never on EVSE 0") {
        REQUIRE( remoteStartWithoutEvseId("REMOTE1") == RequestStartStopStatus_Accepted );
        REQUIRE( txService->getEvse(1)->getTransaction() != nullptr );
        REQUIRE( !strcmp(txService->getEvse(1)->getTransaction()->idToken.get(), "REMOTE1") );
        REQUIRE( txService->getEvse(0)->getTransaction() == nullptr );
    }

    SECTION("EVSE 1 occupied: takes the next free EVSE and keeps EVSE 1's transaction") {
        txService->getEvse(1)->beginAuthorization("mIdToken", false);
        loop();
        REQUIRE( remoteStartWithoutEvseId("REMOTE1") == RequestStartStopStatus_Accepted );
        REQUIRE( !strcmp(txService->getEvse(1)->getTransaction()->idToken.get(), "mIdToken") );
        REQUIRE( txService->getEvse(2)->getTransaction() != nullptr );
        REQUIRE( !strcmp(txService->getEvse(2)->getTransaction()->idToken.get(), "REMOTE1") );
        REQUIRE( txService->getEvse(0)->getTransaction() == nullptr );
    }

    SECTION("All EVSEs occupied: rejected (accepted on EVSE 0 before 2026-10-22)") {
        for (unsigned int evseId = 1; evseId < MO_NUM_EVSEID; evseId++) {
            txService->getEvse(evseId)->beginAuthorization("mIdToken", false);
        }
        loop();
        REQUIRE( remoteStartWithoutEvseId("REMOTE1") == RequestStartStopStatus_Rejected );
        REQUIRE( txService->getEvse(0)->getTransaction() == nullptr );
        for (unsigned int evseId = 1; evseId < MO_NUM_EVSEID; evseId++) {
            REQUIRE( !strcmp(txService->getEvse(evseId)->getTransaction()->idToken.get(), "mIdToken") );
        }
    }

    mocpp_deinitialize();
}

#endif //MO_ENABLE_V201
