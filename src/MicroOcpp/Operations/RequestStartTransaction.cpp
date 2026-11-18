// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Version.h>

#if MO_ENABLE_V201

#include <MicroOcpp/Operations/RequestStartTransaction.h>
#include <MicroOcpp/Model/RemoteControl/RemoteControlService.h>
#include <MicroOcpp/Debug.h>

using MicroOcpp::Ocpp201::RequestStartTransaction;
using MicroOcpp::JsonDoc;

RequestStartTransaction::RequestStartTransaction(RemoteControlService& rcService) : MemoryManaged("v201.Operation.", "RequestStartTransaction"), rcService(rcService) {
  
}

const char* RequestStartTransaction::getOperationType(){
    return "RequestStartTransaction";
}

void RequestStartTransaction::processReq(JsonObject payload) {

    // evseId は省けて、省けば充電器が選ぶ(0 として渡す)。書いてあれば 1 以上(スキーマの説明「EvseId SHALL be > 0」)。
    // 0・負・無い EVSE は形の誤りではなく受け付けられない要求なので、CALLERROR でなく Rejected で返す
    // (GRIDPORT 1.6.10。テラチャージの評価シート TM-EVS-002・003・004)
    int evseId = 0;
    if (payload.containsKey("evseId")) {
        evseId = payload["evseId"] | -1;
        if (evseId <= 0 || evseId >= MO_NUM_EVSEID) {
            MO_DBG_INFO("reject remote start for evseId %d", evseId);
            status = RequestStartStopStatus_Rejected;
            return;
        }
    }

    int remoteStartId = payload["remoteStartId"] | 0;
    if (remoteStartId < 0) {
        errorCode = "PropertyConstraintViolation";
        MO_DBG_ERR("IDs must be >= 0");
        return;
    }

    IdToken idToken;
    if (!idToken.parseCstr(payload["idToken"]["idToken"] | (const char*)nullptr, payload["idToken"]["type"] | (const char*)nullptr)) { //parseCstr rejects nullptr as argument
        MO_DBG_ERR("could not parse idToken");
        errorCode = "FormationViolation";
        return;
    }

    // 空の idToken を持てるのは NoAuthorization だけ(IdTokenEnumType の説明)。ほかの型の空は誰とも照合できないので
    // Rejected で返す(GRIDPORT 1.6.10。TM-IDT-001 で Central の空を Accepted にしていた)
    if (idToken.get()[0] == '\0' && strcmp(idToken.getTypeCstr(), "NoAuthorization")) {
        MO_DBG_INFO("reject remote start with empty idToken of type %s", idToken.getTypeCstr());
        status = RequestStartStopStatus_Rejected;
        return;
    }

    status = rcService.requestStartTransaction(evseId, remoteStartId, idToken, transactionId, sizeof(transactionId));
}

std::unique_ptr<JsonDoc> RequestStartTransaction::createConf(){

    auto doc = makeJsonDoc(getMemoryTag(), JSON_OBJECT_SIZE(2));
    JsonObject payload = doc->to<JsonObject>();

    const char *statusCstr = "";

    switch (status) {
        case RequestStartStopStatus_Accepted:
            statusCstr = "Accepted";
            break;
        case RequestStartStopStatus_Rejected:
            statusCstr = "Rejected";
            break;
        default:
            MO_DBG_ERR("internal error");
            break;
    }

    payload["status"] = statusCstr;

    // F01.FR.13: 認可の前の取引がある(先に挿した)ときは、その transactionId を応答に載せる。requestStartTransaction() は
    // transactionId に写すが、ここは使われていない transaction を見ていて一度も載せていなかった(GRIDPORT 1.6.10。TM-CMB-004)。
    // 受け付けたときだけ載せる(ほかの人の取引で断ったときに、その取引の ID を返さない)
    if (status == RequestStartStopStatus_Accepted && transactionId[0] != '\0') {
        payload["transactionId"] = (const char*)transactionId; //force zero-copy mode
    }

    return doc;
}

#endif // MO_ENABLE_V201
