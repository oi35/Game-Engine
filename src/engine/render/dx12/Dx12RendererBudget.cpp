// File: Dx12RendererBudget.cpp
// Purpose: Implements pass-budget profiles, auto-tiering, and visibility budget controls.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

bool Dx12Renderer::Impl::loadPassBudgetConfig(bool verbose) {
        // Read legacy cfg format and map values into runtime budget profile state.
        ensureBudgetProfiles();
        std::ifstream input(passBudgetConfigPath);
        if (!input.is_open()) {
            applyBudgetProfile(activeBudgetProfileIndex, false);
            if (verbose) {
                logPassBudgets("Config not found, using defaults");
            }
            return false;
        }

        // Parse both legacy single-profile keys and profile.<scene>.<quality> keys.
        PassBudgetConfig legacyBudgets = passBudgetConfig;
        bool parsedLegacyValue = false;
        bool parsedProfileValue = false;
        bool parsedAutoMode = false;
        bool parsedVisibilityBudgetEnabled = false;
        bool parsedHzbEnabled = false;
        bool parsedComputeDispatchEnabled = false;
        bool parsedTierBinding = false;
        bool parsedTierCurrent = false;
        bool configAutoModeValue = budgetAutoModeEnabled;
        bool configVisibilityBudgetEnabled = visibilityBudgetEnabled;
        bool configHzbEnabled = hzbOcclusionEnabled;
        bool configComputeDispatchEnabled = computeDispatchPrototypeEnabled;
        ComplexityTier configTierCurrent = budgetTierCurrent;
        auto configTierBindings = budgetTierBindings;
        std::string activeScene;
        std::string activeQuality;
        std::string rawLine;
        std::size_t lineNumber = 0;
        while (std::getline(input, rawLine)) {
            lineNumber += 1;
            std::string line = trimText(rawLine);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            const std::string key = trimText(line.substr(0, eq));
            const std::string valueText = trimText(line.substr(eq + 1));
            if (key.empty() || valueText.empty()) {
                continue;
            }

            if (key == "active_scene" || key.ends_with("active_scene")) {
                activeScene = valueText;
                continue;
            }
            if (key == "active_quality" || key.ends_with("active_quality")) {
                activeQuality = valueText;
                continue;
            }
            if (key == "auto_mode") {
                bool parsedValue = false;
                if (!parseBoolText(valueText, parsedValue)) {
                    std::cout << "[BudgetAuto] Ignoring invalid auto_mode value at line " << lineNumber
                              << " value=" << valueText << '\n';
                } else {
                    configAutoModeValue = parsedValue;
                    parsedAutoMode = true;
                }
                continue;
            }
            if (key == "visibility_budget_enabled") {
                bool parsedValue = false;
                if (!parseBoolText(valueText, parsedValue)) {
                    std::cout << "[VisibilityBudget] Ignoring invalid visibility_budget_enabled value at line "
                              << lineNumber << " value=" << valueText << '\n';
                } else {
                    configVisibilityBudgetEnabled = parsedValue;
                    parsedVisibilityBudgetEnabled = true;
                }
                continue;
            }
            if (key == "hzb_enabled") {
                bool parsedValue = false;
                if (!parseBoolText(valueText, parsedValue)) {
                    std::cout << "[HZB] Ignoring invalid hzb_enabled value at line "
                              << lineNumber << " value=" << valueText << '\n';
                } else {
                    configHzbEnabled = parsedValue;
                    parsedHzbEnabled = true;
                }
                continue;
            }
            if (key == "compute_dispatch_enabled") {
                bool parsedValue = false;
                if (!parseBoolText(valueText, parsedValue)) {
                    std::cout << "[DispatchProto] Ignoring invalid compute_dispatch_enabled value at line "
                              << lineNumber << " value=" << valueText << '\n';
                } else {
                    configComputeDispatchEnabled = parsedValue;
                    parsedComputeDispatchEnabled = true;
                }
                continue;
            }
            if (key == "active_tier") {
                const std::string tierText = toLowerAscii(valueText);
                if (tierText == "low") {
                    configTierCurrent = ComplexityTier::Low;
                    parsedTierCurrent = true;
                } else if (tierText == "medium") {
                    configTierCurrent = ComplexityTier::Medium;
                    parsedTierCurrent = true;
                } else if (tierText == "high") {
                    configTierCurrent = ComplexityTier::High;
                    parsedTierCurrent = true;
                } else {
                    std::cout << "[BudgetAuto] Ignoring invalid active_tier at line " << lineNumber
                              << " value=" << valueText << '\n';
                }
                continue;
            }
            if (key == "tier_low_scene") {
                configTierBindings[complexityTierIndex(ComplexityTier::Low)].scene = valueText;
                parsedTierBinding = true;
                continue;
            }
            if (key == "tier_low_quality") {
                configTierBindings[complexityTierIndex(ComplexityTier::Low)].quality = valueText;
                parsedTierBinding = true;
                continue;
            }
            if (key == "tier_medium_scene") {
                configTierBindings[complexityTierIndex(ComplexityTier::Medium)].scene = valueText;
                parsedTierBinding = true;
                continue;
            }
            if (key == "tier_medium_quality") {
                configTierBindings[complexityTierIndex(ComplexityTier::Medium)].quality = valueText;
                parsedTierBinding = true;
                continue;
            }
            if (key == "tier_high_scene") {
                configTierBindings[complexityTierIndex(ComplexityTier::High)].scene = valueText;
                parsedTierBinding = true;
                continue;
            }
            if (key == "tier_high_quality") {
                configTierBindings[complexityTierIndex(ComplexityTier::High)].quality = valueText;
                parsedTierBinding = true;
                continue;
            }

            const bool expectsNumericValue =
                key == "total_ms" ||
                key == "shadow_ms" ||
                key == "main_ms" ||
                key == "debug_ms" ||
                key.rfind("profile.", 0) == 0;
            if (!expectsNumericValue) {
                continue;
            }

            double value = 0.0;
            try {
                value = std::stod(valueText);
            } catch (...) {
                std::cout << "[Budget] Ignoring invalid value at line " << lineNumber
                          << " key=" << key << " value=" << valueText << '\n';
                continue;
            }

            if (key == "total_ms") {
                legacyBudgets.totalMs = value;
                parsedLegacyValue = true;
                continue;
            }
            if (key == "shadow_ms") {
                legacyBudgets.shadowMs = value;
                parsedLegacyValue = true;
                continue;
            }
            if (key == "main_ms") {
                legacyBudgets.mainMs = value;
                parsedLegacyValue = true;
                continue;
            }
            if (key == "debug_ms") {
                legacyBudgets.debugMs = value;
                parsedLegacyValue = true;
                continue;
            }

            if (key.rfind("profile.", 0) == 0) {
                const std::vector<std::string> parts = splitByDelimiter(key, '.');
                if (parts.size() != 4 || parts[0] != "profile") {
                    continue;
                }

                const std::string& sceneName = parts[1];
                const std::string& qualityName = parts[2];
                const std::string& metricName = parts[3];
                if (sceneName.empty() || qualityName.empty()) {
                    continue;
                }

                const std::size_t profileIndex = upsertBudgetProfile(sceneName, qualityName);
                PassBudgetConfig& profileBudgets = budgetProfiles[profileIndex].budgets;
                if (metricName == "total_ms") {
                    profileBudgets.totalMs = value;
                    parsedProfileValue = true;
                } else if (metricName == "shadow_ms") {
                    profileBudgets.shadowMs = value;
                    parsedProfileValue = true;
                } else if (metricName == "main_ms") {
                    profileBudgets.mainMs = value;
                    parsedProfileValue = true;
                } else if (metricName == "debug_ms") {
                    profileBudgets.debugMs = value;
                    parsedProfileValue = true;
                }
            }
        }

        for (auto& profile : budgetProfiles) {
            profile.budgets.totalMs = std::clamp(profile.budgets.totalMs, 0.05, 100.0);
            profile.budgets.shadowMs = std::clamp(profile.budgets.shadowMs, 0.05, 100.0);
            profile.budgets.mainMs = std::clamp(profile.budgets.mainMs, 0.05, 100.0);
            profile.budgets.debugMs = std::clamp(profile.budgets.debugMs, 0.05, 100.0);
        }

        bool appliedProfile = false;
        if (!activeScene.empty() && !activeQuality.empty()) {
            const std::size_t requested = findBudgetProfileIndex(activeScene, activeQuality);
            if (requested != static_cast<std::size_t>(-1)) {
                applyBudgetProfile(requested, false);
                appliedProfile = true;
            }
        }

        if (!appliedProfile) {
            applyBudgetProfile(activeBudgetProfileIndex, false);
        }

        if (parsedLegacyValue && !parsedProfileValue && !appliedProfile) {
            passBudgetConfig = legacyBudgets;
            clampPassBudgets();
        }

        if (parsedTierCurrent) {
            budgetTierCurrent = configTierCurrent;
        }
        if (parsedTierBinding) {
            budgetTierBindings = std::move(configTierBindings);
        }
        if (parsedAutoMode) {
            budgetAutoModeEnabled = configAutoModeValue;
        }
        if (parsedVisibilityBudgetEnabled) {
            visibilityBudgetEnabled = configVisibilityBudgetEnabled;
        }
        if (parsedHzbEnabled) {
            hzbOcclusionEnabled = configHzbEnabled;
        }
        if (parsedComputeDispatchEnabled) {
            computeDispatchPrototypeEnabled = configComputeDispatchEnabled;
        }

        validateBudgetProfiles(false);

        if (verbose) {
            if (parsedProfileValue || appliedProfile) {
                logPassBudgets("Loaded profile budgets from config");
            } else if (parsedLegacyValue) {
                logPassBudgets("Loaded legacy budgets from config");
            } else {
                logPassBudgets("Config parsed without recognized keys, keeping current budgets");
            }
            std::cout << "[BudgetAuto] Config mode=" << (budgetAutoModeEnabled ? "AUTO" : "MANUAL")
                      << " tier=" << complexityTierName(budgetTierCurrent)
                      << " bindings="
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].scene << "/"
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].quality << ","
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].scene << "/"
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].quality << ","
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::High)].scene << "/"
                      << budgetTierBindings[complexityTierIndex(ComplexityTier::High)].quality
                      << '\n';
            std::cout << "[VisibilityBudget] Config mode="
                      << (visibilityBudgetEnabled ? "ENABLED" : "DISABLED")
                      << '\n';
            std::cout << "[HZB] Config mode="
                      << (hzbOcclusionEnabled ? "ENABLED" : "DISABLED")
                      << " tiles=" << kOcclusionTilesX << "x" << kOcclusionTilesY
                      << " levels<=" << kHzbMaxLevels
                      << '\n';
            std::cout << "[DispatchProto] Config mode="
                      << (computeDispatchPrototypeEnabled ? "ENABLED" : "DISABLED")
                      << " groups(F/O)=" << computeFrustumDispatchGroupSize << "/" << computeOcclusionDispatchGroupSize
                      << " indirectMain=" << (indirectMainDrawReady ? "yes" : "no")
                      << " frustumGpu=" << (gpuFrustumCullingReady ? "yes" : "no")
                      << " occlusionGpu=" << (gpuOcclusionCullingReady ? "yes" : "no")
                      << '\n';
        }

        return parsedLegacyValue || parsedProfileValue || appliedProfile ||
               parsedAutoMode || parsedVisibilityBudgetEnabled || parsedHzbEnabled || parsedComputeDispatchEnabled ||
               parsedTierBinding || parsedTierCurrent;
    }

