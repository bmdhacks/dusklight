#include <dusk/archipelago/archipelago_context.hpp>

#include <thread>

#include "Archipelago.h"
#include "d/d_item.h"
#include "d/actor/d_a_alink.h"
#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/randomizer/game/tools.h"
#include "dusk/randomizer/game/verify_item_functions.h"
#include "dusk/randomizer/generator/logic/hints.hpp"
#include "dusk/ui/rando_config.hpp"
#include "dusk/ui/ui.hpp"

namespace dusk::archi
{

static constexpr int ARCHI_ITEM_OFFSET = 2320000;

struct SettingsNameConvert {
    static constexpr std::string kDefaultYes = "On";
    static constexpr std::string kDefaultNo = "Off";

    std::string apName;
    std::string dusklightName;
    std::vector<std::pair<std::string, std::string>> optionsConvert;

    const std::string& tryGetOptionConvert(const std::string& option) const {
        if (optionsConvert.empty()) {
            if (option == "Yes")
                return kDefaultYes;
            if (option == "No")
                return kDefaultNo;
            return option;
        }

        for (const auto& value : optionsConvert) {
            if (value.first == option) {
                return value.second;
            }
        }
        return option;
    }
};

static auto sArchiSettingToDusklight = std::to_array<SettingsNameConvert>({
    {"", ""},
    {"Golden Bugs Shuffled", "Golden Bugs"},
    {"Sky Chracters Shuffled", "Sky Characters"},
    {"NPC Items Shuffled", "Gifts From NPCs"},
    {"Shop Items Shuffled", "Shop Items"},
    {"Hidden Skills Shuffled", "Hidden Skills"},
    {"Skip Prologue", "Skip Prologue"},
    {"Faron Twilight Cleared", "Faron Twilight Cleared"},
    {"Eldin Twilight Cleared", "Eldin Twilight Cleared"},
    {"Lanayru Twilight Cleared", "Lanayru Twilight Cleared"},
    {"Skip MDH", "Skip Midna's Desparate Hour"},
    {"Open Map", "Unlock Map Regions"},
    {"Increase Wallet", "Logic Increase Wallet Capacity"},
    {"Transform Anywhere", "Logic Transform Anywhere"},
    {"Bonks do Damage", "Bonks Do Damage"},
    {"Lakebed Entrance Requirements", "Lakebed Does Not Require Water Bombs"},
    {"Arbiters Grounds Entrance Requirements", "Arbiters Does Not Require Bulblin Camp"},
    {"Snowpeak Entrance Requirements", "Snowpeak Does Not Require Reekfish Scent"},
    {"City in the Sky Entrance Requirements", "City Does Not Require Filled Skybook"},
    {"Goron Mines Entrance Requirements", "Goron Mines Entrance"},
    {"Palace of Twilight Requirements", "Palace of Twilight Requirements"},
    {"Faron Woods Logic", "Faron Woods Logic"},
{"Starting ToD", "Starting Time of Day"},
   {"Skip Major Cutscenes", "Skip Major Cutscenes"},
{"Skip Minor Cutscenes", "Skip Minor Cutscenes"},
   {"Open Door of Time", "Open Door of Time"},

    {"Dungeon Rewards Progression", "Dungeon Rewards Can Be Anywhere", {
         // these two are functionally identical in terms of tracker logic, so treat it as such
         {"Anything", "On"},
         {"Any Progressive", "On"},
         {"Vanilla", "Off"},
     }},
    {"Small Key Settings", "Small Keys", {
         {"Startwith", "Keysy"},
     }},
    {"Big Key Settings", "Big Keys", {
         {"Startwith", "Keysy"},
     }},
    {"Map and Compass Settings", "Maps and Compasses", {
         {"Startwith", "Start With"},
     }},
    {"Trap Frequency", "Trap Item Frequency", {
         {"No Traps", "None"},
     }},
    {"Damage Magnification", "Logic Damage Multiplier", {
         {"Ohko", "OHKO"},
     }},
    {"Logic Settings", "Logic Rules", {
         {"Glitchless", "All Locations Reachable"},
         {"Glitched", "Beatable Only"},
    }},
    {"Poes Shuffled", "Poe Souls", {
        {"Yes", "All"},
        {"No", "Vanilla"}
    }}
});

ArchipelagoContext& instance() {
    static ArchipelagoContext instance;
    return instance;
}

const SettingsNameConvert& GetAPSettingNameConvert(const std::string& apSettingName) {
    for (const auto& entry : sArchiSettingToDusklight) {
        if (entry.apName == apSettingName)
            return entry;
    }
    return sArchiSettingToDusklight[0];
}

const char* getMessageTypeName(AP_MessageType type) {
    switch (type) {
    case AP_MessageType::Plaintext:
        return "Plaintext";
    case AP_MessageType::ItemSend:
        return "ItemSend";
    case AP_MessageType::ItemRecv:
        return "ItemRecv";
    case AP_MessageType::Hint:
        return "Hint";
    case AP_MessageType::Countdown:
        return "Countdown";
    default:
        return nullptr;
    }
}

void ParseMessageData() {
    auto msg = AP_GetLatestMessage();

    switch (msg->type) {
    case AP_MessageType::ItemSend: {
        auto sendMsg = (AP_ItemSendMessage*)msg;
        ui::push_toast({
            .title = "Item Sent",
            .content = fmt::format("Sent {} to {}", sendMsg->item, sendMsg->recvPlayer),
            .duration = std::chrono::seconds(3),
        });

        DuskLog.info("[{}] {}", getMessageTypeName(msg->type), msg->text);
        break;
    }
    case AP_MessageType::ItemRecv: {
        auto recvMsg = (AP_ItemRecvMessage*)msg;

        ui::push_toast({
            .title = "Item Received",
            .content = fmt::format("Got {} From {}", recvMsg->item, recvMsg->sendPlayer),
            .duration = std::chrono::seconds(3),
        });
        // fallthrough for debug logging text contents
    }
    case AP_MessageType::Plaintext:
    case AP_MessageType::Hint:
    case AP_MessageType::Countdown:
        DuskLog.info("[{}] {}", getMessageTypeName(msg->type), msg->text);
        break;
    default:
        DuskLog.warn("Unknown message type! Type: {}", fmt::underlying(msg->type));
        break;
    }

    AP_ClearLatestMessage();
}

void ArchipelagoContext::LoadTempItemInfo() {
    auto itemDataTree = LOAD_EMBED_YAML(RANDO_DATA_PATH "items.yaml");
    for (const auto& itemNode : itemDataTree) {
        if (!itemNode["APItemId"]) {
            DuskLog.warn("Item {} missing APItemId field!", itemNode["Name"].as<std::string>());
            continue;
        }
        auto apItemId = itemNode["APItemId"].as<int>();

        if (apItemId == -1)
            continue;

        auto id = itemNode["Id"].as<int>();
        auto importance = randomizer::logic::item::ImportanceFromStr(itemNode["Importance"].as<std::string>());
        auto itemName = itemNode["Name"].as<std::string>();

        m_apItemToGameItem[apItemId] = {
            id,
            importance,
            itemName
        };
    }

    // add temporary replacement IDs for items not included in the base rando

    m_apItemToGameItem[16] = {  // Water Bombs (3)
        0x16,
        randomizer::logic::item::Importance::JUNK,
        "Water Bombs 5"
    };

    m_apItemToGameItem[20] = {  // Bomblings (3)
        0x1A,
        randomizer::logic::item::Importance::JUNK,
        "Bomblings 5"
    };
}

void ArchipelagoContext::LoadTempLocationInfo() {
    auto locDataTree = LOAD_EMBED_YAML(RANDO_DATA_PATH "locations.yaml");
    for (const auto& locNode : locDataTree) {
        const auto& metadata = locNode["Metadata"];
        auto locationName =  locNode["Name"].as<std::string>();

        if (!metadata.IsMap()) {
            DuskLog.warn("Location {} missing correct Metadata field!", locationName);
            continue;
        }

        if (!metadata["APLocationId"]) {
            DuskLog.warn("Location {} missing APLocationId field!", locationName);
            continue;
        }

        auto apLocationId = metadata["APLocationId"].as<int>();

        if (apLocationId == -1)
            continue;

        m_apLocToGameLoc.push_back({
            apLocationId,
            locationName
        });
    }
}

void ArchipelagoContext::itemRecvImpl(int id, bool notify) {
    if (!m_apItemToGameItem.contains(id)) {
        DuskLog.warn("Got an invalid Item Id: {}", id);
        return;
    }

    m_isAllowUpdateLocations = true; // guards against triggering UpdateCheckedLocations

    auto& item = m_apItemToGameItem[id];

    if (notify && item.importance == randomizer::logic::item::Importance::MAJOR) {
        DuskLog.info("[AP] Adding Item: {}", item.itemName);
        g_randomizerState.addItemToEventQueue(verifyProgressiveItem(item.itemId));
    }else {
        DuskLog.info("[AP] Silently Adding Item: {}", item.itemName);
        execItemGet(item.itemId);
    }

    m_isAllowUpdateLocations = false;
}

int ArchipelagoContext::getItemIdFromApId(int apId) {
    if (!m_apItemToGameItem.contains(apId)) {
        DuskLog.warn("Got an invalid Item Id: {}", apId);
        return -1;
    }

    auto& item = m_apItemToGameItem[apId];

    return item.itemId;
}

std::string ArchipelagoContext::getLocationNameFromApId(int apId) const {
    for (const auto& entry : m_apLocToGameLoc) {
        if (entry.apId == apId)
            return entry.locName;
    }
    return "";
}

bool ArchipelagoContext::tryKillPlayer() {
    if (!m_isNeedPlayerDeath)
        return false;

    auto linkActor = daAlink_getAlinkActorClass();

    if (!linkActor)
        return false;

    switch (linkActor->mProcID) {
        case daAlink_c::PROC_WAIT:
        case daAlink_c::PROC_TIRED_WAIT:
        case daAlink_c::PROC_MOVE:
        case daAlink_c::PROC_WOLF_WAIT:
        case daAlink_c::PROC_WOLF_TIRED_WAIT:
        case daAlink_c::PROC_WOLF_MOVE:
        case daAlink_c::PROC_ATN_MOVE:
        case daAlink_c::PROC_WOLF_ATN_AC_MOVE: {
            // Check if link is currently in a cutscene
            if (linkActor->checkEventRun())
                break;

            // Ensure that link is not currently in a message-based event.
            if (linkActor->getEventId() != 0)
                break;

            dComIfGs_setLife(0);

            m_isNeedPlayerDeath = false;

            return true;
        }
        default:
            break;
    }

    return false;
}

ArchipelagoContext::ArchipelagoContext() = default;

void ArchipelagoContext::SetServerIp(const std::string_view& ip) {
    getSettings().archipelago.serverIP.setValue(std::string(ip));
}

void ArchipelagoContext::SetSlotName(const std::string_view& name) {
    getSettings().archipelago.slotName.setValue(std::string(name));
}

void ArchipelagoContext::SetPassword(const std::string_view& pass) {
    getSettings().archipelago.serverPass.setValue(std::string(pass));
}

const std::string& ArchipelagoContext::GetServerIp() {
    return getSettings().archipelago.serverIP.getValue();
}

const std::string& ArchipelagoContext::GetSlotName() {
    return getSettings().archipelago.slotName.getValue();
}

const std::string& ArchipelagoContext::GetPassword() {
    return getSettings().archipelago.serverPass.getValue();
}

std::string ArchipelagoContext::GetArchipelagoSeedName() {
    if (IsConnected()) {
        auto& roomInfo = instance().m_roomInfo;
        return fmt::format("AP_{}", roomInfo.seed_name);
    }else {
        DuskLog.fatal("Archipelago was not connected when attempting to get seed name!");
    }
}

void ArchipelagoContext::GetSeedDirectoryPath(std::filesystem::path& outPath) {
    if (IsConnected()) {
        auto& roomInfo = instance().m_roomInfo;
        outPath = ui::GetRandomizerPath() / "archipelago" / GetArchipelagoSeedName();
    }
}

bool ArchipelagoContext::IsSeedHashArchipelago(const std::string& seedStr) {
    return seedStr.starts_with("AP_");
}

bool ArchipelagoContext::IsCurrentSeedHash(const std::string& seedStr) {
    return GetArchipelagoSeedName() == seedStr;
}

bool ArchipelagoContext::ConnectToServer(bool isBlocking) {
    config::Save();

    instance().LoadTempItemInfo();

    instance().LoadTempLocationInfo();

    AP_SetLogCallback([](const std::string& msg) {
       DuskLog.info("{}", msg);
    });

    AP_Init(GetServerIp().c_str(), "Twilight Princess", GetSlotName().c_str(), GetPassword().c_str());

    AP_NetworkVersion ver{0, 6,7};
    AP_SetClientVersion(&ver);

    AP_SetDeathLinkSupported(true);

    AP_SetDeathLinkRecvCallback([](std::string source, std::string cause) {
        DuskLog.info("Player {} sent death link. Cause: {}", source, cause);
        RequestPlayerDeath(true);
    });

    AP_SetItemClearCallback([]() {
        DuskLog.info("Item Clear Callback Called!");
        instance().m_isNeedResetInv = true;
    });

    AP_SetItemRecvCallback([](AP_NetworkItem& item, bool notify) {
        DuskLog.debug("Item Receive Callback Called! Item: {} Notify: {}", item.item, notify);
        HandleItemReceived(item, notify);
    });

    AP_SetLocationCheckedCallback([](int loc) {
        DuskLog.info("Location Checked Callback Called! Location: {}", loc);
        SetLocationChecked(loc, true);
    });

    AP_SetLocationInfoCallback([](std::vector<AP_NetworkItem> items) {
        DuskLog.info("Got {} Location Scouts from Server.", items.size());
        HandleReceiveLocationScout(items);
    });

    AP_RegisterSlotDataRawCallback("Settings", [](std::string data) {
        DuskLog.info("Got Settings from Slot Data.");
        instance().m_SettingsFile = data;
    });

    AP_RegisterSlotDataRawCallback("World Version", [](std::string data) {
        DuskLog.info("TP APWorld Version: {}", data);
    });

    AP_Start();

    // above func spawns a websocket thread, but there isn't really a good way to ensure a connection
    // attempt has been made except to wait for that thread to tick once

    if (isBlocking) {
        // wait for ws thread to run a frame before checking for status
        std::this_thread::sleep_for(std::chrono::seconds(1));

        while (AP_GetConnectionStatus() == AP_ConnectionStatus::Connecting)
            std::this_thread::yield();

        if (!IsConnected()) {
            DuskLog.error("Failed to connect to Archipelago Server!");
            return false;
        }
    }

    std::thread messageThread = std::thread(MessageThreadFunc);
    messageThread.detach();

    return true;
}

void ArchipelagoContext::DisconnectFromServer() {
    AP_Shutdown();
}

bool ArchipelagoContext::IsConnected() {
    auto status = AP_GetConnectionStatus();
    return status == AP_ConnectionStatus::Connected || status == AP_ConnectionStatus::Authenticated;
}

void ArchipelagoContext::MessageThreadFunc() {
    DuskLog.info("AP Thread started.");

    if (IsConnected()) {
        AP_GetRoomInfo(&instance().m_roomInfo);
        instance().m_isEnableDeathLink = AP_IsDeathLinkEnabled();
        RequestAllLocationScout();
    }

    while (IsConnected()) {
        if (AP_IsMessagePending())
            ParseMessageData();
    }

    DuskLog.info("AP Thread ended.");
}

void ArchipelagoContext::Execute() {
    if (!IsConnected())
        return;

    // reset player inventory if server requested it
    if (instance().m_isNeedResetInv) {
        HandleResetInventory();
        instance().m_isNeedResetInv = false;
        return; // end execution early so next frame can re-add inventory if needed
    }

    // process death links
    if (instance().tryKillPlayer()) {
        // if successful, don't bother processing item queue or location checks
        return;
    }

    // drain pending item queue here
    instance().m_queueMutex.lock();
    if (!instance().m_receivedItemsQueue.empty()) {
        for (auto item : instance().m_receivedItemsQueue) {
            instance().itemRecvImpl(item.first, item.second);
        }

        instance().m_receivedItemsQueue.clear();
    }
    instance().m_queueMutex.unlock();

    // update location checks here if we need to
    if (instance().m_isUpdateLocations) {
        UpdateCheckedLocations();
        instance().m_isUpdateLocations = false;
    }
}

void ArchipelagoContext::HandleItemReceived(AP_NetworkItem& netItem, bool notify) {
    int relativeId = netItem.item - ARCHI_ITEM_OFFSET;

    if (!notify && ((relativeId >= 0 && relativeId <= 6) || relativeId == 7)) {
        // skip rupee refills so players cant abuse disconnect/reconnect
        return;
    }

    if (netItem.location != -1 && IsLocationChecked(netItem.location)) {
        // no need to handle item if its location has already been checked
        return;
    }

    instance().m_queueMutex.lock();
    instance().m_receivedItemsQueue.push_back({relativeId, notify});
    instance().m_queueMutex.unlock();
}

void ArchipelagoContext::HandleResetInventory() {
    DuskLog.info("Resetting Inventory.");
    // NOTE: this does not clear ALL things from save, so if a player managed to do something while disconnected from the archi, it might mess with things

    auto& playerInfo = g_dComIfG_gameInfo.info.getPlayer();

    // reset items
    playerInfo.getItem().init();
    playerInfo.getGetItem().init();

    // reset collect (poes, shards, swords)
    playerInfo.getCollect().init();

    playerInfo.getPlayerStatusA().setMaxLife(15);
    playerInfo.getPlayerStatusA().setWalletSize(WALLET);
    // dont reset rupees, and instead reject rupee updates while refilling inv

    // sync all location collect flags with current collection status obtained from initial room connection
    UpdateAllLocationState();

    // clear all item-related flags

    dComIfGs_offEventBit(0x2580); // Power up dominion rod

    // shadow crystal
    dComIfGs_offEventBit(0xD04); // Can transform at will
    dComIfGs_offEventBit(0x501); // Midna Charge Unlocked

    // hidden skills
    dComIfGs_offEventBit(0x2904); // ENDING BLOW
    dComIfGs_offEventBit(0x2908); // SHIELD ATTACK
    dComIfGs_offEventBit(0x2902); // BACK SLICE
    dComIfGs_offEventBit(0x2901); // HELM SPLITTER
    dComIfGs_offEventBit(0x2A80); // MORTAL DRAW
    dComIfGs_offEventBit(0x2A40); // JUMP STRIKE
    dComIfGs_offEventBit(0x2A20); // GREAT SPIN

}

void ArchipelagoContext::HandleReceiveLocationScout(const std::vector<AP_NetworkItem>& items) {
    for (const auto& item : items) {
        int parsedItemId;
        std::string parsedItemName;
        if (item.player == AP_GetPlayerID()) {
            int adjustedId = item.item - ARCHI_ITEM_OFFSET;

            if (instance().m_apItemToGameItem.contains(adjustedId)) {
                auto& itemInfo = instance().m_apItemToGameItem[adjustedId];
                parsedItemId = itemInfo.itemId;
                parsedItemName = itemInfo.itemName;
            }else {
                parsedItemId = -1;
                parsedItemName = "Unknown";
            }
        }else {
            parsedItemId = dItemNo_Randomizer_ARCHIPELAGO_ITEM_e;
            parsedItemName = "Archipelago Item";
        }
        int locationId = item.location - ARCHI_ITEM_OFFSET;

        auto locName = instance().getLocationNameFromApId(locationId);

        if (locName.empty()) {
            DuskLog.info("No location with ID {} found.", locationId);
            continue;
        }

        bool collected = false;
        if (instance().m_initLocationCollectState.contains(item.location))
            collected = instance().m_initLocationCollectState[item.location];

        instance().m_locationItemInfo[locName] = {
            parsedItemId,
            parsedItemName,
            locName,
            item.location,
            collected
        };
    }
}
// TODO: atm this is a sort of lazy solution to not having direct access to what location was checked when an execItemGet is called
// so eventually finding a way to properly associate locations with their respective item get funcs would benefit this system
void ArchipelagoContext::UpdateCheckedLocations() {
    auto& world = instance().m_archiWorld;

    bool changed = false;

    for (auto location : world->GetAllLocations()) {
        // skip locations that aren't progression, which are locations that just aren't randomized
        if (!location->IsProgression()) {
            continue;
        }

        auto locName = location->GetName();

        if (!instance().m_locationItemInfo.contains(locName)) {
            DuskLog.debug("No item found for ({}).", locName);
            continue;
        }

        auto& cachedLocData = instance().m_locationItemInfo[locName];

        bool isCollected = isLocationObtained(location);

        if (isCollected && !cachedLocData.collected) {
            cachedLocData.collected = true;
            AP_SendItem(cachedLocData.apLocationId);
            changed = true;
        }
    }

    if (!changed) {
        DuskLog.warn("No locations had any changes! this might not be normal.");
    }
}

void ArchipelagoContext::SetNeedUpdateLocations(bool update) {
    if (!instance().m_isAllowUpdateLocations)
        instance().m_isUpdateLocations = update;
}

bool ArchipelagoContext::IsLocationChecked(int locId) {
    auto& world = instance().m_archiWorld;

    for (const auto& [locName, locInfo] : instance().m_locationItemInfo) {
        if (locInfo.apLocationId == locId) {
            if (locInfo.collected)
                return true;

            if (auto location = world->GetLocation(locInfo.locationName, true)) {
                return isLocationObtained(location);
            }

            DuskLog.error("Failed to obtain location: {}", locName);
            return false;
        }
    }
    return false;
}

void ArchipelagoContext::SetLocationChecked(int locId, bool collected) {
    // func was ran before location scouts could be sent out, cache result until scouts return.
    if (!IsReceivedLocationScouts()) {
        instance().m_initLocationCollectState[locId] = collected;
        return;
    }

    auto& world = instance().m_archiWorld;

    for (auto& [locName, locInfo] : instance().m_locationItemInfo) {
        if (locInfo.apLocationId == locId) {
            locInfo.collected = collected;

            // update location flags if possible
            auto location = world->GetLocation(locInfo.locationName, true);
            if (!location || !location->IsProgression())
                return;

            setLocationCollected(location, collected);
            return;
        }
    }

    DuskLog.warn("No location found for locId {}.", locId);
}

void ArchipelagoContext::UpdateLocationState(int locId, bool collected) {
    auto& world = instance().m_archiWorld;

    for (const auto& [locName, locInfo] : instance().m_locationItemInfo) {
        if (locInfo.apLocationId == locId) {
            auto location = world->GetLocation(locInfo.locationName, true);
            if (!location || !location->IsProgression())
                continue;

            setLocationCollected(location, collected);
            return;
        }
    }

    DuskLog.warn("No location found for locId {}.", locId);
}

void ArchipelagoContext::UpdateAllLocationState() {
    auto& world = instance().m_archiWorld;

    for (const auto& [locName, locInfo] : instance().m_locationItemInfo) {
        auto location = world->GetLocation(locInfo.locationName, true);
        if (!location || !location->IsProgression())
            continue;

        setLocationCollected(location, locInfo.collected);
    }
}

bool ArchipelagoContext::IsReceivedLocationScouts() {
    return !instance().m_locationItemInfo.empty();
}

void ArchipelagoContext::TryHandleDeathLink() {
    if (instance().m_isEnableDeathLink && !instance().m_isFromDeathLink) {
        // TODO: come up with better death messages
        AP_DeathLinkSend("%YOU% was unable to become the Hero of Twilight.");
    }
}

void ArchipelagoContext::RequestAllLocationScout(bool isHint) {
    std::set<int64_t> locations;
    // TEMP: apworld has 475 locations with ids in sequential order, so add them all individually to location set
    // (eventually we will iterate through locations.yaml for a better data-driven solution)
    for (int i = 0; i < 475; i++) {
        locations.insert(ARCHI_ITEM_OFFSET + i);
    }

    AP_SendLocationScouts(locations, isHint);
}

void ArchipelagoContext::RequestPlayerDeath(bool isDeathLink) {
    instance().m_isNeedPlayerDeath = true;
    instance().m_isFromDeathLink = isDeathLink;
}

bool ArchipelagoContext::GenerateConfigFromAP(randomizer::seedgen::config::Config& config, const std::string& settingsStr) {
    YAML::Node apConfigYaml;
    try {
        apConfigYaml = YAML::Load(settingsStr);
    }catch (YAML::BadFile& e) {
        DuskLog.warn("Failed to load AP Config YAML file!");
        return false;
    }

    config.SetSeed("Archipelago");
    randomizer::seedgen::settings::Settings& settings = config.GetSettings();

    // update settings using ap config
    for (const auto& apSettingEntry : apConfigYaml) {
        auto apSettingName = apSettingEntry.first.as<std::string>();
        auto apSettingValue = apSettingEntry.second.as<std::string>();

        const auto& settingConvert = GetAPSettingNameConvert(apSettingName);

        if (!settingConvert.apName.empty()) {
            auto& setting = settings.GetMap().at(settingConvert.dusklightName);
            setting.SetCurrentOption(settingConvert.tryGetOptionConvert(apSettingValue));
        } else if (apSettingName == "Castle Requirements") {
            auto& setting = settings.GetMap().at("Hyrule Barrier Requirements");

            // ap assumes max mirror shards/fused shadows/dungeons, so update those settings as well

            if(apSettingValue == "Open")
                setting.SetCurrentOption("Open");
            else if(apSettingValue == "Vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "Fused Shadows") {
                setting.SetCurrentOption("Fused Shadows");
                settings.GetMap().at("Hyrule Barrier Fused Shadows").SetCurrentOption("3");
            }else if(apSettingValue == "Mirror Shards") {
                setting.SetCurrentOption("Mirror Shards");
                settings.GetMap().at("Hyrule Barrier Mirror Shards").SetCurrentOption("4");
            }else if(apSettingValue == "All Dungeons") {
                setting.SetCurrentOption("Dungeons");
                settings.GetMap().at("Hyrule Barrier Dungeons").SetCurrentOption("8");
            }
        }else if (apSettingName == "Temple of Time Entrance Requirements") {
            auto& setting = settings.GetMap().at("Sacred Grove Does Not Require Skull Kid");
            auto& setting2 = settings.GetMap().at("Temple of Time Sword Requirement");

            if(apSettingValue == "Closed") {
                setting.SetCurrentOption("Off");
                setting2.SetCurrentOption("Master Sword");
            }else if (apSettingValue == "Open Grove") {
                setting.SetCurrentOption("On");
                setting2.SetCurrentOption("Master Sword");
            }else if (apSettingValue == "Open") {
                setting.SetCurrentOption("On");
                setting2.SetCurrentOption("None");
            }
        }else {
            DuskLog.debug("Missing Setting: {} Value: {}", apSettingName, apSettingValue);
        }
    }

    return true;
}

int ArchipelagoContext::GetItemAtLocation(const std::string& locName) {
    if (!instance().m_locationItemInfo.contains(locName)) {
        DuskLog.warn("No item found for ({}).", locName);
        return 0;
    }
    return instance().m_locationItemInfo[locName].itemId;
}

int ArchipelagoContext::GetItemAtLocation(int locId) {
    for (const auto& [locName, locInfo] : instance().m_locationItemInfo) {
        if (locInfo.apLocationId == locId) {
            return locInfo.itemId;
        }
    }
    return 0;
}

void ArchipelagoContext::CreateArchipelagoWorld() {
    std::filesystem::path workingDir;
    GetSeedDirectoryPath(workingDir);

    auto trackerRando = randomizer::Randomizer(workingDir);
    trackerRando.GenerateTrackerWorld(false);

    instance().m_archiWorld = std::move(trackerRando.GetWorlds().front());
}

void ArchipelagoContext::FillArchipelagoWorld() {
    auto& world = instance().m_archiWorld;

    if (world == nullptr) {
        DuskLog.error("Archipelago world was not created!");
        return;
    }

    auto& locationInfo = instance().m_locationItemInfo;

    // fill all locations with data pulled from archi session
    for (auto location : world->GetAllLocations()) {
        // skip locations that aren't progression, which are locations that just aren't randomized
        if (!location->IsProgression()) {
            location->SetCurrentItem(location->GetOriginalItem());
            continue;
        }

        auto locName = location->GetName();
        if (!locationInfo.contains(locName)) {
            if (!location->HasCategories("Warp Portal") &&
                !location->HasCategories("Placeholder") &&
                !location->HasCategories("Hint Sign"))
                DuskLog.warn("Missing archipelago location data for: {}", locName);
            auto origItem = location->GetOriginalItem();

            // set location to original item

            if (origItem->GetID() != -1) // ensure item is not nothing
                location->SetCurrentItem(origItem);
            else
                DuskLog.info("Location ({}) does not have an original item!", locName);

            continue;
        }

        auto& locInfo = locationInfo[locName];
        if (locInfo.itemId != -1) {
            location->SetCurrentItem(world->GetItem(locInfo.itemId));
        }else {
            DuskLog.info("Skipping location ({}) as item is -1.", locName);
        }
    }
}

void ArchipelagoContext::CreateRandomizerContext() {
    auto& world = instance().m_archiWorld;

    // Set hint texts before writing context
    randomizer::logic::hints::GenerateAllHints(world);

    // TODO: generate archipelago item get text replacements

    auto randoData = WriteSeedData(world.get());
    randoData.mHash = GetArchipelagoSeedName();

    randomizer_GetContext() = randoData;

    std::filesystem::path workingDir;
    GetSeedDirectoryPath(workingDir);

    auto writeToFileResult = randoData.WriteToFile(workingDir / "seed.dat");

    if (writeToFileResult.has_value()) {
        DuskLog.error("Failed to create Rando Data. Reason: {}", writeToFileResult.value());
        return;
    }
}

void ArchipelagoContext::LoadRandomizerContext() {
    randomizer_GetContext() = RandomizerContext();

    std::filesystem::path workingDir;
    GetSeedDirectoryPath(workingDir);

    randomizer_GetContext().LoadFromPath(workingDir / "seed.dat");
    randomizer_GetContext().mHash = GetArchipelagoSeedName();
}

void ArchipelagoContext::GenerateLocalWorldData() {
    bool createContext = false;
    std::filesystem::path workingDir;

    GetSeedDirectoryPath(workingDir);

    if (std::filesystem::exists(workingDir)) {
        instance().m_config.LoadFromFile(workingDir / "settings.yaml", workingDir / "preferences.yaml");
    }else {
        std::filesystem::create_directories(workingDir);
        // creates base yamls at directory if they dont exist yet
        instance().m_config.LoadFromFile(workingDir / "settings.yaml", workingDir / "preferences.yaml");

        if (instance().m_SettingsFile.empty()) {
            DuskLog.fatal("Settings Data was not sent to client! Unable to generate world data.");
            return;
        }

        GenerateConfigFromAP(instance().m_config, instance().m_SettingsFile);

        instance().m_config.WriteToFile(workingDir / "settings.yaml", workingDir / "preferences.yaml");

        createContext = true;
    }

    CreateArchipelagoWorld();

    FillArchipelagoWorld();

    if (createContext) {
        CreateRandomizerContext();
    }else {
        LoadRandomizerContext();
    }
}
} // dusk::archi