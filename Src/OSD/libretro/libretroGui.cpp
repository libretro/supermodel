#include <GL/glew.h>
#include "libretroGui.h"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <thread>
#include "GameLoader.h"
#include "../../Pkgs/imgui/imgui.h"
#include "../../Pkgs/imgui/imgui_internal.h"
#include "../../Pkgs/imgui/imgui_impl_opengl3.h"
#include "../../Util/NewConfig.h"
#include "../../Util/ConfigBuilders.h"
#include "../../Graphics/GLSLVersion.h"
#include "../Src/Inputs/Inputs.h"
#include "LibretroConfigProvider.h"
#include "LibretroWrapper.h"

namespace fs = std::filesystem;

// [FIX] Removed global 'config' variable to prevent Static Initialization Order crash.
// Instead, we use a static getter to initialize it safely on first use.
[[maybe_unused]] static Util::Config::Node& GetConfig() {
    // This will only run once, the first time this function is called.
    static Util::Config::Node s_config = LibretroConfigProvider::DefaultConfig(LibretroWrapper::GetGameXMLPath());
    return s_config;
}

// SDL stubs to keep the existing function signatures as similar as possible

[[maybe_unused]] static void WriteGameNode(Util::Config::Node& baseNode, const Util::Config::Node& diffNode, Util::Config::Node& writeNode, const std::string& group)
{
    for (const auto& n : baseNode) {
        if (n.IsLeaf() && n.Exists()) {
            auto& key = n.Key();
            auto  val = n.GetValue();
            if (val) {
                auto vRange = val->GetValueRange();
                if (vRange->GetGroup() != group) {
                    continue;
                }
                // Use the const iterator to avoid triggering range_error lookup
                auto s1 = n.ValueAs<std::string>();
                
                // For the diffNode, we must check existence first
                std::string s2 = s1;
                for (const auto& dn : diffNode) {
                    if (dn.Key() == key) {
                        s2 = dn.ValueAs<std::string>();
                        break;
                    }
                }

                if (s1 != s2) {
                    writeNode[key] = s2;
                }
            }
        }
    }
}

[[maybe_unused]] static std::string NodeToString(Util::Config::Node& config)
{
    std::string s;
    for (const auto& n : config) {
        if (n.IsLeaf() && n.Exists()) {
            s += n.Key();
            s += " ";
            s += n.ValueAs<std::string>();
            s += "\n";
        }
    }
    return s;
}

static void UpdateTempValues(Util::Config::Node& config, const std::string group, bool init)
{
    for (auto it = config.begin(); it != config.end(); ++it)
    {
        if (it->IsLeaf() && it->Exists()) {
            auto val = it->GetValue();
            if (val && val->GetValueRange() && val->GetValueRange()->GetGroup() == group) {
                auto vRange = val->GetValueRange();
                auto index = vRange->GetIndex();

                if (init) {
                    // Access via iterator 'it' to avoid config[key] lookup
                    switch (index) {
                        case 0: vRange->tempValue = it->ValueAs<bool>(); break;
                        case 1: vRange->tempValue = it->ValueAs<unsigned>(); break;
                        case 2: vRange->tempValue = it->ValueAs<int>(); break;
                        case 3: vRange->tempValue = it->ValueAs<float>(); break;
                        case 4: vRange->tempValue = it->ValueAs<std::string>(); break;
                    }
                } else {
                    // Call Set directly on the Value object to avoid Node assignment/lookup logic
                    switch (index) {
                        case 0: val->Set(std::get<bool>(vRange->tempValue)); break;
                        case 1: val->Set(std::get<unsigned>(vRange->tempValue)); break;
                        case 2: val->Set(std::get<int>(vRange->tempValue)); break;
                        case 3: val->Set(std::get<float>(vRange->tempValue)); break;
                        case 4: val->Set(std::get<std::string>(vRange->tempValue)); break;
                    }
                }
                val->SetValueRange(vRange);
            }
        }
    }
}

