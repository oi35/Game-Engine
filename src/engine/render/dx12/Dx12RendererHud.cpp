// File: Dx12RendererHud.cpp
// Purpose: Contains legacy HUD overlay helpers retained for fallback/debug compatibility.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

void Dx12Renderer::Impl::drawHudOverlay(std::size_t entityCount) {
        // Legacy CPU/GDI HUD path kept as fallback; primary HUD path is now GPU line rendering.
        if (!debugShowHud || window == nullptr) {
            return;
        }

        HDC hdc = GetDC(window);
        if (hdc == nullptr) {
            return;
        }

        HFONT oldFont = nullptr;
        if (hudFont != nullptr) {
            oldFont = static_cast<HFONT>(SelectObject(hdc, hudFont));
        }
        const int oldBkMode = SetBkMode(hdc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(hdc, RGB(235, 242, 248));
        const HBRUSH dcBrush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));

        RECT panelRect{8, 8, static_cast<LONG>(width) - 8, 396};
        SetDCBrushColor(hdc, RGB(20, 26, 33));
        FillRect(hdc, &panelRect, dcBrush);

        // Build textual telemetry block.
        std::ostringstream oss;
        const auto toMiB = [](std::uint64_t bytes) -> double {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        };
        oss << std::fixed << std::setprecision(2);
        oss << "FPS " << cpuFrameLastFps << "  CPU " << cpuFrameLastMs << "ms  Entities " << entityCount
            << "  Contacts " << debugContacts.size() << "\n";
        if (gpuTimingLast.valid) {
            oss << "GPU Total " << gpuTimingLast.totalMs << "ms"
                << "  Shadow " << gpuTimingLast.shadowMs << "ms"
                << "  Main " << gpuTimingLast.mainMs << "ms"
                << "  Debug " << gpuTimingLast.debugMs << "ms\n";
        } else {
            oss << "GPU timings pending...\n";
        }
        if (gpuTimingAverage.valid) {
            oss << "GPU Avg(" << gpuTimingHistoryCount << ") "
                << "Total " << gpuTimingAverage.totalMs << "ms"
                << "  Shadow " << gpuTimingAverage.shadowMs << "ms"
                << "  Main " << gpuTimingAverage.mainMs << "ms"
                << "  Debug " << gpuTimingAverage.debugMs << "ms\n";
        } else {
            oss << "GPU average warming up...\n";
        }
        oss << "Mem MiB D/U/R/O "
            << toMiB(gpuMemoryStats.defaultBytes) << "/"
            << toMiB(gpuMemoryStats.uploadBytes) << "/"
            << toMiB(gpuMemoryStats.readbackBytes) << "/"
            << toMiB(gpuMemoryStats.otherBytes) << "\n";
        oss << "Res Count D/U/R/O "
            << gpuMemoryStats.defaultResourceCount << "/"
            << gpuMemoryStats.uploadResourceCount << "/"
            << gpuMemoryStats.readbackResourceCount << "/"
            << gpuMemoryStats.otherResourceCount << "\n";
        oss << "Pass S D/T " << passDrawStatsLast.shadowDrawCalls << "/" << passDrawStatsLast.shadowTriangles
            << "  M D/T " << passDrawStatsLast.mainDrawCalls << "/" << passDrawStatsLast.mainTriangles
            << "  D D/L " << passDrawStatsLast.debugDrawCalls << "/" << passDrawStatsLast.debugLines
            << "  C In/V/Out " << passDrawStatsLast.cullInputCount << "/" << passDrawStatsLast.cullVisibleCount
            << "/" << passDrawStatsLast.cullRejectedCount
            << "  G(F/O) " << passDrawStatsLast.cullFrustumDispatchGroups << "/"
            << passDrawStatsLast.cullOcclusionDispatchGroups
            << "  HZB L/B/C/R " << passDrawStatsLast.hzbLevelsBuilt << "/"
            << passDrawStatsLast.hzbBuildDispatchGroups << "/"
            << passDrawStatsLast.hzbCellsTested << "/"
            << passDrawStatsLast.hzbRejectedCount
            << "\n";
        oss << "Cam (" << cameraPosition.x << ", " << cameraPosition.y << ", " << cameraPosition.z << ")"
            << "  Yaw " << cameraYawRadians << "  Pitch " << cameraPitchRadians << "\n";
        oss << "F1 Shadow:" << (debugShowShadowFactor ? "ON" : "OFF")
            << "  F2 Wire:" << (debugWireframe ? "ON" : "OFF")
            << "  F3 Coll:" << (debugShowCollision ? "ON" : "OFF")
            << "  F4 HUD:" << (debugShowHud ? "ON" : "OFF") << "\n";
        const BudgetProfile* hudProfile = activeBudgetProfile();
        const std::string hudScene = hudProfile != nullptr ? hudProfile->scene : "none";
        const std::string hudQuality = hudProfile != nullptr ? hudProfile->quality : "none";
        oss << "BudgetMode " << (budgetAutoModeEnabled ? "AUTO" : "MANUAL")
            << "  Tier " << complexityTierName(budgetTierCurrent)
            << "  Score " << budgetComplexityScore;
        if (budgetAutoModeEnabled && budgetTierPending != budgetTierCurrent) {
            oss << "  Pending " << complexityTierName(budgetTierPending)
                << " (" << budgetTierPendingFrames << "/" << kBudgetAutoSwitchHoldFrames << ")";
        }
        oss << "\n";
        const ComplexityTier visibilityTier = effectiveVisibilityTier();
        const VisibilityTierBudget& visibilityTierBudget = visibilityBudgetForTier(visibilityTier);
        oss << "VisBudget " << (visibilityBudgetEnabled ? "ON" : "OFF")
            << "  Tier " << complexityTierName(visibilityTier)
            << "  HZB " << (hzbOcclusionEnabled ? "ON" : "OFF")
            << "  Dispatch " << (computeDispatchPrototypeEnabled ? "ON" : "OFF")
            << "  Indirect " << (indirectMainDrawReady ? "ON" : "OFF")
            << "  FrustumGPU " << (gpuFrustumCullingReady ? "READY" : "CPU")
            << "  OcclusionGPU " << (gpuOcclusionCullingReady ? "READY" : "CPU")
            << "  State " << visibilityBudgetState
            << "  Vis/F/O " << visibilityVisibleRatio << "/" << visibilityFrustumRejectedRatio << "/"
            << visibilityOcclusionRejectedRatio
            << "  Target<=" << visibilityTierBudget.maxVisibleRatio
            << "  OccCov>=" << visibilityTierBudget.occlusionCoverageThreshold
            << "  V/H/C Toggle\n";
        oss << "BudgetProfile " << hudScene << "/" << hudQuality
            << "  Target " << budgetTargetName(selectedBudgetTarget)
            << " T/S/M/D " << passBudgetConfig.totalMs << "/" << passBudgetConfig.shadowMs << "/"
            << passBudgetConfig.mainMs << "/" << passBudgetConfig.debugMs << "ms"
            << "  B AutoMode  V VisBudget  H HZB  C Dispatch  F5 Target  F6 -  F7 + (AutoSave)  F8 Reload"
            << "  F9 Quality  F10 Scene  F11 ExportJson  F12 ImportJson";

        const std::string text = oss.str();
        RECT textRect{16, 14, static_cast<LONG>(width) - 16, 284};
        DrawTextA(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_NOCLIP);

        // Draw budget bar widgets and over-budget indicators.
        const int barLabelX = 16;
        const int barTrackX = 178;
        const int barTop = 292;
        const int barRowHeight = 20;
        const int barTrackHeight = 11;
        const int barTrackWidth =
            std::max(220, std::min(540, static_cast<int>(width) - barTrackX - 34));
        const double totalBudgetMs = std::max(0.05, passBudgetConfig.totalMs);
        const double shadowBudgetMs = std::max(0.05, passBudgetConfig.shadowMs);
        const double mainBudgetMs = std::max(0.05, passBudgetConfig.mainMs);
        const double debugBudgetMs = std::max(0.05, passBudgetConfig.debugMs);
        const double barTotalMs = gpuTimingAverage.valid ? gpuTimingAverage.totalMs : 0.0;
        const double barShadowMs = gpuTimingAverage.valid ? gpuTimingAverage.shadowMs : 0.0;
        const double barMainMs = gpuTimingAverage.valid ? gpuTimingAverage.mainMs : 0.0;
        const double barDebugMs = gpuTimingAverage.valid ? gpuTimingAverage.debugMs : 0.0;

        std::ostringstream barTitle;
        barTitle << std::fixed << std::setprecision(2)
                 << "GPU pass bars (" << gpuTimingHistoryCount << "/" << kGpuTimingHistoryWindow
                 << " avg) budgets T/S/M/D=" << totalBudgetMs << "/" << shadowBudgetMs << "/"
                 << mainBudgetMs << "/" << debugBudgetMs
                 << "ms  <=60% OK <=90% WATCH <=100% HIGH >100% OVER";
        const std::string barTitleText = barTitle.str();
        TextOutA(hdc,
                 barLabelX,
                 barTop - 18,
                 barTitleText.c_str(),
                 static_cast<int>(barTitleText.size()));

        struct BudgetVisual {
            const char* state;
            COLORREF color;
        };
        const auto classifyBudget = [](double ratio) -> BudgetVisual {
            if (ratio <= 0.60) {
                return {"OK", RGB(140, 214, 132)};
            }
            if (ratio <= 0.90) {
                return {"WATCH", RGB(236, 213, 114)};
            }
            if (ratio <= 1.00) {
                return {"HIGH", RGB(255, 170, 102)};
            }
            return {"OVER", RGB(245, 108, 108)};
        };

        struct HudBar {
            const char* label;
            double valueMs;
            double budgetMs;
        };
        const std::array<HudBar, 4> bars = {{
            {"Total", barTotalMs, totalBudgetMs},
            {"Shadow", barShadowMs, shadowBudgetMs},
            {"Main", barMainMs, mainBudgetMs},
            {"Debug", barDebugMs, debugBudgetMs},
        }};

        const bool hasBudgetSamples = gpuTimingAverage.valid && gpuTimingHistoryCount > 0;
        bool hasOverBudgetPass = false;
        std::ostringstream budgetWarning;
        budgetWarning << std::fixed << std::setprecision(0) << "Budget alerts: ";
        if (hasBudgetSamples) {
            for (const auto& bar : bars) {
                const double ratio = bar.budgetMs > 0.0 ? (bar.valueMs / bar.budgetMs) : 0.0;
                if (ratio > 1.0) {
                    hasOverBudgetPass = true;
                    budgetWarning << bar.label << " " << (ratio * 100.0) << "%  ";
                }
            }
        } else {
            budgetWarning << "warming up";
        }
        if (hasBudgetSamples && !hasOverBudgetPass) {
            budgetWarning << "none";
        }
        const std::string budgetWarningText = budgetWarning.str();
        SetTextColor(hdc, !hasBudgetSamples ? RGB(178, 185, 192) : (hasOverBudgetPass ? RGB(245, 108, 108) : RGB(140, 214, 132)));
        TextOutA(hdc,
                 barLabelX,
                 barTop - 34,
                 budgetWarningText.c_str(),
                 static_cast<int>(budgetWarningText.size()));

        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int y = barTop + static_cast<int>(i) * barRowHeight;
            const double ratio = hasBudgetSamples && bars[i].budgetMs > 0.0 ? (bars[i].valueMs / bars[i].budgetMs) : 0.0;
            const BudgetVisual visual = hasBudgetSamples ? classifyBudget(ratio) : BudgetVisual{"WARMUP", RGB(178, 185, 192)};

            std::ostringstream label;
            label << std::fixed << std::setprecision(2)
                  << bars[i].label << " " << bars[i].valueMs << "/" << bars[i].budgetMs
                  << "ms (" << (ratio * 100.0) << "%) " << visual.state;
            const std::string labelText = label.str();
            SetTextColor(hdc, visual.color);
            TextOutA(hdc, barLabelX, y, labelText.c_str(), static_cast<int>(labelText.size()));

            RECT trackRect{barTrackX, y + 3, barTrackX + barTrackWidth, y + 3 + barTrackHeight};
            SetDCBrushColor(hdc, RGB(52, 61, 73));
            FillRect(hdc, &trackRect, dcBrush);

            const double normalized = std::clamp(ratio, 0.0, 1.0);
            const int fillWidth = static_cast<int>(
                std::round(normalized * static_cast<double>(barTrackWidth)));
            if (fillWidth > 0) {
                RECT fillRect{barTrackX, y + 3, barTrackX + fillWidth, y + 3 + barTrackHeight};
                SetDCBrushColor(hdc, visual.color);
                FillRect(hdc, &fillRect, dcBrush);
            }

            if (ratio > 1.0) {
                SetDCBrushColor(hdc, visual.color);
                FrameRect(hdc, &trackRect, dcBrush);
            }
        }

        SetTextColor(hdc, oldColor);
        SetBkMode(hdc, oldBkMode);
        if (oldFont != nullptr) {
            SelectObject(hdc, oldFont);
        }
        ReleaseDC(window, hdc);
    }



}  // namespace engine::render

#endif  // _WIN32

