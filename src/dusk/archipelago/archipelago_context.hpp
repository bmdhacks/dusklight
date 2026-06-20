#pragma once

#include <mutex>
#include <string>

#include "Archipelago.h"

namespace dusk::archi
{
    class ArchipelagoContext {
    private:
        struct TEMP_GameItemInfo {
            int itemId = -1;
            randomizer::logic::item::Importance importance = randomizer::logic::item::Importance::INVALID;
            std::string itemName;
        };

        struct TEMP_GameLocationInfo {
            int apId = -1;
            std::string locName;
        };

        struct GameLocationInfo {
            int itemId = -1;
            std::string itemName;
            std::string locationName;
            int64_t apLocationId = -1;
            bool collected = false;
        };

        std::vector<std::pair<int, bool>> m_receivedItemsQueue;
        std::mutex m_queueMutex;

        // Rando Data
        randomizer::seedgen::config::Config m_config;
        std::unique_ptr<randomizer::logic::world::World> m_archiWorld = nullptr;
        bool m_isUpdateLocations = false;
        bool m_isNeedResetInv = false;
        bool m_isAllowUpdateLocations = false;

        // AP Data
        std::unordered_map<std::string, GameLocationInfo> m_locationItemInfo;
        std::map<int, bool> m_initLocationCollectState;
        AP_RoomInfo m_roomInfo;

        // TEMP
        std::map<int, TEMP_GameItemInfo> m_apItemToGameItem;
        std::vector<TEMP_GameLocationInfo> m_apLocToGameLoc;
        std::string m_apConfigPath;

        void LoadTempItemInfo();

        void LoadTempLocationInfo();

        void itemRecvImpl(int id, bool notify);

        int getItemIdFromApId(int apId);

        std::string getLocationNameFromApId(int apId) const;
    public:
        ArchipelagoContext();

        // Config Getters/Setters

        static void SetServerIp(const std::string_view& ip);
        static void SetSlotName(const std::string_view& name);
        static void SetPassword(const std::string_view& pass);

        static const std::string& GetServerIp();
        static const std::string& GetSlotName();
        static const std::string& GetPassword();

        static std::string GetArchipelagoSeedName();

        static void GetSeedDirectoryPath(std::filesystem::path& outPath);

        // Connection Handlers

        static void ConnectToServer();

        static void DisconnectFromServer();

        static bool IsConnected();

        // State Handlers

        static void MessageThreadFunc();

        static void Execute();

        static void HandleItemReceived(AP_NetworkItem& id, bool notify);

        static void HandleResetInventory();

        static void HandleReceiveLocationScout(const std::vector<AP_NetworkItem>& items);

        static void UpdateCheckedLocations();

        static void SetNeedUpdateLocations(bool update);

        static bool IsLocationChecked(int locId);

        static void SetLocationChecked(int locId, bool collected);

        static void UpdateLocationState(int locId, bool collected);

        static void UpdateAllLocationState();

        // State Requesters

        static void RequestAllLocationScout(bool isHint = false);

        // AP -> Internal Rando Converters

        static void SetAPConfigYamlPath(const std::string_view& path);

        static bool GenerateConfigFromAP(randomizer::seedgen::config::Config& config);

        static int GetItemAtLocation(const std::string& locName);

        static int GetItemAtLocation(int locId);

        static void CreateArchipelagoWorld();

        static void FillArchipelagoWorld();

        static void CreateRandomizerContext();

        static void LoadRandomizerContext();

        static void GenerateLocalWorldData();

    };
} // dusk::archi