bool Dx12Renderer::Impl::savePassBudgetConfig(bool verbose) {
        // Persist current profile state in cfg format for runtime reload and tooling.
        ensureBudgetProfiles();
        clampPassBudgets();
        if (!budgetProfiles.empty() && activeBudgetProfileIndex < budgetProfiles.size()) {
            budgetProfiles[activeBudgetProfileIndex].budgets = passBudgetConfig;
        }

        try {
            const std::filesystem::path parentPath = passBudgetConfigPath.parent_path();
            if (!parentPath.empty()) {
                std::filesystem::create_directories(parentPath);
            }
        } catch (const std::exception& ex) {
            std::cout << "[Budget] Failed to create config directory: " << ex.what()
                      << " path=" << passBudgetConfigPath.string() << '\n';
            return false;
        }

        std::ofstream output(passBudgetConfigPath, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            std::cout << "[Budget] Failed to write config file: " << passBudgetConfigPath.string() << '\n';
            return false;
        }

        output << "# Runtime pass budget configuration (milliseconds).\n"
               << "# Supported keys:\n"
               << "#   active_scene / active_quality\n"
               << "#   auto_mode (0/1)\n"
               << "#   visibility_budget_enabled (0/1)\n"
               << "#   hzb_enabled (0/1)\n"
               << "#   compute_dispatch_enabled (0/1)\n"
               << "#   active_tier (low/medium/high)\n"
               << "#   tier_low_scene / tier_low_quality\n"
               << "#   tier_medium_scene / tier_medium_quality\n"
               << "#   tier_high_scene / tier_high_quality\n"
               << "#   total_ms / shadow_ms / main_ms / debug_ms (legacy fallback)\n"
               << "#   profile.<scene>.<quality>.total_ms\n"
               << "#   profile.<scene>.<quality>.shadow_ms\n"
               << "#   profile.<scene>.<quality>.main_ms\n"
               << "#   profile.<scene>.<quality>.debug_ms\n"
               << "#\n"
               << "# During runtime:\n"
               << "#   F5 = cycle target (Total/Shadow/Main/Debug)\n"
               << "#   F6 = decrease selected budget (hold Shift for larger step, auto-save)\n"
               << "#   F7 = increase selected budget (hold Shift for larger step, auto-save)\n"
               << "#   F8 = reload this file\n"
               << "#   F9 = cycle quality preset in current scene\n"
               << "#   F10 = cycle scene preset for current quality\n"
               << "#   F11 = export JSON profiles to assets/config/pass_budgets.json\n"
               << "#   F12 = import JSON profiles from assets/config/pass_budgets.json\n"
               << "#   B = toggle auto profile mode (tier-based)\n"
               << "#   V = toggle visibility budget mode (occlusion-aware culling)\n"
               << "#   H = toggle hierarchical-Z occlusion path\n"
               << "#   C = toggle compute-dispatch path (GPU frustum pass + grouped prototype stages)\n"
               << '\n';

        output << std::fixed << std::setprecision(2);
        const BudgetProfile* activeProfile = activeBudgetProfile();
        if (activeProfile != nullptr) {
            output << "active_scene=" << activeProfile->scene << '\n';
            output << "active_quality=" << activeProfile->quality << '\n';
        } else {
            output << "active_scene=sample\n";
            output << "active_quality=balanced\n";
        }
        output << "auto_mode=" << (budgetAutoModeEnabled ? 1 : 0) << '\n';
        output << "visibility_budget_enabled=" << (visibilityBudgetEnabled ? 1 : 0) << '\n';
        output << "hzb_enabled=" << (hzbOcclusionEnabled ? 1 : 0) << '\n';
        output << "compute_dispatch_enabled=" << (computeDispatchPrototypeEnabled ? 1 : 0) << '\n';
        output << "active_tier=" << toLowerAscii(complexityTierName(budgetTierCurrent)) << '\n';
        output << "tier_low_scene=" << budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].scene << '\n';
        output << "tier_low_quality=" << budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].quality << '\n';
        output << "tier_medium_scene=" << budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].scene << '\n';
        output << "tier_medium_quality=" << budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].quality << '\n';
        output << "tier_high_scene=" << budgetTierBindings[complexityTierIndex(ComplexityTier::High)].scene << '\n';
        output << "tier_high_quality=" << budgetTierBindings[complexityTierIndex(ComplexityTier::High)].quality << '\n';
        output << '\n';

        // Legacy single-profile keys remain for backward compatibility.
        output << "total_ms=" << passBudgetConfig.totalMs << '\n';
        output << "shadow_ms=" << passBudgetConfig.shadowMs << '\n';
        output << "main_ms=" << passBudgetConfig.mainMs << '\n';
        output << "debug_ms=" << passBudgetConfig.debugMs << '\n';
        output << '\n';

        for (const auto& profile : budgetProfiles) {
            output << "profile." << profile.scene << "." << profile.quality << ".total_ms=" << profile.budgets.totalMs << '\n';
            output << "profile." << profile.scene << "." << profile.quality << ".shadow_ms=" << profile.budgets.shadowMs << '\n';
            output << "profile." << profile.scene << "." << profile.quality << ".main_ms=" << profile.budgets.mainMs << '\n';
            output << "profile." << profile.scene << "." << profile.quality << ".debug_ms=" << profile.budgets.debugMs << '\n';
        }

        output.flush();
        if (!output.good()) {
            std::cout << "[Budget] Failed to flush config file: " << passBudgetConfigPath.string() << '\n';
            return false;
        }

        if (verbose) {
            logPassBudgets("Saved budgets to config");
        }
        return true;
    }

