// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Core/Operation.h>

#include <MicroOcpp/Model/FirmwareManagement/FirmwareStatus.h>

#ifndef MO_FIRMWARESTATUSNOTIFICATION_H
#define MO_FIRMWARESTATUSNOTIFICATION_H

namespace MicroOcpp {
namespace Ocpp16 {

class FirmwareStatusNotification : public Operation, public MemoryManaged {
private:
    FirmwareStatus status = FirmwareStatus::Idle;
    int requestId = -1;
    static const char *cstrFromFwStatus(FirmwareStatus status);
public:
    /**
     * @param requestId OCPP 2.0.1: requestId of the UpdateFirmwareRequest that started the update.
     *                  Negative omits the field (OCPP 1.6, or no update has been requested)
     */
    FirmwareStatusNotification(FirmwareStatus status, int requestId = -1);

    const char* getOperationType() override {return "FirmwareStatusNotification"; }

    std::unique_ptr<JsonDoc> createReq() override;

    void processConf(JsonObject payload) override;

};

} //end namespace Ocpp16
} //end namespace MicroOcpp

#endif