static void CreateControls(Util::Config::Node& config, const std::string group)
{
    for (auto it = config.begin(); it != config.end(); ++it)
    {
        if (it->IsLeaf() && it->Exists()) {
            auto key = it->Key();
            auto val = it->GetValue();
            if (val && val->GetValueRange() && val->GetValueRange()->GetGroup() == group) {
                auto vRange = val->GetValueRange();
                auto index = vRange->GetIndex();
                auto& list = vRange->GetList();

                auto ProcessCombo = [&](auto* valuePtr) {
                    using T = std::decay_t<decltype(*valuePtr)>;
                    int selectedIndex = 0;
                    int loopCount = 0;
                    std::vector<std::string> sVector;
                    std::vector<const char*> sVectorChar;
                    for (auto& l : list) {
                        auto value = std::get<T>(l);
                        sVector.emplace_back(std::to_string(value));
                        if (value == *valuePtr) selectedIndex = loopCount;
                        loopCount++;
                    }
                    for (auto& s : sVector) sVectorChar.emplace_back(s.c_str());
                    ImGui::Combo(key.c_str(), &selectedIndex, sVectorChar.data(), (int)sVectorChar.size());
                    vRange->tempValue = list[selectedIndex];
                };

                auto ProcessScalar = [&](auto valuePtr, ImGuiDataType type) {
                    using T = std::decay_t<decltype(*valuePtr)>;
                    auto min_ = std::get<T>(vRange->GetMin());
                    auto max_ = std::get<T>(vRange->GetMax());
                    ImGui::SliderScalar(key.c_str(), type, valuePtr, &min_, &max_);
                };
                
                auto ProcessControls = [&](auto valuePtr, ImGuiDataType type) {
                    if (vRange->HasMinMax()) ProcessScalar(valuePtr, type);
                    else if (list.size()) ProcessCombo(valuePtr);
                    else ImGui::InputScalar(key.c_str(), type, valuePtr);
                };

                switch (index) {
                    case 0: ImGui::Checkbox(key.c_str(), std::get_if<bool>(&vRange->tempValue)); break;
                    case 1: ProcessControls(std::get_if<unsigned>(&vRange->tempValue), ImGuiDataType_U32); break;
                    case 2: ProcessControls(std::get_if<int>(&vRange->tempValue), ImGuiDataType_S32); break;
                    case 3: ProcessControls(std::get_if<float>(&vRange->tempValue), ImGuiDataType_Float); break;
                    case 4: {
                        auto& option = std::get<std::string>(vRange->tempValue);
                        if (list.size()) {
                            int sel = 0, i = 0;
                            std::vector<const char*> items;
                            for (auto& l : list) {
                                auto& s = std::get<std::string>(l);
                                if (option == s) sel = i;
                                items.push_back(s.c_str()); i++;
                            }
                            if (ImGui::Combo(key.c_str(), &sel, items.data(), (int)items.size())) vRange->tempValue = list[sel];
                        } else {
                            char buffer[256];
                            std::strncpy(buffer, option.c_str(), 255);
                            if (ImGui::InputText(key.c_str(), buffer, 256)) option = buffer;
                        }
                        break;
                    }
                }
            }
        }
    }
}

static void SetDefaultKeyVal(std::shared_ptr<CInput> input)
{
    std::string key = std::string("Input") + input->id;
    auto defaultConfig = LibretroConfigProvider::DefaultConfig(LibretroWrapper::s_gameXMLFilePath);
    // Use a safe iterator lookup for the default config to prevent crashes
    for (const auto& n : defaultConfig) {
        if (n.Key() == key) {
            input->SetMapping(n.ValueAs<std::string>().c_str());
            break;
        }
    }
}

static std::vector<std::string> SplitByComma(const std::string& input) 
{
    std::vector<std::string> result;
    size_t start = 0, end;
    while ((end = input.find(',', start)) != std::string::npos) {
        result.emplace_back(input.substr(start, end - start));
        start = end + 1;
    }
    result.emplace_back(input.substr(start));
    return result;
}

struct KeyBindState {
    std::shared_ptr<CInput> input;
    bool waitingForInput = false;
    int processKeyCount = 0;
    void Reset() { input = nullptr; waitingForInput = false; processKeyCount = 0; }
};

