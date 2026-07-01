#include <dusk/imgui/ImGuiArchipelagoDebug.h>

#include "imgui.h"

#include "Archipelago.h"
#include "aurora/lib/window.hpp"
#include "dusk/file_select.hpp"
#include "dusk/archipelago/archipelago_context.hpp"
#include "dusk/ui/rando_seed_generation.hpp"

namespace dusk {

ImGuiArchipelagoDebug::ImGuiArchipelagoDebug() {

}

void ImGuiArchipelagoDebug::drawMenuItem() {
    if (ImGui::BeginMenu("Randomizer")) {
        ImGui::Checkbox("Archipelago Window", &m_drawWindow);
        ImGui::EndMenu();
    }
}

void ImGuiArchipelagoDebug::drawWindow() {
    if (!m_drawWindow)
        return;

    if (!m_hasInitValues) {
        m_hasInitValues = true;
    }

    ImGui::Begin("Archipelago Debug", &m_drawWindow);

    if (archi::ArchipelagoContext::IsConnected()) {
        if (ImGui::Button("Test Create World Data")) {
            archi::ArchipelagoContext::GenerateLocalWorldData();
        }

        if (ImGui::Button("Disconnect")) {
            archi::ArchipelagoContext::DisconnectFromServer();
        }
    }else {
        if (ImGui::Button("Connect")) {
            archi::ArchipelagoContext::ConnectToServer(dComIfGs_getDataNum());
        }
    }

    ImGui::SeparatorText("Debug Buttons");

    static bool isSimulateDeathLink = false;

    if (ImGui::Button("Request Death Link")) {
        archi::ArchipelagoContext::RequestPlayerDeath(isSimulateDeathLink);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Simulate Death Link", &isSimulateDeathLink);

    ImGui::End();
}
}