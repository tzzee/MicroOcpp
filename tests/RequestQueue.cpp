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
    bool refuseSend = false;
    std::vector<std::string> sentRequests;
    std::vector<std::string> heldResponses;
    unsigned long lastRecv = 0;
    unsigned long lastConn = 0;
public:
    LossyConnection() : MemoryManaged("LossyConnection") { }
    void loop() override { }

    bool sendTXT(const char *msg, size_t length) override {
        std::string message (msg, length);

        if (refuseSend) {
            return false; //the transport does not take the message
        }

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

    void setRefuseSend(bool refuseSend) {this->refuseSend = refuseSend;}
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

#if MO_ENABLE_HEAP_PROFILER
//bytes which the heap profiler currently accounts to one memory tag
size_t heapInUse(const char *tag) {
    std::vector<char> buf (1 << 17);
    if (mo_mem_write_stats_json(buf.data(), buf.size()) < 0) {
        return 0;
    }
    DynamicJsonDocument doc {1 << 18};
    if (deserializeJson(doc, buf.data())) {
        return 0;
    }
    for (JsonObject entry : doc["by_tag"].as<JsonArray>()) {
        if (!strcmp(entry["tag"] | "", tag)) {
            return entry["current"] | (size_t)0;
        }
    }
    return 0;
}
#endif

std::unique_ptr<Request> makeLargeRequest(int& responseCount) {
    return makeRequest(new Ocpp16::CustomOperation(
            "DataTransfer",
            [] () {
                std::string data (4000, 'x');
                auto doc = makeJsonDoc(UNIT_MEM_TAG, JSON_OBJECT_SIZE(2) + data.length() + 1);
                auto payload = doc->to<JsonObject>();
                payload["vendorId"] = "UnitTests";
                payload["data"] = data;
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

    SECTION("a request is sent again unchanged when its response is lost") {

        //the payload of makeTestRequest changes from one call of createReq() to the next

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        int createReqCount = 0;
        int responseCount = 0;

        connection.setHoldResponses(true);
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(0); //never give up on this one
        getOcppContext()->initiateRequest(std::move(request));
        loop();

        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 1 );
        REQUIRE( connection.getHeldResponses().size() == 1 ); //the server has answered
        REQUIRE( responseCount == 0 );                        //but the charging station has not seen it

        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();

        auto attempts = selectOperation(connection.getSentRequests(), "DataTransfer");
        REQUIRE( attempts.size() == 2 );
        REQUIRE( attempts[0] == attempts[1] ); //same messageID and same payload
        REQUIRE( createReqCount == 1 );        //the payload has been created once

        mocpp_deinitialize();
    }

    SECTION("the response of a request which was sent twice is processed once") {

        //the other response may arrive while the next request is already in flight

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
        std::string lostResponse = connection.getHeldResponses().front();

        //the request is sent again and this time the response comes through
        connection.setHoldResponses(false);
        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();

        REQUIRE( responseCount == 1 );

        //the next request is in flight, its response is held back
        int secondCreateReqCount = 0;
        int secondResponseCount = 0;

        connection.setHoldResponses(true);
        auto second = makeTestRequest(secondCreateReqCount, secondResponseCount);
        second->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(second));
        loop();

        REQUIRE( secondCreateReqCount == 1 );
        REQUIRE( connection.getHeldResponses().size() == 2 );
        std::string secondResponse = connection.getHeldResponses().back();

        //the response which got lost at the beginning arrives now
        connection.deliver(lostResponse);
        loop();

        REQUIRE( responseCount == 1 );       //the first request is not completed a second time
        REQUIRE( secondResponseCount == 0 ); //and the request in flight is left alone

        connection.deliver(secondResponse);
        loop();

        REQUIRE( secondResponseCount == 1 );

        mocpp_deinitialize();
    }

    SECTION("a lost response does not block the queue") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        int firstCreateReqCount = 0, firstResponseCount = 0;
        int secondCreateReqCount = 0, secondResponseCount = 0;

        //the first request goes out, but the server never sees it
        connection.setHoldRequests(true);
        auto first = makeTestRequest(firstCreateReqCount, firstResponseCount);
        first->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(first));
        loop();
        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 1 );

        //the second request must wait: only one request may be in flight
        auto second = makeTestRequest(secondCreateReqCount, secondResponseCount);
        second->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(second));
        loop();
        REQUIRE( secondCreateReqCount == 0 );

        //after the response timeout the first request is sent again; now it gets through
        connection.setHoldRequests(false);
        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();
        REQUIRE( firstResponseCount == 1 );

        //and the queue keeps moving
        REQUIRE( secondResponseCount == 1 );

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

        //and the StopTransaction is not sent over and over again
        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();
        REQUIRE( selectOperation(connection.getSentRequests(), "StopTransaction").size() == 1 );

        mocpp_deinitialize();
    }

    SECTION("a request without a deadline survives an outage") {

        //setTimeout(0) means "do not give up on this request"

        LoopbackConnection loopback;
        mocpp_initialize(loopback, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        loopback.setConnected(false);

        int createReqCount = 0;
        int responseCount = 0;
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(request));

        //stay offline well beyond the response timeout
        for (int i = 0; i < 4; i++) {
            mtime += MO_REQUEST_RESPONSE_TIMEOUT;
            loop();
        }

        REQUIRE( createReqCount == 0 );

        loopback.setConnected(true);
        loop();

        REQUIRE( createReqCount == 1 );
        REQUIRE( responseCount == 1 );

        mocpp_deinitialize();
    }

    SECTION("a request with a deadline is still given up") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        connection.setHoldResponses(true);

        bool aborted = false;
        int createReqCount = 0, responseCount = 0;
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(10000);
        request->setOnAbortListener([&aborted] () {aborted = true;});
        getOcppContext()->initiateRequest(std::move(request));

        loop();
        mtime += 11000;
        loop();

        REQUIRE( aborted );

        mocpp_deinitialize();
    }

    SECTION("sending a request again does not extend its deadline") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        connection.setHoldResponses(true);

        bool aborted = false;
        int createReqCount = 0, responseCount = 0;
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(3 * MO_REQUEST_RESPONSE_TIMEOUT);
        request->setOnAbortListener([&aborted] () {aborted = true;});
        getOcppContext()->initiateRequest(std::move(request));
        loop();

        //the request goes out again twice while its deadline has not passed yet
        for (int i = 1; i <= 2; i++) {
            mtime += MO_REQUEST_RESPONSE_TIMEOUT;
            loop();
            REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == (size_t)i + 1 );
            REQUIRE( !aborted );
        }

        //the deadline is measured from the first attempt, so it passes as before
        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();

        REQUIRE( aborted );
        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 3 );

        mocpp_deinitialize();
    }

    SECTION("a request the connection refuses is sent again later") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        connection.setHoldResponses(true);

        int createReqCount = 0, responseCount = 0;
        auto request = makeTestRequest(createReqCount, responseCount);
        request->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(request));
        loop();

        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 1 );

        //the connection does not take the message
        connection.setRefuseSend(true);
        mtime += MO_REQUEST_RESPONSE_TIMEOUT;
        loop();

        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 1 );

        //as soon as it does, the request goes out without waiting another interval
        connection.setRefuseSend(false);
        connection.setHoldResponses(false);
        loop();

        REQUIRE( selectOperation(connection.getSentRequests(), "DataTransfer").size() == 2 );
        REQUIRE( responseCount == 1 );

        mocpp_deinitialize();
    }

#if MO_ENABLE_HEAP_PROFILER
    SECTION("the message of a request is released when the request is answered") {

        LossyConnection connection;
        mocpp_initialize(connection, ChargerCredentials(CHARGEPOINTMODEL, CHARGEPOINTVENDOR), filesystem);
        mocpp_set_timer(custom_timer_cb);
        loop();

        const size_t idle = heapInUse("RequestQueue");

        connection.setHoldResponses(true);
        int responseCount = 0;
        auto request = makeLargeRequest(responseCount);
        request->setTimeout(0);
        getOcppContext()->initiateRequest(std::move(request));
        loop();

        //the message is kept for as long as the request may have to be sent again
        REQUIRE( heapInUse("RequestQueue") >= idle + 4000 );

        REQUIRE( connection.getHeldResponses().size() == 1 );
        connection.deliver(connection.getHeldResponses().front());
        loop();

        REQUIRE( responseCount == 1 );
        REQUIRE( heapInUse("RequestQueue") <= idle );

        mocpp_deinitialize();
    }
#endif
}