bool Dx12Renderer::Impl::exportBudgetProfilesJson(bool verbose) {
        // Export normalized profile state for validation/diff tooling.
        ensureBudgetProfiles();
        validateBudgetProfiles(false);
        sanitizeTierBindings(false);

        try {
            const std::filesystem::path parentPath = passBudgetJsonPath.parent_path();
            if (!parentPath.empty()) {
                std::filesystem::create_directories(parentPath);
            }
        } catch (const std::exception& ex) {
            std::cout << "[Budget] Failed to prepare JSON directory: " << ex.what()
                      << " path=" << passBudgetJsonPath.string() << '\n';
            return false;
        }

        std::ofstream output(passBudgetJsonPath, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            std::cout << "[Budget] Failed to open JSON config for write: " << passBudgetJsonPath.string() << '\n';
            return false;
        }

        const BudgetProfile* active = activeBudgetProfile();
        const std::string activeScene = active != nullptr ? active->scene : "sample";
        const std::string activeQuality = active != nullptr ? active->quality : "balanced";

        output << "{\n";
        output << "  \"schema_version\": " << kBudgetJsonSchemaVersionCurrent << ",\n";
        output << "  \"active_scene\": \"" << escapeJsonString(activeScene) << "\",\n";
        output << "  \"active_quality\": \"" << escapeJsonString(activeQuality) << "\",\n";
        output << "  \"auto_mode\": " << (budgetAutoModeEnabled ? 1 : 0) << ",\n";
        output << "  \"visibility_budget_enabled\": " << (visibilityBudgetEnabled ? 1 : 0) << ",\n";
        output << "  \"hzb_enabled\": " << (hzbOcclusionEnabled ? 1 : 0) << ",\n";
        output << "  \"compute_dispatch_enabled\": " << (computeDispatchPrototypeEnabled ? 1 : 0) << ",\n";
        output << "  \"active_tier\": \"" << escapeJsonString(toLowerAscii(complexityTierName(budgetTierCurrent))) << "\",\n";
        output << "  \"tier_low_scene\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].scene) << "\",\n";
        output << "  \"tier_low_quality\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::Low)].quality) << "\",\n";
        output << "  \"tier_medium_scene\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].scene) << "\",\n";
        output << "  \"tier_medium_quality\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::Medium)].quality) << "\",\n";
        output << "  \"tier_high_scene\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::High)].scene) << "\",\n";
        output << "  \"tier_high_quality\": \""
               << escapeJsonString(budgetTierBindings[complexityTierIndex(ComplexityTier::High)].quality) << "\",\n";
        output << "  \"profiles\": [\n";
        output << std::fixed << std::setprecision(2);
        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            const BudgetProfile& profile = budgetProfiles[i];
            output << "    {\n";
            output << "      \"scene\": \"" << escapeJsonString(profile.scene) << "\",\n";
            output << "      \"quality\": \"" << escapeJsonString(profile.quality) << "\",\n";
            output << "      \"total_ms\": " << profile.budgets.totalMs << ",\n";
            output << "      \"shadow_ms\": " << profile.budgets.shadowMs << ",\n";
            output << "      \"main_ms\": " << profile.budgets.mainMs << ",\n";
            output << "      \"debug_ms\": " << profile.budgets.debugMs << '\n';
            output << "    }" << (i + 1 < budgetProfiles.size() ? "," : "") << '\n';
        }
        output << "  ]\n";
        output << "}\n";
        output.flush();

        if (!output.good()) {
            std::cout << "[Budget] Failed to flush JSON config: " << passBudgetJsonPath.string() << '\n';
            return false;
        }

        if (verbose) {
            std::cout << "[Budget] Exported profiles to JSON path=" << passBudgetJsonPath.string()
                      << " count=" << budgetProfiles.size()
                      << " schema=v" << kBudgetJsonSchemaVersionCurrent
                      << '\n';
        }
        return true;
    }

