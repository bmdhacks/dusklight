#include <dusk/archipelago/archipelago_context.hpp>

#include <thread>

#include "Archipelago.h"
#include "d/d_item.h"
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
    std::string apName;
    std::string dusklightName;
    bool invert = false;
};

static auto sArchiSettingToDusklight = std::to_array<SettingsNameConvert>({
    {"", ""},
    {"golden_bugs_shuffled", "Golden Bugs"},
    {"sky_characters_shuffled", "Sky Characters"},
    {"npc_items_shuffled", "Gifts From NPCs"},
    {"shop_items_shuffled", "Shop Items"},
    {"hidden_skills_shuffled", "Hidden Skills"},
    // {"poe_shuffled", ""}, // poe shuffle is Overworld, Dungeon, All, or Vanilla, so special logic is needed to convert
    // {"heart_piece_shuffled", ""},
    // {"overworld_shuffled", ""},
    // {"dungeons_shuffled", ""},
    {"dungeon_rewards_progression", "Dungeon Rewards Can Be Anywhere"},
    {"small_keys_on_bosses", "No Small Keys on Bosses", true},
    {"skip_prologue", "Skip Prologue"},
    {"faron_twilight_cleared", "Faron Twilight Cleared"},
    {"eldin_twilight_cleared", "Eldin Twilight Cleared"},
    {"lanayru_twilight_cleared", "Lanayru Twilight Cleared"},
    {"skip_mdh", "Skip Midna's Desparate Hour"},
    {"open_map", "Unlock Map Regions"},
    {"increase_wallet", "Logic Increase Wallet Capacity"},
    {"transform_anywhere", "Logic Transform Anywhere"},
    {"bonks_do_damage", "Bonks Do Damage"},
    {"skip_lakebed_entrance", "Lakebed Does Not Require Water Bombs"},
    {"skip_arbiters_grounds_entrance", "Arbiters Does Not Require Bulblin Camp"},
    {"skip_snowpeak_entrance", "Snowpeak Does Not Require Reekfish Scent"},
    {"skip_city_in_the_sky_entrance", "City Does Not Require Filled Skybook"},
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
    case AP_MessageType::ItemSend:
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
        return fmt::format("AP_{}_{}", GetSlotName(), roomInfo.seed_name);
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

void ArchipelagoContext::ConnectToServer() {
    config::Save();

    instance().LoadTempItemInfo();

    instance().LoadTempLocationInfo();

    AP_Init(GetServerIp().c_str(), "Twilight Princess", GetSlotName().c_str(), GetPassword().c_str());

    AP_NetworkVersion ver{0, 6,7};
    AP_SetClientVersion(&ver);

    AP_SetItemClearCallback([]() {
        DuskLog.info("Item Clear Callback Called!");
        instance().m_isNeedResetInv = true;
    });

    AP_SetItemRecvCallback([](AP_NetworkItem& item, bool notify) {
        DuskLog.info("Item Receive Callback Called! Item: {} Notify: {}", item.item, notify);
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

    AP_Start();

    if (AP_GetConnectionStatus() == AP_ConnectionStatus::ConnectionRefused) {
        DuskLog.warn("Failed to Connect to Archipelago Server.");
        return;
    }

    std::thread messageThread = std::thread(MessageThreadFunc);
    messageThread.detach();
}

void ArchipelagoContext::DisconnectFromServer() {
    if (!IsConnected()) {
        DuskLog.warn("Attempted to disconnect from an already disconnected state!");
        return;
    }

    AP_Shutdown();
}

bool ArchipelagoContext::IsConnected() {
    auto status = AP_GetConnectionStatus();
    return status == AP_ConnectionStatus::Connected || status == AP_ConnectionStatus::Authenticated;
}

void ArchipelagoContext::MessageThreadFunc() {
    // wait a bit before checking connection state, as websocket is probably not connected yet
    // (i really am not liking APCpp, why cant I check if the websocket is in the process of connecting???)
    std::this_thread::sleep_for(std::chrono::seconds(2));

    DuskLog.info("AP Thread started.");

    if (IsConnected()) {
        AP_GetRoomInfo(&instance().m_roomInfo);
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
            DuskLog.warn("No item found for ({}).", locName);
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
    if (instance().m_locationItemInfo.empty()) {
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

void ArchipelagoContext::RequestAllLocationScout(bool isHint) {
    std::set<int64_t> locations;
    // TEMP: apworld has 475 locations with ids in sequential order, so add them all individually to location set
    // (eventually we will iterate through locations.yaml for a better data-driven solution)
    for (int i = 0; i < 475; i++) {
        locations.insert(ARCHI_ITEM_OFFSET + i);
    }

    AP_SendLocationScouts(locations, isHint);
}

void ArchipelagoContext::SetAPConfigYamlPath(const std::string_view& path) {
    instance().m_apConfigPath = path;
}

bool ArchipelagoContext::GenerateConfigFromAP(randomizer::seedgen::config::Config& config) {
    if (instance().m_apConfigPath.empty()) {
        DuskLog.warn("AP Config Path Empty!");
        return false;
    }

    if (!std::filesystem::exists(instance().m_apConfigPath)) {
        DuskLog.warn("AP Config Path does not exist!");
        return false;
    }

    YAML::Node apConfigYaml;
    try {
        apConfigYaml = YAML::LoadFile(instance().m_apConfigPath);
    }catch (YAML::BadFile& e) {
        DuskLog.warn("Failed to load AP Config YAML file!");
        return false;
    }

    config.SetSeed("Archipelago");
    randomizer::seedgen::settings::Settings& settings = config.GetSettings();

    // update settings using ap config
    for (const auto& apSettingEntry : apConfigYaml["Twilight Princess"]) {
        auto apSettingName = apSettingEntry.first.as<std::string>();

        // ignore AP-only settings
        if (apSettingName == "progression_balancing" ||
            apSettingName == "accessibility" ||
            apSettingName == "local_items" ||
            apSettingName == "non_local_items" ||
            apSettingName == "start_inventory" ||
            apSettingName == "start_hints" ||
            apSettingName == "start_location_hints" ||
            apSettingName == "exclude_locations" ||
            apSettingName == "priority_locations" ||
            apSettingName == "start_inventory_from_pool")
            continue;

        const auto& settingConvert = GetAPSettingNameConvert(apSettingName);

        if (!settingConvert.apName.empty()) {
            bool apSettingValue = apSettingEntry.second.as<bool>();

            if (settingConvert.invert)
                apSettingValue = !apSettingValue;

            auto& setting = settings.GetMap().at(settingConvert.dusklightName);

            setting.SetCurrentOption(apSettingValue ? "On" : "Off");

            continue;
        }
        if (apSettingName == "poe_shuffled") {
            auto& setting = settings.GetMap().at("Poe Souls");
            bool apSettingValue = apSettingEntry.second.as<bool>();

            // this setting has more options, but the current apworld only has off or on for now.
            setting.SetCurrentOption(apSettingValue ? "All" : "Vanilla");

            continue;
        }
        // remaining settings will have string values

        auto apSettingValue = apSettingEntry.second.as<std::string>();

        // TODO: clean up this if-else hellscape

        if (apSettingName == "castle_requirements") {
            auto& setting = settings.GetMap().at("Hyrule Barrier Requirements");

            // ap assumes max mirror shards/fused shadows/dungeons, so update those settings as well

            if(apSettingValue == "open")
                setting.SetCurrentOption("Open");
            else if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "fused_shadows") {
                setting.SetCurrentOption("Fused Shadows");
                settings.GetMap().at("Hyrule Barrier Fused Shadows").SetCurrentOption("3");
            }else if(apSettingValue == "mirror_shards") {
                setting.SetCurrentOption("Mirror Shards");
                settings.GetMap().at("Hyrule Barrier Mirror Shards").SetCurrentOption("4");
            }else if(apSettingValue == "all_dungeons") {
                setting.SetCurrentOption("Dungeons");
                settings.GetMap().at("Hyrule Barrier Dungeons").SetCurrentOption("8");
            }
        }else if (apSettingName == "palace_requirements") {
            auto& setting = settings.GetMap().at("Palace of Twilight Requirements");

            if(apSettingValue == "open")
                setting.SetCurrentOption("Open");
            else if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "fused_shadows")
                setting.SetCurrentOption("Fused Shadows");
            else if(apSettingValue == "mirror_shards")
                setting.SetCurrentOption("Mirror Shards");

        }else if (apSettingName == "faron_woods_logic") {
            auto& setting = settings.GetMap().at("Faron Woods Logic");

            if(apSettingValue == "open")
                setting.SetCurrentOption("Open");
            else if(apSettingValue == "closed")
                setting.SetCurrentOption("Closed");
        }else if (apSettingName == "small_key_settings") {
            auto& setting = settings.GetMap().at("Small Keys");

            if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "own_dungeon")
                setting.SetCurrentOption("Own Dungeon");
            else if(apSettingValue == "any_dungeon")
                setting.SetCurrentOption("Any Dungeon");
            else if(apSettingValue == "anywhere")
                setting.SetCurrentOption("Anywhere");
            else if(apSettingValue == "startwith")
                setting.SetCurrentOption("Keysy");

        }else if (apSettingName == "big_key_settings") {
            auto& setting = settings.GetMap().at("Big Keys");

            if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "own_dungeon")
                setting.SetCurrentOption("Own Dungeon");
            else if(apSettingValue == "any_dungeon")
                setting.SetCurrentOption("Any Dungeon");
            else if(apSettingValue == "anywhere")
                setting.SetCurrentOption("Anywhere");
            else if(apSettingValue == "startwith")
                setting.SetCurrentOption("Keysy");

        }else if (apSettingName == "map_and_compass_settings") {
            auto& setting = settings.GetMap().at("Maps and Compasses");

            if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "own_dungeon")
                setting.SetCurrentOption("Own Dungeon");
            else if(apSettingValue == "any_dungeon")
                setting.SetCurrentOption("Any Dungeon");
            else if(apSettingValue == "anywhere")
                setting.SetCurrentOption("Anywhere");
            else if(apSettingValue == "startwith")
                setting.SetCurrentOption("Keysy");

        }else if (apSettingName == "trap_frequency") {
            auto& setting = settings.GetMap().at("Trap Item Frequency");

            if(apSettingValue == "no_traps")
                setting.SetCurrentOption("None");
            else if(apSettingValue == "few")
                setting.SetCurrentOption("Few");
            else if(apSettingValue == "many")
                setting.SetCurrentOption("Many");
            else if(apSettingValue == "mayhem")
                setting.SetCurrentOption("Mayhem");
            else if(apSettingValue == "nightmare")
                setting.SetCurrentOption("Nightmare");

        }else if (apSettingName == "damage_magnification") {
            auto& setting = settings.GetMap().at("Logic Damage Multiplier");

            if(apSettingValue == "vanilla")
                setting.SetCurrentOption("Vanilla");
            else if(apSettingValue == "double")
                setting.SetCurrentOption("Double");
            else if(apSettingValue == "triple")
                setting.SetCurrentOption("Triple");
            else if(apSettingValue == "quadruple")
                setting.SetCurrentOption("Quadruple");
            else if(apSettingValue == "ohko")
                setting.SetCurrentOption("OHKO");

        }else if (apSettingName == "goron_mines_entrance") {
            auto& setting = settings.GetMap().at("Goron Mines Entrance");

            if(apSettingValue == "closed")
                setting.SetCurrentOption("Closed");
            else if(apSettingValue == "no_wrestling")
                setting.SetCurrentOption("No Wrestling");
            else if(apSettingValue == "open")
                setting.SetCurrentOption("Open");

        }else if (apSettingName == "tot_entrance") {
            auto& setting = settings.GetMap().at("Sacred Grove Does Not Require Skull Kid");
            auto& setting2 = settings.GetMap().at("Temple of Time Sword Requirement");

            if(apSettingValue == "closed") {
                setting.SetCurrentOption("Off");
                setting2.SetCurrentOption("Master Sword");
            }else if (apSettingValue == "open_grove") {
                setting.SetCurrentOption("On");
                setting2.SetCurrentOption("Master Sword");
            }else if (apSettingValue == "open") {
                setting.SetCurrentOption("On");
                setting2.SetCurrentOption("None");
            }

        }else if (apSettingName == "logic_rules") {
            auto& setting = settings.GetMap().at("Logic Rules");

            if(apSettingValue == "glitchless") {
                setting.SetCurrentOption("All Locations Reachable");
            }else if (apSettingValue == "glitched") { // this might not be the most direct translation
                setting.SetCurrentOption("Beatable Only");
            }
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

        GenerateConfigFromAP(instance().m_config);

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