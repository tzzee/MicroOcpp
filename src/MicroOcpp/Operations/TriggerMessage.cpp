// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Operations/TriggerMessage.h>
#include <MicroOcpp/Model/ConnectorBase/Connector.h>
#include <MicroOcpp/Model/Metering/MeteringService.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Core/Request.h>
#include <MicroOcpp/Debug.h>
#include <MicroOcpp/Version.h>
#if MO_ENABLE_V201
#include <MicroOcpp/Model/Availability/AvailabilityService.h>
#endif

using MicroOcpp::Ocpp16::TriggerMessage;
using MicroOcpp::JsonDoc;

TriggerMessage::TriggerMessage(Context& context) : MemoryManaged("v16.Operation.", "TriggerMessage"), context(context) {

}

const char* TriggerMessage::getOperationType(){
    return "TriggerMessage";
}

void TriggerMessage::processReq(JsonObject payload) {

    const char *requestedMessage = payload["requestedMessage"] | "Invalid";
    int connectorId = payload["connectorId"] | -1;
#if MO_ENABLE_V201
    // OCPP 2.0.1 addresses the target with "evse": {"id", "connectorId"} instead of "connectorId" (1.6).
    // Reading only "connectorId" made every StatusNotification trigger fall back to -1 and get Rejected
    // (no 1.6 connectors exist in 2.0.1 mode).
    const bool v201 = context.getVersion().major == 2;
    if (v201) {
        connectorId = payload["evse"]["id"] | -1;
    }
#endif

    MO_DBG_INFO("Execute for message type %s, connectorId = %i", requestedMessage, connectorId);

    statusMessage = "Rejected";

    if (!strcmp(requestedMessage, "MeterValues")) {
        if (auto mService = context.getModel().getMeteringService()) {
            if (connectorId < 0) {
                auto nConnectors = mService->getNumConnectors();
                for (decltype(nConnectors) cId = 0; cId < nConnectors; cId++) {
                    if (auto meterValues = mService->takeTriggeredMeterValues(cId)) {
                        context.getRequestQueue().sendRequestPreBoot(std::move(meterValues));
                        statusMessage = "Accepted";
                    }
                }
            } else if (connectorId < mService->getNumConnectors()) {
                if (auto meterValues = mService->takeTriggeredMeterValues(connectorId)) {
                    context.getRequestQueue().sendRequestPreBoot(std::move(meterValues));
                    statusMessage = "Accepted";
                }
            } else {
                errorCode = "PropertyConstraintViolation";
            }
        }
    } else if (!strcmp(requestedMessage, "StatusNotification")
#if MO_ENABLE_V201
            && !v201
#endif
            ) {
        unsigned int cIdRangeBegin = 0, cIdRangeEnd = 0;
        if (connectorId < 0) {
            cIdRangeEnd = context.getModel().getNumConnectors();
        } else if ((unsigned int) connectorId < context.getModel().getNumConnectors()) {
            cIdRangeBegin = connectorId;
            cIdRangeEnd = connectorId + 1;
        } else {
            errorCode = "PropertyConstraintViolation";
        }

        for (auto i = cIdRangeBegin; i < cIdRangeEnd; i++) {
            auto connector = context.getModel().getConnector(i);
            if (connector->triggerStatusNotification()) {
                statusMessage = "Accepted";
            }
        }
    }
#if MO_ENABLE_V201
    else if (!strcmp(requestedMessage, "StatusNotification") && v201) {
        // 2.0.1: StatusNotification is sent per EVSE by the AvailabilityService. Without "evse", trigger all EVSEs (>= 1)
        auto availabilityService = context.getModel().getAvailabilityService();
        if (availabilityService) {
            if (connectorId < 0) {
                for (unsigned int id = 1; id < MO_NUM_EVSEID; id++) {
                    auto evse = availabilityService->getEvse(id);
                    if (evse && evse->triggerStatusNotification()) {
                        statusMessage = "Accepted";
                    }
                }
            } else if (connectorId >= 1 && connectorId < MO_NUM_EVSEID) {
                auto evse = availabilityService->getEvse((unsigned int)connectorId);
                if (evse && evse->triggerStatusNotification()) {
                    statusMessage = "Accepted";
                }
            } else {
                errorCode = "PropertyConstraintViolation";
            }
        }
    }
#endif
    else {
        auto msg = context.getOperationRegistry().deserializeOperation(requestedMessage);
        if (msg) {
            context.getRequestQueue().sendRequestPreBoot(std::move(msg));
            statusMessage = "Accepted";
        } else {
            statusMessage = "NotImplemented";
        }
    }
}

std::unique_ptr<JsonDoc> TriggerMessage::createConf(){
    auto doc = makeJsonDoc(getMemoryTag(), JSON_OBJECT_SIZE(1));
    JsonObject payload = doc->to<JsonObject>();
    payload["status"] = statusMessage;
    return doc;
}