bool Dx12Renderer::Impl::importBudgetProfilesJson(bool verbose) {
        // Import current or legacy schema JSON and migrate into runtime state.
        ensureBudgetProfiles();

        std::ifstream input(passBudgetJsonPath, std::ios::in);
        if (!input.is_open()) {
            std::cout << "[Budget] JSON profile file not found: " << passBudgetJsonPath.string() << '\n';
            return false;
        }

        const std::string jsonText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        int schemaVersion = 0;
        const bool hasSchemaVersion = extractJsonIntegerField(jsonText, "schema_version", schemaVersion);
        if (!hasSchemaVersion) {
            if (verbose) {
                std::cout << "[Budget] JSON schema_version missing, treating as legacy version 0\n";
            }
            schemaVersion = 0;
        }

        if (schemaVersion < 0) {
            std::cout << "[Budget] JSON import failed: invalid schema_version=" << schemaVersion << '\n';
            return false;
        }
        if (schemaVersion > kBudgetJsonSchemaVersionCurrent) {
            std::cout << "[Budget] JSON import failed: schema_version=" << schemaVersion
                      << " is newer than supported=" << kBudgetJsonSchemaVersionCurrent << '\n';
            return false;
        }

        std::string activeScene;
        std::string activeQuality;
        (void)extractJsonStringField(jsonText, "active_scene", activeScene);
        (void)extractJsonStringField(jsonText, "active_quality", activeQuality);
        if (schemaVersion == 0) {
            (void)extractJsonStringField(jsonText, "selected_scene", activeScene);
            (void)extractJsonStringField(jsonText, "selected_quality", activeQuality);
        }

        bool importedAutoMode = budgetAutoModeEnabled;
        bool parsedAutoMode = false;
        if (extractJsonBoolField(jsonText, "auto_mode", importedAutoMode)) {
            parsedAutoMode = true;
        }

        bool importedVisibilityBudgetEnabled = visibilityBudgetEnabled;
        bool parsedVisibilityBudgetEnabled = false;
        if (extractJsonBoolField(jsonText, "visibility_budget_enabled", importedVisibilityBudgetEnabled)) {
            parsedVisibilityBudgetEnabled = true;
        }

        bool importedHzbEnabled = hzbOcclusionEnabled;
        bool parsedHzbEnabled = false;
        if (extractJsonBoolField(jsonText, "hzb_enabled", importedHzbEnabled)) {
            parsedHzbEnabled = true;
        }

        bool importedComputeDispatchEnabled = computeDispatchPrototypeEnabled;
        bool parsedComputeDispatchEnabled = false;
        if (extractJsonBoolField(jsonText, "compute_dispatch_enabled", importedComputeDispatchEnabled)) {
            parsedComputeDispatchEnabled = true;
        }

        ComplexityTier importedTierCurrent = budgetTierCurrent;
        bool parsedTierCurrent = false;
        std::string activeTierText;
        if (extractJsonStringField(jsonText, "active_tier", activeTierText)) {
            const std::string normalizedTier = toLowerAscii(activeTierText);
            if (normalizedTier == "low") {
                importedTierCurrent = ComplexityTier::Low;
                parsedTierCurrent = true;
            } else if (normalizedTier == "medium") {
                importedTierCurrent = ComplexityTier::Medium;
                parsedTierCurrent = true;
            } else if (normalizedTier == "high") {
                importedTierCurrent = ComplexityTier::High;
                parsedTierCurrent = true;
            }
        }

        auto importedTierBindings = budgetTierBindings;
        bool parsedTierBindings = false;
        std::string tierValue;
        if (extractJsonStringField(jsonText, "tier_low_scene", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::Low)].scene = tierValue;
            parsedTierBindings = true;
        }
        if (extractJsonStringField(jsonText, "tier_low_quality", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::Low)].quality = tierValue;
            parsedTierBindings = true;
        }
        if (extractJsonStringField(jsonText, "tier_medium_scene", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::Medium)].scene = tierValue;
            parsedTierBindings = true;
        }
        if (extractJsonStringField(jsonText, "tier_medium_quality", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::Medium)].quality = tierValue;
            parsedTierBindings = true;
        }
        if (extractJsonStringField(jsonText, "tier_high_scene", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::High)].scene = tierValue;
            parsedTierBindings = true;
        }
        if (extractJsonStringField(jsonText, "tier_high_quality", tierValue)) {
            importedTierBindings[complexityTierIndex(ComplexityTier::High)].quality = tierValue;
            parsedTierBindings = true;
        }

        const std::string profileArrayText = extractJsonProfilesArray(jsonText);
        if (profileArrayText.empty()) {
            std::cout << "[Budget] JSON import failed: missing profiles array\n";
            return false;
        }

        std::vector<BudgetProfile> importedProfiles;
        const std::regex objectRegex("\\{[^\\{\\}]*\\}");
        for (std::sregex_iterator it(profileArrayText.begin(), profileArrayText.end(), objectRegex), end; it != end; ++it) {
            const std::string objectText = it->str();
            BudgetProfile profile{};
            double total = 0.0;
            double shadow = 0.0;
            double main = 0.0;
            double debug = 0.0;

            const bool parsedScene =
                extractJsonStringField(objectText, "scene", profile.scene) ||
                (schemaVersion == 0 && extractJsonStringField(objectText, "scene_name", profile.scene));
            const bool parsedQuality =
                extractJsonStringField(objectText, "quality", profile.quality) ||
                (schemaVersion == 0 && extractJsonStringField(objectText, "quality_name", profile.quality));
            const bool parsedTotal =
                extractJsonNumberField(objectText, "total_ms", total) ||
                (schemaVersion == 0 && extractJsonNumberField(objectText, "total", total));
            const bool parsedShadow =
                extractJsonNumberField(objectText, "shadow_ms", shadow) ||
                (schemaVersion == 0 && extractJsonNumberField(objectText, "shadow", shadow));
            const bool parsedMain =
                extractJsonNumberField(objectText, "main_ms", main) ||
                (schemaVersion == 0 && extractJsonNumberField(objectText, "main", main));
            const bool parsedDebug =
                extractJsonNumberField(objectText, "debug_ms", debug) ||
                (schemaVersion == 0 && extractJsonNumberField(objectText, "debug", debug));
            const bool parsed = parsedScene && parsedQuality && parsedTotal && parsedShadow && parsedMain && parsedDebug;
            if (!parsed) {
                if (verbose) {
                    std::cout << "[Budget] Skipping malformed profile object during JSON import\n";
                }
                continue;
            }

            profile.budgets.totalMs = total;
            profile.budgets.shadowMs = shadow;
            profile.budgets.mainMs = main;
            profile.budgets.debugMs = debug;
            importedProfiles.push_back(profile);
        }

        if (importedProfiles.empty()) {
            std::cout << "[Budget] JSON import failed: no valid profile objects\n";
            return false;
        }

        budgetProfiles = std::move(importedProfiles);
        activeBudgetProfileIndex = 0;
        if (!activeScene.empty() && !activeQuality.empty()) {
            const std::size_t found = findBudgetProfileIndex(activeScene, activeQuality);
            if (found != static_cast<std::size_t>(-1)) {
                activeBudgetProfileIndex = found;
            }
        }

        if (parsedAutoMode) {
            budgetAutoModeEnabled = importedAutoMode;
        }
        if (parsedVisibilityBudgetEnabled) {
            visibilityBudgetEnabled = importedVisibilityBudgetEnabled;
        }
        if (parsedHzbEnabled) {
            hzbOcclusionEnabled = importedHzbEnabled;
        }
        if (parsedComputeDispatchEnabled) {
            computeDispatchPrototypeEnabled = importedComputeDispatchEnabled;
        }
        if (parsedTierCurrent) {
            budgetTierCurrent = importedTierCurrent;
        }
        if (parsedTierBindings) {
            budgetTierBindings = std::move(importedTierBindings);
        }

        validateBudgetProfiles(verbose);
        applyBudgetProfile(activeBudgetProfileIndex, false);
        savePassBudgetConfig(false);
        if (schemaVersion < kBudgetJsonSchemaVersionCurrent) {
            if (!exportBudgetProfilesJson(false)) {
                std::cout << "[Budget] Migration warning: imported legacy JSON but failed to export upgraded version\n";
            } else if (verbose) {
                std::cout << "[Budget] Migrated JSON schema from v" << schemaVersion
                          << " to v" << kBudgetJsonSchemaVersionCurrent << '\n';
            }
        }

        if (verbose) {
            std::cout << "[Budget] Imported profiles from JSON path=" << passBudgetJsonPath.string()
                      << " count=" << budgetProfiles.size()
                      << " active=" << activeBudgetProfileLabel()
                      << " auto=" << (budgetAutoModeEnabled ? "on" : "off")
                      << " visibilityBudget=" << (visibilityBudgetEnabled ? "on" : "off")
                      << " hzb=" << (hzbOcclusionEnabled ? "on" : "off")
                      << " dispatchProto=" << (computeDispatchPrototypeEnabled ? "on" : "off")
                      << " tier=" << complexityTierName(budgetTierCurrent)
                      << " schema=v" << schemaVersion
                      << '\n';
        }
        return true;
    }

