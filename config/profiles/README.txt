DXVK test profiles for the GBFR Endless Ragnarok demo (GTX 960M)
=================================================================
The active config is dxvk.conf in the game folder (currently = A_baseline.conf).
To test another profile: close the game, copy that profile over dxvk.conf, start the game.

For each run, stand in the same spot (e.g. first area after the tutorial prompt) and note:
  - FPS and the frame-time graph
  - GPU load %  (95-100% = GPU-bound -> lower in-game settings help; low % = CPU/driver-bound)
  - memory (VRAM) - if it sits near 3.3 GB you are out of VRAM -> lower Texture Quality
  - any missing models / flicker

A_baseline        : same rendering as last run, measurement HUD, vsync off
B_fl11_1          : caps feature level at 11_1 (the 960M's true hardware level)
C_fl11_1_relaxed  : B + relaxed compute barriers + 4x anisotropy cap

In-game (biggest wins, try in this order): Lighting Quality -> Standard/Low,
Rendering Quality -> Low, Resolution -> 1600x900 or 1280x720, Frame rate cap -> 30 for steadiness.
D_vram_budget     : A + tells the game the GPU has 3 GB instead of 4 GB, so its streaming
                    budget stays inside DXVK's ~3.35 GB VRAM budget (less paging to system RAM)
E_steady25        : even 40 ms frame pacing (DXVK limiter at 25 fps), light HUD. Recommended for 720p on the 960M.
