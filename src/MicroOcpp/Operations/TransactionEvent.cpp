// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Version.h>

#if MO_ENABLE_V201

#include <MicroOcpp/Operations/TransactionEvent.h>

#include <string.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Model/Transactions/Transaction.h>
#include <MicroOcpp/Debug.h>

using namespace MicroOcpp::Ocpp201;
using MicroOcpp::JsonDoc;

TransactionEvent::TransactionEvent(Model& model, TransactionEventData *txEvent)
        : MemoryManaged("v201.Operation.", "TransactionEvent"), model(model), txEvent(txEvent) {

}

const char* TransactionEvent::getOperationType() {
    return "TransactionEvent";
}

std::unique_ptr<JsonDoc> TransactionEvent::createReq() {

    size_t capacity = 0;

    if (txEvent->eventType == TransactionEventData::Type::Ended) {
        for (size_t i = 0; i < txEvent->transaction->sampledDataTxEnded.size(); i++) {
            JsonDoc meterValueJson = initJsonDoc(getMemoryTag()); //just measure, create again for serialization later
            txEvent->transaction->sampledDataTxEnded[i]->toJson(meterValueJson);
            capacity += meterValueJson.capacity();
        }
    }

    for (size_t i = 0; i < txEvent->meterValue.size(); i++) {
        JsonDoc meterValueJson = initJsonDoc(getMemoryTag()); //just measure, create again for serialization later
        txEvent->meterValue[i]->toJson(meterValueJson);
        capacity += meterValueJson.capacity();
    }

    // NOEX: 記録から読み戻した積算値(再起動・読み直しのあと。2026-10-24)
    if (txEvent->eventType == TransactionEventData::Type::Ended && txEvent->transaction->sampledDataTxEndedStored) {
        capacity += txEvent->transaction->sampledDataTxEndedStored->memoryUsage() + JSON_ARRAY_SIZE(1);
    }
    if (txEvent->meterValueStored) {
        capacity += txEvent->meterValueStored->memoryUsage() + JSON_ARRAY_SIZE(1);
    }

    // NOEX: 時計が合う前のメーター値の時刻を、下で書き直す分(書き直すと文字列を新しく持つ)
    {
        size_t mvCount = txEvent->meterValue.size();
        if (txEvent->eventType == TransactionEventData::Type::Ended) {
            mvCount += txEvent->transaction->sampledDataTxEnded.size();
            if (txEvent->transaction->sampledDataTxEndedStored) {
                mvCount += txEvent->transaction->sampledDataTxEndedStored->as<JsonArrayConst>().size();
            }
        }
        if (txEvent->meterValueStored) {
            mvCount += txEvent->meterValueStored->as<JsonArrayConst>().size();
        }
        capacity += mvCount * (JSONDATE_LENGTH + 1);
    }

    capacity +=
            JSON_OBJECT_SIZE(12) + //total of 12 fields
            JSONDATE_LENGTH + 1 + //timestamp string
            JSON_OBJECT_SIZE(5) + //transactionInfo
                MO_TXID_LEN_MAX + 1 + //transactionId
            MO_IDTOKEN_LEN_MAX + 1; //idToken

    auto doc = makeJsonDoc(getMemoryTag(), capacity);
    JsonObject payload = doc->to<JsonObject>();

    payload["eventType"] = serializeTransactionEventType(txEvent->eventType);

    const bool clockSet = model.getClock().now() >= MIN_TIME;
    const uint32_t powerCycle = mo_getPowerCycleId();
    // NOEX: 同じ電源の間に作ったできごとか。製品が OCPP を作り直すと bootNr は変わるが、電源の番号は変わらない。
    // 同じ電源の間なら、時計が合う前の時刻(起動からの経過秒)を本当の時刻に直せる(2026-11-15)
    const bool eventSamePowerCycle = txEvent->bootNr == model.getBootNr() ||
            (txEvent->powerCycle != 0 && txEvent->powerCycle == powerCycle && txEvent->timestamp > Timestamp());

    if (txEvent->timestamp < MIN_TIME &&
            eventSamePowerCycle &&
            clockSet) {
        //same as v16 StartTransaction: an event created before the clock was set in this power cycle gets its time now
        MO_DBG_DEBUG("adjust preboot TransactionEvent timestamp");
        txEvent->timestamp = model.getClock().adjustPrebootTimestamp(txEvent->timestamp);
    }

    // NOEX: 前の起動で時刻が合う前に作ったできごとは時刻が分からない。捨てずに今の時刻で送る(2026-10-24)
    if (txEvent->timestamp < MIN_TIME && model.getClock().now() >= MIN_TIME) {
        MO_DBG_WARN("TransactionEvent timestamp unknown (previous power cycle), use current time seqNo=%u", txEvent->seqNo);
        txEvent->timestamp = model.getClock().now();
    }

    char timestamp [JSONDATE_LENGTH + 1];
    txEvent->timestamp.toJsonString(timestamp, JSONDATE_LENGTH + 1);
    payload["timestamp"] = timestamp;

    if (serializeTransactionEventTriggerReason(txEvent->triggerReason)) {
        payload["triggerReason"] = serializeTransactionEventTriggerReason(txEvent->triggerReason);
    } else {
        MO_DBG_ERR("serialization error");
    }

    payload["seqNo"] = txEvent->seqNo;

    if (txEvent->offline) {
        payload["offline"] = txEvent->offline;
    }

    if (txEvent->numberOfPhasesUsed >= 0) {
        payload["numberOfPhasesUsed"] = txEvent->numberOfPhasesUsed;
    }

    if (txEvent->cableMaxCurrent >= 0) {
        payload["cableMaxCurrent"] = txEvent->cableMaxCurrent;
    }

    if (txEvent->reservationId >= 0) {
        payload["reservationId"] = txEvent->reservationId;
    }

    JsonObject transactionInfo = payload.createNestedObject("transactionInfo");
    transactionInfo["transactionId"] = txEvent->transaction->transactionId;

    if (serializeTransactionEventChargingState(txEvent->chargingState)) { // optional
        transactionInfo["chargingState"] = serializeTransactionEventChargingState(txEvent->chargingState);
    }

    if (txEvent->eventType == TransactionEventData::Type::Ended && //Updated events sent after the stop must not carry it
            txEvent->transaction->stoppedReason != Transaction::StoppedReason::Local &&
            serializeTransactionStoppedReason(txEvent->transaction->stoppedReason)) { // optional
        transactionInfo["stoppedReason"] = serializeTransactionStoppedReason(txEvent->transaction->stoppedReason);
    }

    if (txEvent->remoteStartId >= 0) {
        transactionInfo["remoteStartId"] = txEvent->transaction->remoteStartId;
    }

    if (txEvent->idToken) {
        JsonObject idToken = payload.createNestedObject("idToken");
        idToken["idToken"] = txEvent->idToken->get();
        idToken["type"] = txEvent->idToken->getTypeCstr();
    }

    if (txEvent->evse.id >= 0) {
        JsonObject evse = payload.createNestedObject("evse");
        evse["id"] = txEvent->evse.id;
        if (txEvent->evse.connectorId >= 0) {
            evse["connectorId"] = txEvent->evse.connectorId;
        }
    }

    if (txEvent->eventType == TransactionEventData::Type::Ended) {
        // NOEX: 記録から読み戻した分(再起動の前の Transaction.Begin など)を先に載せる
        if (txEvent->transaction->sampledDataTxEndedStored) {
            for (JsonVariantConst mv : txEvent->transaction->sampledDataTxEndedStored->as<JsonArrayConst>()) {
                payload["meterValue"].add(mv);
            }
        }
        for (size_t i = 0; i < txEvent->transaction->sampledDataTxEnded.size(); i++) {
            JsonDoc meterValueJson = initJsonDoc(getMemoryTag());
            txEvent->transaction->sampledDataTxEnded[i]->toJson(meterValueJson);
            payload["meterValue"].add(meterValueJson);
        }
    }

    if (txEvent->meterValueStored) {
        for (JsonVariantConst mv : txEvent->meterValueStored->as<JsonArrayConst>()) {
            payload["meterValue"].add(mv);
        }
    }

    for (size_t i = 0; i < txEvent->meterValue.size(); i++) {
        JsonDoc meterValueJson = initJsonDoc(getMemoryTag());
        txEvent->meterValue[i]->toJson(meterValueJson);
        payload["meterValue"].add(meterValueJson);
    }

    // NOEX: 時計が合う前に取ったメーター値の時刻(1970 年 + 起動からの経過秒)も、できごとと同じく直す。直さずに送ると、
    // CSMS に 1970 年の時刻のメーター値が届いていた(LOCAL から戻した取引・停電で起き直した取引。2026-11-15)。
    // - 同じ電源の間に取った値は adjustPrebootTimestamp で本当の時刻に直す。Transaction.Begin の値は取引を始めた電源、
    //   ほかの値はこのできごとを作った電源で見る
    // - 前の電源で取った値で時刻が分からないものは、このできごとと同じ時に取った値(Transaction.Begin 以外)なら
    //   このできごとの時刻にそろえる。Transaction.Begin の値は分からないまま(1970 年)残す(もっともらしい違う時刻にしない)
    if (clockSet && payload.containsKey("meterValue")) {
        const bool txBeginSamePowerCycle = txEvent->transaction->beginPowerCycle != 0 &&
                txEvent->transaction->beginPowerCycle == powerCycle;
        const int uptimeNow = (int) (mocpp_tick_ms() / 1000UL);
        for (JsonObject mv : payload["meterValue"].as<JsonArray>()) {
            Timestamp t;
            if (!t.setTime(mv["timestamp"] | "_Undefined") || t >= MIN_TIME) {
                continue;
            }
            const char *readingContext = mv["sampledValue"][0]["context"] | "";
            const bool beginValue = !strcmp(readingContext, "Transaction.Begin");
            const bool samePowerCycle = (beginValue ? txBeginSamePowerCycle : eventSamePowerCycle) &&
                    t > Timestamp() && (t - Timestamp()) <= uptimeNow;
            Timestamp fixed;
            if (samePowerCycle) {
                fixed = model.getClock().adjustPrebootTimestamp(t);
            } else if (!beginValue && txEvent->timestamp >= MIN_TIME) {
                fixed = txEvent->timestamp;
            } else {
                continue;
            }
            char timestampStr [JSONDATE_LENGTH + 1];
            fixed.toJsonString(timestampStr, sizeof(timestampStr));
            if (!mv["timestamp"].set(timestampStr)) {
                MO_DBG_ERR("could not correct meterValue timestamp (OOM)");
            }
        }
    }

    return doc;
}

void TransactionEvent::processConf(JsonObject payload) {

    if (payload.containsKey("idTokenInfo")) {
        if (strcmp(payload["idTokenInfo"]["status"], "Accepted")) {
            MO_DBG_INFO("transaction deAuthorized");
            if (txEvent->transaction->active) {
                //the Ended event takes its reasons from here. Without them it is stored with an undefined
                //triggerReason, which cannot be loaded back from flash and the Ended event gets lost
                txEvent->transaction->stopTrigger = TransactionEventTriggerReason::Deauthorized;
                txEvent->transaction->stoppedReason = Transaction::StoppedReason::DeAuthorized;
            }
            txEvent->transaction->active = false;
            txEvent->transaction->isDeauthorized = true;
        }
    }
}

void TransactionEvent::processReq(JsonObject payload) {
    /**
     * Ignore Contents of this Req-message, because this is for debug purposes only
     */
}

std::unique_ptr<JsonDoc> TransactionEvent::createConf() {
    return createEmptyDocument();
}

#endif // MO_ENABLE_V201
