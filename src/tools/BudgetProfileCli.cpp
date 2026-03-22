// File: BudgetProfileCli.cpp
// Purpose: CLI utility for linting and validating pass-budget profile configuration files.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kSchemaCurrent = 1;

// Severity classification for lint output lines.
enum class IssueSeverity {
    Info,
    Warning,
    Error,
};

// One lint diagnostic entry.
struct Issue {
    IssueSeverity severity = IssueSeverity::Info;
    std::string message;
};

// Per-pass millisecond budget values.
struct PassBudgetConfig {
    double totalMs = 16.67;
    double shadowMs = 4.00;
    double mainMs = 10.00;
    double debugMs = 2.00;
};

// One (scene, quality) profile record.
struct BudgetProfile {
    std::string scene;
    std::string quality;
    PassBudgetConfig budgets{};
};

// Parsed JSON document model with compatibility flags.
struct BudgetDocument {
    bool hasSchemaVersion = false;
    int schemaVersion = 0;
    std::string activeScene;
    std::string activeQuality;
    bool hasAutoMode = false;
    bool autoMode = false;
    bool hasVisibilityBudgetEnabled = false;
    bool visibilityBudgetEnabled = true;
    bool hasHzbEnabled = false;
    bool hzbEnabled = true;
    bool hasComputeDispatchEnabled = false;
    bool computeDispatchEnabled = true;
    std::string activeTier;
    std::string tierLowScene;
    std::string tierLowQuality;
    std::string tierMediumScene;
    std::string tierMediumQuality;
    std::string tierHighScene;
    std::string tierHighQuality;
    std::vector<BudgetProfile> profiles;
};