void Dx12Renderer::Impl::cycleBudgetTarget() {
        // Cycle UI-selected pass budget target.
        const auto currentIndex = static_cast<std::uint32_t>(selectedBudgetTarget);
        const auto nextIndex = (currentIndex + 1U) % 4U;
        selectedBudgetTarget = static_cast<BudgetTarget>(nextIndex);
        logPassBudgets("Selected budget target");
    }

void Dx12Renderer::Impl::cycleBudgetQualityProfile() {
        // Keep scene fixed, rotate among quality variants.
        ensureBudgetProfiles();
        if (budgetProfiles.empty()) {
            return;
        }

        if (activeBudgetProfileIndex >= budgetProfiles.size()) {
            activeBudgetProfileIndex = 0;
        }

        const std::string currentScene = budgetProfiles[activeBudgetProfileIndex].scene;
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            if (budgetProfiles[i].scene == currentScene) {
                candidates.push_back(i);
            }
        }
        if (candidates.size() <= 1) {
            logPassBudgets("Quality profile unchanged (single option)");
            return;
        }

        std::size_t currentPosition = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i] == activeBudgetProfileIndex) {
                currentPosition = i;
                break;
            }
        }
        const std::size_t nextProfileIndex = candidates[(currentPosition + 1) % candidates.size()];
        applyBudgetProfile(nextProfileIndex, true);
        savePassBudgetConfig(false);
    }