static void BindKeys(Util::Config::Node& config, KeyBindState& kb, bool openPopup)
{
    bool finish = false;
    bool appendPressed = false;
    if (openPopup) ImGui::OpenPopup("Key Binding");
    if (ImGui::BeginPopupModal("Key Binding", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTable("ShortcutTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Keys"); ImGui::TableHeadersRow();
            auto keyList = SplitByComma(kb.input->GetMapping());
            for (auto& k : keyList) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s", kb.input->label); ImGui::TableNextColumn();
                ImGui::Text("%s", k.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (kb.waitingForInput) {
            ImVec4 color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            color.w = 0.7f + 0.3f * 0.1f;
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Text("Waiting for key input. Press esc to cancel.");
            ImGui::PopStyleColor();
            kb.processKeyCount++;
        } else ImGui::NewLine();
        ImGui::Spacing();
        if (kb.waitingForInput) ImGui::BeginDisabled();
        if (ImGui::Button("Set", ImVec2(120, 0))) { kb.input->ClearMapping(); appendPressed = true; }
        if (ImGui::Button("Append", ImVec2(120, 0))) appendPressed = true;
        if (ImGui::Button("Clear", ImVec2(120, 0))) { kb.input->ClearMapping(); kb.input->StoreToConfig(&config); }
        if (ImGui::Button("Default", ImVec2(120, 0))) { SetDefaultKeyVal(kb.input); kb.input->StoreToConfig(&config); }
        if (ImGui::Button("Finish", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); finish = true; }
        if (kb.waitingForInput) ImGui::EndDisabled();
        ImGui::EndPopup();
        if (kb.processKeyCount > 2) {
            auto system = kb.input->GetInputSystem();
            kb.input->Configure(true);
            kb.input->StoreToConfig(&config);
            kb.processKeyCount = 0; kb.waitingForInput = false;
        }
        if (appendPressed) kb.waitingForInput = true;
        if (finish) kb.Reset();
    }
}

[[maybe_unused]] static void AddKeys(Util::Config::Node& config, KeyBindState& kb, std::vector<std::shared_ptr<CInput>> keyInputs)
{
    bool openPopup = false;
    for (auto& k : keyInputs) {
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", k->GetInputGroup()); ImGui::TableSetColumnIndex(1);
        if (ImGui::Selectable(k->label, false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {
            if (ImGui::IsMouseDoubleClicked(0)) { kb.input = k; openPopup = true; }
        }
        ImGui::TableSetColumnIndex(2); ImGui::Text("%s", k->GetMapping());
    }
    BindKeys(config, kb, openPopup);
}

static void DrawButtonOptions(Util::Config::Node& config, int selectedGameIndex, bool& exit, bool& saveSettings)
{
    if (ImGui::Button("Load game")) {
        if (selectedGameIndex < 0) ImGui::OpenPopup("Load game");
        else exit = true;
    }
    if (ImGui::BeginPopupModal("Load game", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("No game selected"); ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Defaults")) ImGui::OpenPopup("Confirm Load");
    if (ImGui::BeginPopupModal("Confirm Load", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to load defaults?"); ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(120, 0))) { config = LibretroConfigProvider::DefaultConfig(LibretroWrapper::s_gameXMLFilePath); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit")) ImGui::OpenPopup("Save On Exit");
    if (ImGui::BeginPopupModal("Save On Exit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save settings upon exit?"); ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(120, 0))) { selectedGameIndex = -1; saveSettings = true; exit = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) { selectedGameIndex = -1; saveSettings = false; exit = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

[[maybe_unused]] static Game GetGame(const std::map<std::string, Game>& games, int selectedGameIndex)
{
    Game game;
    if (selectedGameIndex >= 0) {
        int index = 0;
        for (const auto& g : games) {
            if (index == selectedGameIndex) { game = g.second; break; }
            index++;
        }
    }
    return game;
}

[[maybe_unused]] static void GUI(const ImGuiIO& /*io*/, Util::Config::Node& config, const std::map<std::string, Game>& games, int& selectedGameIndex, bool& exit, bool& saveSettings, std::shared_ptr<CInputs>& /*inputs*/, KeyBindState& /*kb*/)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::Begin("SuperSetup", nullptr, ImGuiWindowFlags_NoTitleBar);

    if (ImGui::BeginChild("TableRegion", ImVec2(0.0f, 200.0f), true)) {
        if (ImGui::BeginTable("Games", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Title"); ImGui::TableSetupColumn("Rom Name");
            ImGui::TableHeadersRow();
            int row = 0;
            for (const auto& g : games) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s", g.second.title.c_str()); ImGui::TableNextColumn();
                if (ImGui::Selectable(g.second.name.c_str(), selectedGameIndex == row, ImGuiSelectableFlags_SpanAllColumns)) selectedGameIndex = row;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) exit = true;
                row++;
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    DrawButtonOptions(config, selectedGameIndex, exit, saveSettings);
    ImGui::Dummy(ImVec2(0.0f, 20.0f));

    if (ImGui::BeginTabBar("Tabs")) {
        const char* tabs[] = {"Core", "Video", "Sound", "Network", "Misc", "ForceFeedback", "Sensitivity"};
        for (auto tab : tabs) {
            if (ImGui::BeginTabItem(tab)) {
                UpdateTempValues(config, (std::string(tab) == "Sound" ? "Sound" : tab), true);
                CreateControls(config, (std::string(tab) == "Sound" ? "Sound" : tab));
                UpdateTempValues(config, (std::string(tab) == "Sound" ? "Sound" : tab), false);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

std::vector<std::string> RunGUI(const std::string& configPath, Util::Config::Node& config)
{
    // [FIX] ImGui Context should only be created once
    static bool s_imguiInitialized = false;
    
    if (!s_imguiInitialized) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplOpenGL3_Init(Graphics::GLSLVersion::GetImGui().c_str());
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        s_imguiInitialized = true;
    }

    // 1. Determine the XML filename
    std::string xmlName = "Games.xml";
    for (const auto& n : config) {
        if (n.Key() == "GameXMLFile") { xmlName = n.ValueAs<std::string>(); break; }
    }

    // 2. Construct absolute path
    fs::path fullXmlPath = fs::path(configPath) / xmlName;

    // 3. Safety Check: Verify file exists before loading
    if (!fs::exists(fullXmlPath)) {
        // Log to terminal for the developer/user
        fprintf(stderr, "[Supermodel GUI] ERROR: %s not found!\n", fullXmlPath.c_str());
        fprintf(stderr, "[Supermodel GUI] Please ensure assets are in: %s\n", configPath.c_str());
        
        // Return early to avoid GameLoader crash
        return {}; 
    }

    // In Libretro, you cannot run a 'while' loop here because it blocks the frontend.
    // This function should probably be split into 'Init' and 'Draw' phases.
    // For now, returning empty to satisfy the compiler and avoid blocking.

    return {};
}

// ---------------------------------------------------------------------------
// In-game timing overlay
// ---------------------------------------------------------------------------

static bool s_overlayInitialized = false;

void Libretro_InitOverlay(const char* glslVersion)
{
    if (s_overlayInitialized) return;
    if (!ImGui::GetCurrentContext())
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
    }
    ImGui_ImplOpenGL3_Init(glslVersion);
    s_overlayInitialized = true;
}

void Libretro_ShutdownOverlay()
{
    if (!s_overlayInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    s_overlayInitialized = false;
}

void Libretro_DrawTimingOverlay(const FrameTimings& t,
                                const LibretroFrontendTimings& frontend,
                                int displayW, int displayH,
                                double framesPerSecond, float gpuMs)
{
    if (!s_overlayInitialized) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)displayW, (float)displayH);
    io.DeltaTime   = 1.0f / static_cast<float>(framesPerSecond);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    const float PAD = 8.0f;
    ImVec2 pos(PAD, PAD);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Always);
    ImGui::Begin("##timings", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Colour-code each row: green < threshold, yellow < 2×, red >= 2×
    auto coloured = [](UINT32 ms, UINT32 warn, UINT32 crit) {
        if      (ms >= crit) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.3f,0.3f,1));
        else if (ms >= warn) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,0,1));
        else                 ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,1,0.4f,1));
    };

    ImGui::Text("Frame  timing (ms)");
    ImGui::Separator();

    coloured(t.ppcTicks,    20, 34); ImGui::Text("PPC    : %3u ms", t.ppcTicks);    ImGui::PopStyleColor();
    coloured(t.renderTicks, 20, 34); ImGui::Text("Render : %3u ms", t.renderTicks); ImGui::PopStyleColor();
    coloured((UINT32)gpuMs, 20, 34); ImGui::Text("GPU    : %5.1f ms", gpuMs);       ImGui::PopStyleColor();
    coloured(t.syncTicks,    2,  5); ImGui::Text("Sync   : %3u ms", t.syncTicks);   ImGui::PopStyleColor();
    coloured(t.sndTicks,    10, 20); ImGui::Text("Sound  : %3u ms", t.sndTicks);    ImGui::PopStyleColor();
    coloured(t.frameTicks,  20, 34); ImGui::Text("Total  : %3u ms", t.frameTicks);  ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::Text("61-frame averages");
    ImGui::Text("Engine       : %5.1f ms", frontend.engineMs);
    ImGui::Text("Audio/pacing : %5.1f ms", frontend.audioSubmitMs);
    ImGui::Text("Overlay      : %5.1f ms", frontend.overlayMs);
    ImGui::Text("Blit         : %5.1f ms", frontend.blitMs);
    ImGui::Text("Other        : %5.1f ms", frontend.otherMs);
    ImGui::Text("Core+blit    : %5.1f ms", frontend.coreAndBlitMs);
    ImGui::Text("Present      : %5.1f ms", frontend.presentMs);
    ImGui::Text("retro_run    : %5.1f ms", frontend.retroRunMs);
    ImGui::Text("Worst        : %5.1f ms", frontend.worstRetroRunMs);
    ImGui::Text("Actual       : %5.1f FPS", frontend.actualFps);
    ImGui::Text("Engine cap   : %5.1f FPS",
                frontend.engineMs > 0.0f ? 1000.0f / frontend.engineMs : 0.0f);
    ImGui::Text("Callback cap : %5.1f FPS",
                frontend.retroRunMs > 0.0f ? 1000.0f / frontend.retroRunMs : 0.0f);

    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