// Whitespace trimming helper used by validation and diff output.
std::string trimText(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Lowercase helper for case-insensitive keyword parsing.
std::string toLowerAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

// Read full file as text for regex-based parsing.
bool readTextFile(const std::filesystem::path& path, std::string& outText, std::string& outError) {
    std::ifstream input(path, std::ios::in);
    if (!input.is_open()) {
        outError = "Failed to open file: " + path.string();
        return false;
    }

    outText.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

// Regex extraction helpers intentionally keep parser dependencies minimal.
bool extractJsonStringField(const std::string& jsonObject, const std::string& fieldName, std::string& outValue) {
    const std::regex fieldRegex("\"" + fieldName + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
        return false;
    }
    outValue = match[1].str();
    return true;
}

bool extractJsonNumberField(const std::string& jsonObject, const std::string& fieldName, double& outValue) {
    const std::regex fieldRegex(
        "\"" + fieldName + "\"\\s*:\\s*([-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?)");
    std::smatch match;
    if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
        return false;
    }
    try {
        outValue = std::stod(match[1].str());
    } catch (...) {
        return false;
    }
    return true;
}

bool extractJsonIntegerField(const std::string& jsonObject, const std::string& fieldName, int& outValue) {
    double value = 0.0;
    if (!extractJsonNumberField(jsonObject, fieldName, value)) {
        return false;
    }
    const double rounded = std::round(value);
    if (std::abs(value - rounded) > 1e-6) {
        return false;
    }
    outValue = static_cast<int>(rounded);
    return true;
}

bool extractJsonBoolField(const std::string& jsonObject, const std::string& fieldName, bool& outValue) {
    int integerValue = 0;
    if (extractJsonIntegerField(jsonObject, fieldName, integerValue)) {
        outValue = (integerValue != 0);
        return true;
    }

    const std::regex fieldRegex(
        "\"" + fieldName + "\"\\s*:\\s*(true|false)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
        return false;
    }
    const std::string normalized = toLowerAscii(match[1].str());
    outValue = (normalized == "true");
    return true;
}

std::string extractJsonProfilesArray(const std::string& jsonText) {
    const std::string marker = "\"profiles\"";
    const std::size_t markerPos = jsonText.find(marker);
    if (markerPos == std::string::npos) {
        return {};
    }

    const std::size_t arrayStart = jsonText.find('[', markerPos);
    if (arrayStart == std::string::npos) {
        return {};
    }

    int depth = 0;
    for (std::size_t i = arrayStart; i < jsonText.size(); ++i) {
        if (jsonText[i] == '[') {
            depth += 1;
        } else if (jsonText[i] == ']') {
            depth -= 1;
            if (depth == 0) {
                const std::size_t contentStart = arrayStart + 1;
                return jsonText.substr(contentStart, i - contentStart);
            }
        }
    }
    return {};
}

// Append one issue entry.
void appendIssue(std::vector<Issue>& issues, IssueSeverity severity, const std::string& message) {
    issues.push_back({severity, message});
}

// Parse current or legacy budget JSON variants into a normalized document model.
bool parseBudgetDocument(const std::filesystem::path& path, BudgetDocument& outDoc, std::vector<Issue>& issues) {
    std::string jsonText;
    std::string fileError;
    if (!readTextFile(path, jsonText, fileError)) {
        appendIssue(issues, IssueSeverity::Error, fileError);
        return false;
    }

    outDoc = {};
    outDoc.hasSchemaVersion = extractJsonIntegerField(jsonText, "schema_version", outDoc.schemaVersion);
    (void)extractJsonStringField(jsonText, "active_scene", outDoc.activeScene);
    (void)extractJsonStringField(jsonText, "active_quality", outDoc.activeQuality);
    outDoc.hasAutoMode = extractJsonBoolField(jsonText, "auto_mode", outDoc.autoMode);
    outDoc.hasVisibilityBudgetEnabled =
        extractJsonBoolField(jsonText, "visibility_budget_enabled", outDoc.visibilityBudgetEnabled);
    outDoc.hasHzbEnabled =
        extractJsonBoolField(jsonText, "hzb_enabled", outDoc.hzbEnabled);
    outDoc.hasComputeDispatchEnabled =
        extractJsonBoolField(jsonText, "compute_dispatch_enabled", outDoc.computeDispatchEnabled);
    (void)extractJsonStringField(jsonText, "active_tier", outDoc.activeTier);
    (void)extractJsonStringField(jsonText, "tier_low_scene", outDoc.tierLowScene);
    (void)extractJsonStringField(jsonText, "tier_low_quality", outDoc.tierLowQuality);
    (void)extractJsonStringField(jsonText, "tier_medium_scene", outDoc.tierMediumScene);
    (void)extractJsonStringField(jsonText, "tier_medium_quality", outDoc.tierMediumQuality);
    (void)extractJsonStringField(jsonText, "tier_high_scene", outDoc.tierHighScene);
    (void)extractJsonStringField(jsonText, "tier_high_quality", outDoc.tierHighQuality);
    if (!outDoc.hasSchemaVersion) {
        outDoc.schemaVersion = 0;
        if (outDoc.activeScene.empty()) {
            (void)extractJsonStringField(jsonText, "selected_scene", outDoc.activeScene);
        }
        if (outDoc.activeQuality.empty()) {
            (void)extractJsonStringField(jsonText, "selected_quality", outDoc.activeQuality);
        }
    }

    const std::string profileArrayText = extractJsonProfilesArray(jsonText);
    if (profileArrayText.empty()) {
        appendIssue(issues, IssueSeverity::Error, "Missing profiles array in JSON: " + path.string());
        return false;
    }

    const std::regex objectRegex("\\{[^\\{\\}]*\\}");
    std::size_t objectCount = 0;
    for (std::sregex_iterator it(profileArrayText.begin(), profileArrayText.end(), objectRegex), end; it != end; ++it) {
        objectCount += 1;
        const std::string objectText = it->str();
        BudgetProfile profile{};
        double total = 0.0;
        double shadow = 0.0;
        double main = 0.0;
        double debug = 0.0;

        const bool parsedScene =
            extractJsonStringField(objectText, "scene", profile.scene) ||
            (outDoc.schemaVersion == 0 && extractJsonStringField(objectText, "scene_name", profile.scene));
        const bool parsedQuality =
            extractJsonStringField(objectText, "quality", profile.quality) ||
            (outDoc.schemaVersion == 0 && extractJsonStringField(objectText, "quality_name", profile.quality));
        const bool parsedTotal =
            extractJsonNumberField(objectText, "total_ms", total) ||
            (outDoc.schemaVersion == 0 && extractJsonNumberField(objectText, "total", total));
        const bool parsedShadow =
            extractJsonNumberField(objectText, "shadow_ms", shadow) ||
            (outDoc.schemaVersion == 0 && extractJsonNumberField(objectText, "shadow", shadow));
        const bool parsedMain =
            extractJsonNumberField(objectText, "main_ms", main) ||
            (outDoc.schemaVersion == 0 && extractJsonNumberField(objectText, "main", main));
        const bool parsedDebug =
            extractJsonNumberField(objectText, "debug_ms", debug) ||
            (outDoc.schemaVersion == 0 && extractJsonNumberField(objectText, "debug", debug));
        const bool parsed = parsedScene && parsedQuality && parsedTotal && parsedShadow && parsedMain && parsedDebug;

        if (!parsed) {
            appendIssue(issues, IssueSeverity::Warning, "Skipping malformed profile object index=" + std::to_string(objectCount - 1));
            continue;
        }

        profile.budgets.totalMs = total;
        profile.budgets.shadowMs = shadow;
        profile.budgets.mainMs = main;
        profile.budgets.debugMs = debug;
        outDoc.profiles.push_back(profile);
    }

    if (objectCount == 0) {
        appendIssue(issues, IssueSeverity::Error, "Profiles array contains no JSON objects");
        return false;
    }
    if (outDoc.profiles.empty()) {
        appendIssue(issues, IssueSeverity::Error, "No valid budget profiles could be parsed");
        return false;
    }
    return true;
}

// Validate required fields, ranges, uniqueness, and binding consistency.
std::vector<Issue> validateBudgetDocument(const BudgetDocument& doc, bool strictSchema) {
    std::vector<Issue> issues;

    if (!doc.hasSchemaVersion) {
        appendIssue(issues, strictSchema ? IssueSeverity::Error : IssueSeverity::Warning,
                    "schema_version missing (legacy v0 format)");
    } else if (doc.schemaVersion < 0) {
        appendIssue(issues, IssueSeverity::Error, "schema_version is negative");
    } else if (doc.schemaVersion > kSchemaCurrent) {
        appendIssue(issues, IssueSeverity::Error,
                    "schema_version=" + std::to_string(doc.schemaVersion) +
                    " is newer than supported=" + std::to_string(kSchemaCurrent));
    } else if (doc.schemaVersion < kSchemaCurrent) {
        appendIssue(issues, IssueSeverity::Warning,
                    "schema_version=" + std::to_string(doc.schemaVersion) +
                    " will be migrated to " + std::to_string(kSchemaCurrent));
    }

    if (trimText(doc.activeScene).empty()) {
        appendIssue(issues, IssueSeverity::Error, "active_scene is missing");
    }
    if (trimText(doc.activeQuality).empty()) {
        appendIssue(issues, IssueSeverity::Error, "active_quality is missing");
    }
    if (!doc.hasAutoMode) {
        appendIssue(issues, IssueSeverity::Warning, "auto_mode is missing (defaults to manual)");
    }
    if (!doc.hasVisibilityBudgetEnabled) {
        appendIssue(issues, IssueSeverity::Warning,
                    "visibility_budget_enabled is missing (defaults to enabled)");
    }
    if (!doc.hasHzbEnabled) {
        appendIssue(issues, IssueSeverity::Warning,
                    "hzb_enabled is missing (defaults to enabled)");
    }
    if (!doc.hasComputeDispatchEnabled) {
        appendIssue(issues, IssueSeverity::Warning,
                    "compute_dispatch_enabled is missing (defaults to enabled)");
    }
    if (doc.profiles.empty()) {
        appendIssue(issues, IssueSeverity::Error, "profiles list is empty");
        return issues;
    }

    const std::string normalizedActiveTier = toLowerAscii(trimText(doc.activeTier));
    if (!normalizedActiveTier.empty() &&
        normalizedActiveTier != "low" &&
        normalizedActiveTier != "medium" &&
        normalizedActiveTier != "high") {
        appendIssue(issues, IssueSeverity::Warning,
                    "active_tier should be one of low/medium/high, got: " + doc.activeTier);
    }

    std::set<std::string> uniqueKeys;
    bool activeFound = false;
    for (std::size_t i = 0; i < doc.profiles.size(); ++i) {
        const BudgetProfile& profile = doc.profiles[i];
        const std::string scene = trimText(profile.scene);
        const std::string quality = trimText(profile.quality);
        if (scene.empty() || quality.empty()) {
            appendIssue(issues, IssueSeverity::Error,
                        "profile[" + std::to_string(i) + "] has empty scene/quality");
            continue;
        }

        const std::string key = scene + "|" + quality;
        if (!uniqueKeys.insert(key).second) {
            appendIssue(issues, IssueSeverity::Error,
                        "duplicate profile scene/quality: " + scene + "/" + quality);
        }

        const auto validateMetric = [&](double value, const char* metric) {
            if (!std::isfinite(value)) {
                appendIssue(issues, IssueSeverity::Error,
                            "profile " + scene + "/" + quality + " has non-finite " + metric);
                return;
            }
            if (value < 0.05 || value > 100.0) {
                appendIssue(issues, IssueSeverity::Warning,
                            "profile " + scene + "/" + quality + " " + metric +
                            " out of recommended range [0.05,100.0]: " + std::to_string(value));
            }
        };

        validateMetric(profile.budgets.totalMs, "total_ms");
        validateMetric(profile.budgets.shadowMs, "shadow_ms");
        validateMetric(profile.budgets.mainMs, "main_ms");
        validateMetric(profile.budgets.debugMs, "debug_ms");

        if (scene == doc.activeScene && quality == doc.activeQuality) {
            activeFound = true;
        }
    }

    if (!activeFound) {
        appendIssue(issues, IssueSeverity::Error,
                    "active profile not found in profiles list: " + doc.activeScene + "/" + doc.activeQuality);
    }

    const auto validateTierBinding = [&](const std::string& tierName, const std::string& scene, const std::string& quality) {
        const std::string bindingScene = trimText(scene);
        const std::string bindingQuality = trimText(quality);
        const bool hasScene = !bindingScene.empty();
        const bool hasQuality = !bindingQuality.empty();
        if (!hasScene && !hasQuality) {
            return;
        }
        if (hasScene != hasQuality) {
            appendIssue(issues, IssueSeverity::Warning,
                        "tier_" + tierName + " binding is incomplete: scene/quality must both be set");
            return;
        }
        const std::string bindingKey = bindingScene + "|" + bindingQuality;
        if (!uniqueKeys.contains(bindingKey)) {
            appendIssue(issues, IssueSeverity::Warning,
                        "tier_" + tierName + " binding target missing in profiles: " +
                            bindingScene + "/" + bindingQuality);
        }
    };

    validateTierBinding("low", doc.tierLowScene, doc.tierLowQuality);
    validateTierBinding("medium", doc.tierMediumScene, doc.tierMediumQuality);
    validateTierBinding("high", doc.tierHighScene, doc.tierHighQuality);

    return issues;
}

// Emit diagnostics in human-readable form.
void printIssues(const std::vector<Issue>& issues) {
    for (const Issue& issue : issues) {
        const char* prefix = "[INFO]";
        if (issue.severity == IssueSeverity::Warning) {
            prefix = "[WARN]";
        } else if (issue.severity == IssueSeverity::Error) {
            prefix = "[ERROR]";
        }
        std::cout << prefix << ' ' << issue.message << '\n';
    }
}

int countIssues(const std::vector<Issue>& issues, IssueSeverity severity) {
    int count = 0;
    for (const Issue& issue : issues) {
        if (issue.severity == severity) {
            count += 1;
        }
    }
    return count;
}

std::string profileKey(const BudgetProfile& profile) {
    return profile.scene + "/" + profile.quality;
}

void printUsage() {
    std::cout
        << "BudgetProfileCLI usage:\n"
        << "  BudgetProfileCLI lint <json>\n"
        << "  BudgetProfileCLI precheck <json>\n"
        << "  BudgetProfileCLI diff <old_json> <new_json>\n";
}

int runLint(const std::filesystem::path& jsonPath, bool strictSchema) {
    BudgetDocument document{};
    std::vector<Issue> issues;
    if (!parseBudgetDocument(jsonPath, document, issues)) {
        printIssues(issues);
        return 1;
    }

    std::vector<Issue> validationIssues = validateBudgetDocument(document, strictSchema);
    issues.insert(issues.end(), validationIssues.begin(), validationIssues.end());
    printIssues(issues);

    const int errorCount = countIssues(issues, IssueSeverity::Error);
    const int warningCount = countIssues(issues, IssueSeverity::Warning);
    std::cout << "[SUMMARY] errors=" << errorCount
              << " warnings=" << warningCount
              << " profiles=" << document.profiles.size()
              << " active=" << document.activeScene << "/" << document.activeQuality
              << " auto=" << (document.autoMode ? "on" : "off")
              << " visibilityBudget=" << (document.visibilityBudgetEnabled ? "on" : "off")
              << " hzb=" << (document.hzbEnabled ? "on" : "off")
              << " dispatchProto=" << (document.computeDispatchEnabled ? "on" : "off")
              << " tier=" << (trimText(document.activeTier).empty() ? "n/a" : document.activeTier)
              << '\n';
    return errorCount == 0 ? 0 : 1;
}

// Compare two budget documents and print semantic diffs.
int runDiff(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    BudgetDocument oldDoc{};
    BudgetDocument newDoc{};
    std::vector<Issue> oldIssues;
    std::vector<Issue> newIssues;
    const bool oldOk = parseBudgetDocument(oldPath, oldDoc, oldIssues);
    const bool newOk = parseBudgetDocument(newPath, newDoc, newIssues);

    if (!oldOk || !newOk) {
        if (!oldOk) {
            std::cout << "[DIFF] Failed parsing old file:\n";
            printIssues(oldIssues);
        }
        if (!newOk) {
            std::cout << "[DIFF] Failed parsing new file:\n";
            printIssues(newIssues);
        }
        return 1;
    }

    auto mapProfiles = [](const BudgetDocument& doc) {
        std::map<std::string, PassBudgetConfig> result;
        for (const auto& profile : doc.profiles) {
            result[profileKey(profile)] = profile.budgets;
        }
        return result;
    };

    const auto oldMap = mapProfiles(oldDoc);
    const auto newMap = mapProfiles(newDoc);

    bool hasDifferences = false;
    if (oldDoc.schemaVersion != newDoc.schemaVersion || oldDoc.hasSchemaVersion != newDoc.hasSchemaVersion) {
        hasDifferences = true;
        std::cout << "[DIFF] schema_version "
                  << (oldDoc.hasSchemaVersion ? std::to_string(oldDoc.schemaVersion) : "missing")
                  << " -> "
                  << (newDoc.hasSchemaVersion ? std::to_string(newDoc.schemaVersion) : "missing")
                  << '\n';
    }
    const std::string oldActive = oldDoc.activeScene + "/" + oldDoc.activeQuality;
    const std::string newActive = newDoc.activeScene + "/" + newDoc.activeQuality;
    if (oldActive != newActive) {
        hasDifferences = true;
        std::cout << "[DIFF] active_profile " << oldActive << " -> " << newActive << '\n';
    }
    if (oldDoc.autoMode != newDoc.autoMode || oldDoc.hasAutoMode != newDoc.hasAutoMode) {
        hasDifferences = true;
        std::cout << "[DIFF] auto_mode "
                  << (oldDoc.hasAutoMode ? (oldDoc.autoMode ? "on" : "off") : "missing")
                  << " -> "
                  << (newDoc.hasAutoMode ? (newDoc.autoMode ? "on" : "off") : "missing")
                  << '\n';
    }
    if (oldDoc.visibilityBudgetEnabled != newDoc.visibilityBudgetEnabled ||
        oldDoc.hasVisibilityBudgetEnabled != newDoc.hasVisibilityBudgetEnabled) {
        hasDifferences = true;
        std::cout << "[DIFF] visibility_budget_enabled "
                  << (oldDoc.hasVisibilityBudgetEnabled ? (oldDoc.visibilityBudgetEnabled ? "on" : "off") : "missing")
                  << " -> "
                  << (newDoc.hasVisibilityBudgetEnabled ? (newDoc.visibilityBudgetEnabled ? "on" : "off") : "missing")
                  << '\n';
    }
    if (oldDoc.hzbEnabled != newDoc.hzbEnabled ||
        oldDoc.hasHzbEnabled != newDoc.hasHzbEnabled) {
        hasDifferences = true;
        std::cout << "[DIFF] hzb_enabled "
                  << (oldDoc.hasHzbEnabled ? (oldDoc.hzbEnabled ? "on" : "off") : "missing")
                  << " -> "
                  << (newDoc.hasHzbEnabled ? (newDoc.hzbEnabled ? "on" : "off") : "missing")
                  << '\n';
    }
    if (oldDoc.computeDispatchEnabled != newDoc.computeDispatchEnabled ||
        oldDoc.hasComputeDispatchEnabled != newDoc.hasComputeDispatchEnabled) {
        hasDifferences = true;
        std::cout << "[DIFF] compute_dispatch_enabled "
                  << (oldDoc.hasComputeDispatchEnabled ? (oldDoc.computeDispatchEnabled ? "on" : "off") : "missing")
                  << " -> "
                  << (newDoc.hasComputeDispatchEnabled ? (newDoc.computeDispatchEnabled ? "on" : "off") : "missing")
                  << '\n';
    }
    if (trimText(oldDoc.activeTier) != trimText(newDoc.activeTier)) {
        hasDifferences = true;
        std::cout << "[DIFF] active_tier "
                  << (trimText(oldDoc.activeTier).empty() ? "missing" : oldDoc.activeTier)
                  << " -> "
                  << (trimText(newDoc.activeTier).empty() ? "missing" : newDoc.activeTier)
                  << '\n';
    }

    const auto diffBinding = [&](const char* label,
                                 const std::string& oldScene,
                                 const std::string& oldQuality,
                                 const std::string& newScene,
                                 const std::string& newQuality) {
        const std::string oldValue =
            trimText(oldScene).empty() && trimText(oldQuality).empty() ? "missing" : (oldScene + "/" + oldQuality);
        const std::string newValue =
            trimText(newScene).empty() && trimText(newQuality).empty() ? "missing" : (newScene + "/" + newQuality);
        if (oldValue != newValue) {
            hasDifferences = true;
            std::cout << "[DIFF] " << label << ' ' << oldValue << " -> " << newValue << '\n';
        }
    };
    diffBinding("tier_low", oldDoc.tierLowScene, oldDoc.tierLowQuality, newDoc.tierLowScene, newDoc.tierLowQuality);
    diffBinding("tier_medium",
                oldDoc.tierMediumScene,
                oldDoc.tierMediumQuality,
                newDoc.tierMediumScene,
                newDoc.tierMediumQuality);
    diffBinding("tier_high",
                oldDoc.tierHighScene,
                oldDoc.tierHighQuality,
                newDoc.tierHighScene,
                newDoc.tierHighQuality);

    for (const auto& [key, oldBudget] : oldMap) {
        const auto it = newMap.find(key);
        if (it == newMap.end()) {
            hasDifferences = true;
            std::cout << "[DIFF] removed profile " << key << '\n';
            continue;
        }

        const PassBudgetConfig& newBudget = it->second;
        const auto changed = [](double a, double b) {
            return std::abs(a - b) > 1e-6;
        };
        if (changed(oldBudget.totalMs, newBudget.totalMs) ||
            changed(oldBudget.shadowMs, newBudget.shadowMs) ||
            changed(oldBudget.mainMs, newBudget.mainMs) ||
            changed(oldBudget.debugMs, newBudget.debugMs)) {
            hasDifferences = true;
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "[DIFF] changed " << key
                      << " total " << oldBudget.totalMs << "->" << newBudget.totalMs
                      << " shadow " << oldBudget.shadowMs << "->" << newBudget.shadowMs
                      << " main " << oldBudget.mainMs << "->" << newBudget.mainMs
                      << " debug " << oldBudget.debugMs << "->" << newBudget.debugMs
                      << '\n';
        }
    }

    for (const auto& [key, _] : newMap) {
        if (oldMap.find(key) == oldMap.end()) {
            hasDifferences = true;
            std::cout << "[DIFF] added profile " << key << '\n';
        }
    }

    if (!hasDifferences) {
        std::cout << "[DIFF] No differences.\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Commands: lint, precheck (strict schema), diff.
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "lint") {
        if (argc != 3) {
            printUsage();
            return 1;
        }
        return runLint(argv[2], false);
    }

    if (command == "precheck") {
        if (argc != 3) {
            printUsage();
            return 1;
        }
        return runLint(argv[2], true);
    }

    if (command == "diff") {
        if (argc != 4) {
            printUsage();
            return 1;
        }
        return runDiff(argv[2], argv[3]);
    }

    printUsage();
    return 1;
}