void Dx12Renderer::Impl::cycleBudgetSceneProfile() {
        // Keep quality when possible, rotate to next scene profile.
        ensureBudgetProfiles();
        if (budgetProfiles.empty()) {
            return;
        }

        if (activeBudgetProfileIndex >= budgetProfiles.size()) {
            activeBudgetProfileIndex = 0;
        }

        const std::string currentScene = budgetProfiles[activeBudgetProfileIndex].scene;
        const std::string currentQuality = budgetProfiles[activeBudgetProfileIndex].quality;

        std::vector<std::string> sceneNames;
        for (const auto& profile : budgetProfiles) {
            if (std::find(sceneNames.begin(), sceneNames.end(), profile.scene) == sceneNames.end()) {
                sceneNames.push_back(profile.scene);
            }
        }
        if (sceneNames.size() <= 1) {
            logPassBudgets("Scene profile unchanged (single option)");
            return;
        }

        std::size_t currentScenePos = 0;
        for (std::size_t i = 0; i < sceneNames.size(); ++i) {
            if (sceneNames[i] == currentScene) {
                currentScenePos = i;
                break;
            }
        }
        const std::string& nextScene = sceneNames[(currentScenePos + 1) % sceneNames.size()];

        std::size_t nextIndex = findBudgetProfileIndex(nextScene, currentQuality);
        if (nextIndex == static_cast<std::size_t>(-1)) {
            for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
                if (budgetProfiles[i].scene == nextScene) {
                    nextIndex = i;
                    break;
                }
            }
        }
        if (nextIndex == static_cast<std::size_t>(-1)) {
            return;
        }

        applyBudgetProfile(nextIndex, true);
        savePassBudgetConfig(false);
    }

void Dx12Renderer::Impl::adjustSelectedBudget(bool increase) {
        // Runtime budget nudge with optional shift multiplier, then persist immediately.
        const double shiftMultiplier = isKeyDown(VK_SHIFT) ? 5.0 : 1.0;
        const double delta = budgetStep(selectedBudgetTarget) * shiftMultiplier * (increase ? 1.0 : -1.0);
        double& targetBudget = budgetRef(selectedBudgetTarget);
        targetBudget += delta;
        clampPassBudgets();

        std::cout << "[Budget] " << budgetTargetName(selectedBudgetTarget)
                  << (increase ? " +" : " -")
                  << (budgetStep(selectedBudgetTarget) * shiftMultiplier) << "ms"
                  << " => " << targetBudget << "ms"
                  << '\n';

        if (!savePassBudgetConfig(false)) {
            std::cout << "[Budget] Persist failed after runtime adjustment\n";
        }
    }



}  // namespace engine::render

#endif  // _WIN32
