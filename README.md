# Game Engine MVP

This repository contains an MVP engine + game loop, with a DX12 renderer baseline and a Phase-3 asset pipeline baseline.

## MVP Scope

- C++20 + CMake project skeleton
- Engine runtime loop with fixed timestep
- Lightweight ECS registry
- Gravity simulation (rigid body integration)
- AABB collision detection and impulse-based resolution
- Sweep-and-prune broadphase with structure-of-arrays collision buffers
- Renderer abstraction (`IRenderer`)
- `NullRenderer` fallback path
- Asset pipeline baseline:
  - OBJ mesh loading with cache + weak lifetime tracking
  - PPM texture loading with cache + weak lifetime tracking
  - Runtime mesh registration (`AssetManager` -> `IRenderer::registerMesh`)
  - Runtime texture/material registration (`registerTexture` / `registerMaterial`)
- Windows DX12 renderer backend:
  - Win32 window + event pump
  - Keyboard camera control (`WASD/QE` move, arrows rotate, `Shift` speed up)
  - Runtime debug toggles (`F1` shadow-factor view, `F2` wireframe, `F3` collision overlay, `F4` HUD)
  - Runtime budget controls (`B` auto mode toggle, `V` visibility-budget toggle, `H` HZB toggle, `C` compute-dispatch toggle, `F5` target select, `F6/F7` adjust+auto-save, `F8` reload, `F9/F10` profile switching, `F11/F12` JSON export/import)
  - DX12 device / swap chain / command queue
  - Depth buffer + indexed mesh draw
  - Per-entity transform upload through constant buffers
  - Dynamic mesh/texture/material pools with default fallbacks
  - Shader-visible SRV heap + texture sampling in pixel shader
  - Directional light + shadow map pass (depth prepass + shadow compare sampling)
  - Frame timing output (CPU frame ms/FPS estimate)
  - GPU pass timing queries (Shadow/Main/Debug/Total)
  - Rolling-average GPU pass telemetry and HUD pass bars
  - Pass-level budget warnings with color-coded HUD thresholds
  - Runtime-adjustable pass budgets with persistence and scene/quality profiles (`assets/config/pass_budgets.cfg`)
  - Automatic complexity-tier budget profile selection (Low/Medium/High with stable-frame hysteresis)
  - Budget profile JSON import/export + validation (`assets/config/pass_budgets.json`)
  - Budget JSON schema versioning + legacy migration checks
  - GPU resource memory tracking (Default/Upload/Readback heaps)
  - Per-pass draw/triangle diagnostics in HUD/logs
  - Camera-frustum culling with structure-of-arrays visibility buffers
  - Occlusion-aware visibility budgeting linked to complexity tiers
  - Hierarchical-Z occlusion pyramid for tighter culling decisions
  - DX12 compute frustum-culling pass with runtime CPU fallback
  - GPU visibility compaction stream (counter + compact index readback)
  - GPU occlusion classification + HZB sampling pass with runtime CPU fallback
  - DX12 `ExecuteIndirect` main-pass submission (texture-batched command stream)
  - Prototype compute-dispatch grouping for broadphase/HZB work
  - Onscreen HUD with camera/debug/perf state
- Sample scene (ground + obstacle + player)

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Run

```powershell
.\build\Release\GameMVP.exe
```

If your generator is not multi-config (for example Ninja), run:

```powershell
.\build\GameMVP.exe
```

On Windows, the executable starts with DX12. If DX12 initialization fails, it automatically falls back to `NullRenderer`.

### Budget Profile CLI

Validate and compare budget profiles before running:

```powershell
.\build\Release\BudgetProfileCLI.exe lint assets/config/pass_budgets.json
.\build\Release\BudgetProfileCLI.exe precheck assets/config/pass_budgets.json
.\build\Release\BudgetProfileCLI.exe diff assets/config/pass_budgets.json assets/config/pass_budgets.json
```

For single-config generators:

```powershell
.\build\BudgetProfileCLI.exe lint assets/config/pass_budgets.json
```

### Controls (DX12)

- `W / A / S / D`: move camera in the horizontal plane
- `Q / E`: move camera up/down
- `Arrow Keys`: rotate camera (yaw/pitch)
- `Shift`: fast camera move
- `F1`: toggle shadow-factor debug view
- `F2`: toggle wireframe mode
- `F3`: toggle collision overlay (AABB + contact markers)
- `F4`: toggle HUD overlay
- `B`: toggle automatic budget profile mode (complexity-tier driven)
- `V`: toggle visibility-budget mode (occlusion-aware culling)
- `H`: toggle hierarchical-Z occlusion path
- `C`: toggle compute-dispatch path (GPU frustum pass + grouped prototype stages)
- `F5`: cycle budget target (Total/Shadow/Main/Debug)
- `F6`: decrease selected budget (`Shift` for larger step, auto-saves config)
- `F7`: increase selected budget (`Shift` for larger step, auto-saves config)
- `F8`: reload pass budget config from `assets/config/pass_budgets.cfg`
- `F9`: cycle quality preset in current scene profile
- `F10`: cycle scene preset for current quality profile
- `F11`: export budget profiles to `assets/config/pass_budgets.json`
- `F12`: import budget profiles from `assets/config/pass_budgets.json` (with validation)
- `Esc`: close window

## What This MVP Validates

- Engine/game separation (`Application` + `GameLogic`)
- Deterministic update cadence using fixed timestep
- Physics baseline (gravity + collision) for rapid iteration
- GPU-oriented collision broadphase layout (SoA + sweep-and-prune pair pruning)
- Real GPU render backend integration (DX12)
- First asset ingestion path with cache and handle-based mesh registration
- Material + texture sampling path integrated into draw calls
- Directional lighting with real-time shadow mapping in the main pass
- Interactive camera/input loop with runtime debug toggles
- Per-second CPU frame-time diagnostics in renderer logs
- Collision debug overlay pass (AABB wire boxes + contact markers)
- Onscreen debug HUD (FPS/CPU/camera/toggle states)
- Per-pass GPU timing diagnostics (Shadow/Main/Debug/Total)
- Rolling-average GPU pass timing and lightweight HUD bars
- Pass budget warnings and threshold color coding in HUD/logs
- Runtime-adjustable pass budgets with scene/quality profiles and persistence
- Complexity-tier driven automatic budget profile binding with runtime/manual switching
- Occlusion-aware visibility budgeting tied to complexity tier transitions
- Hierarchical-Z occlusion diagnostics and runtime switchable path
- Real DX12 compute frustum-culling pass integrated in the visibility stage
- GPU compacted visibility index path feeding culling/render submission
- GPU occlusion classification and HZB decision path (with safe CPU fallback)
- DX12 `ExecuteIndirect` main-pass submission path with direct-draw fallback
- Prototype compute-dispatch grouping for broadphase/HZB stages
- Budget profile JSON import/export and runtime validation workflow
- Budget JSON schema compatibility checks and migration path
- CLI budget profile lint/diff + pre-run validation path
- GPU memory diagnostics in HUD/logs (heap usage + resource counts)
- Per-pass draw/triangle counters for quick render-cost inspection
- Frustum-culling visibility compaction path feeding render submission
- Extensible boundary for future Vulkan backend integration

## Next Up (After This Step)

1. Add tangent generation and basic PBR material fields
2. Add point/spot lights and a simple clustered/forward+ light list
3. Remove visibility readback and drive `ExecuteIndirect` count fully GPU-side
4. Add async asset streaming with upload-queue backpressure control
