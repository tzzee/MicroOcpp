// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp.h>
#include <MicroOcpp/Core/Connection.h>
#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Core/Request.h>
#include <MicroOcpp/Core/RequestQueue.h>
#include <MicroOcpp/Core/FilesystemUtils.h>
#include <MicroOcpp/Operations/CustomOperation.h>
#include <MicroOcpp/Platform.h>
#include <MicroOcpp/Debug.h>
#include <catch2/catch.hpp>
#include "./helpers/testHelper.h"

#include <string>
#include <vector>

#define CHARGEPOINTMODEL "Test model"
#define CHARGEPOINTVENDOR "Test vendor"

using namespace MicroOcpp;

namespace {

//message type of a serialized OCPP-J message, i.e. the first element of the RPC framework array
int messageType(const std::string& message) {
    size_t i = message.find('[');
    if (i == std::string::npos) {
        return -1;
    }
    i++;
    if (i >= message.length() || message[i] < '0' || message[i] > '9') {
        return -1;
    }
    return message[i] - '0';
}

//the messageID of a serialized OCPP-J message, i.e. the second element of the array
std::string messageId(const std::string& message) {
    size_t open = message.find('"');
    if (open == std::string::npos) {
        return "";
    }
    size_t close = message.find('"', open + 1);
    if (close == std::string::npos) {
        return "";
    }
    return message.substr(open + 1, close - open - 1);
}

//the messages of one operation, taken out of everything the charging station has sent
std::vector<std::string> selectOperation(const std::vector<std::string>& messages, const char *operationType) {
    std::vector<std::string> result;
    std::string needle = std::string("\"") + operationType + "\"";
    for (const auto& message : messages) {
        if (message.find(needle) != std::string::npos) {
            result.push_back(message);
        }
    }
    return result;
}

} //end anonymous namespace

/*
 * Loopback connection which can lose messages. Holding back the responses models a request
 * which the server has processed, but whose response does not make it back. Holding back the
 * requests models a request which the server never sees.
 */
class LossyConnection : public Connection, public MemoryManaged {
private:
    ReceiveTXTcallback receiveTXT;
    bool holdRequests = false;
    bool holdResponses = false;
    std::vector<std::string> sentRequests;
    std::vector<std::string> heldResponses;
    unsigned long lastRecv = 0;
    unsigned long lastConn = 0;
public:
    LossyConnection() : MemoryManaged("LossyConnection") { }
    void loop() override { }

    bool sendTXT(const char *msg, size_t length) override {
        std::string message (msg, length);

        if (messageType(message) == MESSAGE_TYPE_CALL) {
            sentRequests.push_back(message);
            if (holdRequests) {
                return true; //taken by the transport, but the server never sees it
            }
        } else if (holdResponses) {
            heldResponses.push_back(message); //the server has answered, but the response is lost
            return true;
        }

        return deliver(message);
    }

    //pass a message to the charging station, no matter what is held back at the moment
    bool deliver(const std::string& message) {
        if (!receiveTXT) {
            return false;
        }
        lastRecv = mocpp_tick_ms();
        return receiveTXT(message.c_str(), message.length());
    }

    void setReceiveTXTcallback(ReceiveTXTcallback& receiveTXT) override {this->receiveTXT = receiveTXT;}
    unsigned long getLastRecv() override {return lastRecv;}
    unsigned long getLastConnected() override {return lastConn;}
    bool isConnected() override {return true;}

    void setHoldRequests(bool holdRequests) {this->holdRequests = holdRequests;}
    void setHoldResponses(bool holdResponses) {this->holdResponses = holdResponses;}
    const std::vector<std::string>& getSentRequests() {return sentRequests;}
    const std::vector<std::string>& getHeldResponses() {return heldResponses;}
};

namespace {

std::unique_ptr<Request> makeTestRequest(int& createReqCount, int& responseCount) {
    return makeRequest(new Ocpp16::CustomOperation(
            "DataTransfer",
            [&createReqCount] () {
                createReqCount++;
                auto doc = makeJsonDoc(UNIT_MEM_TAG, JSON_OBJECT_SIZE(2));
                auto payload = doc->to<JsonObject>();
                payload["vendorId"] = "UnitTests";
                payload["messageId"] = createReqCount;
                return doc;},
            [&responseCount] (JsonObject) {
                responseCount++;}));
}

} //end anonymous namespace

TEST_CASE( "RequestQueue" ) {
    printf("\nRun %s\n",  "RequestQueue");

    //clean state
    auto filesystem = makeDefaultFilesystemAdapter(FilesystemOpt::Use_Mount_FormatOnFail);
    FilesystemUtils::remove_if(filesystem, [] (const char*) {return true;});

    SECTION("a response which does not belong to the request in flight is ignored") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        int createReqCount = 0;
        int responseCount = 0;

        connection.setHoldResponses(true);
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(request));
        loop();

        REQUIRE( connection.getHeldResponses().size() == 1 );
        std::string response = connection.getHeldResponses().front();

        //a response of another operation arrives while the request is still in flight
        connection.deliver("[3,\"6f2c8b91-0d3a-4e57-9a1c-2b8e4f0d7c35\",{}]");
        loop();

        REQUIRE( responseCount == 0 );

        //the request in flight is untouched, so it can still be completed
        connection.deliver(response);
        loop();

        REQUIRE( responseCount == 1 );

        mocpp_deinitialize();
    }

    SECTION("a CALLERROR takes the request in flight out of the queue") {

        /*
         * An Operation may handle a CALLERROR without aborting, like StopTransaction which
         * accepts the loss and returns false. That request is finished all the same and must
         * not keep the queue.
         */

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        beginTransaction("mIdTag");
        loop();
        REQUIRE( isTransactionRunning() );

        //keep the server from answering, so the StopTransaction can be answered by hand
        connection.setHoldRequests(true);
        endTransaction();
        loop();

        auto stopTx = selectOperation(connection.getSentRequests(), "StopTransaction");
        REQUIRE( stopTx.size() == 1 );

        connection.setHoldRequests(false);
        connection.deliver("[4,\"" + messageId(stopTx.front()) + "\",\"InternalError\",\"\",{}]");
        loop();

        //the queue moved on: the next request is sent and answered
        int createReqCount = 0;
        int responseCount = 0;
        auto next = makeTestRequest(createReqCount, responseCount);
        next->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(next));
        loop();

        REQUIRE( responseCount == 1 );

        mocpp_deinitialize();
    }

}
