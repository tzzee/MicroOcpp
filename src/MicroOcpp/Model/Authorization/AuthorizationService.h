// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#ifndef MO_AUTHORIZATIONSERVICE_H
#define MO_AUTHORIZATIONSERVICE_H

#include <MicroOcpp/Version.h>

#if MO_ENABLE_LOCAL_AUTH

#include <MicroOcpp/Model/Authorization/AuthorizationList.h>
#include <MicroOcpp/Core/FilesystemAdapter.h>
#include <MicroOcpp/Core/Configuration.h>
#include <MicroOcpp/Core/Memory.h>

namespace MicroOcpp {

class Context;

class AuthorizationService : public MemoryManaged {
private:
    Context& context;
    std::shared_ptr<FilesystemAdapter> filesystem;
    AuthorizationList localAuthorizationList;

    std::shared_ptr<Configuration> localAuthListEnabledBool;

    uint32_t localListUpdateCount = 0; //increased on every (attempted) change of localAuthorizationList

public:
    AuthorizationService(Context& context, std::shared_ptr<FilesystemAdapter> filesystem);
    ~AuthorizationService();

    bool loadLists();

    AuthorizationData *getLocalAuthorization(const char *idTag);

    int getLocalListVersion();
    size_t getLocalListSize(); //number of entries in current localAuthList; used in unit tests

    /**
     * @brief Visits every entry of the local authorization list in idTag order.
     * @details Unlike getLocalAuthorization(), this ignores LocalAuthListEnabled. Lets the host keep its own
     *          copy of the list (e.g. to look up idTags from another task without touching MicroOcpp).
     */
    void forEachLocalAuthorization(std::function<void(const AuthorizationData&)> visitor);

    /**
     * @brief Counter which increases whenever the local authorization list may have changed (SendLocalList of
     *        OCPP 1.6 and of the host's OCPP 2.0.1 operation, any other updateLocalList() call, loading from flash).
     * @details The list version and the size can stay the same over a change (e.g. a full update with the same
     *          listVersion), so compare this counter to detect changes.
     */
    uint32_t getLocalListUpdateCount();

    bool updateLocalList(JsonArray localAuthorizationListJson, int listVersion, bool differential);

    void notifyAuthorization(const char *idTag, JsonObject idTagInfo);
};

}

#endif //MO_ENABLE_LOCAL_AUTH
#endif
