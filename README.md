# GTA SA Motion Blur

Standalone **Direct3D 9 temporal motion blur ASI** for GTA San Andreas.

## Build
- Visual Studio 2022: open `MotionBlur.sln`, choose **Release | Win32**, then Build Solution.
- Output: `bin\\Release\\MotionBlur.asi`.
- GitHub Actions automatically builds Release|Win32 on every push to `main`.
- The Actions artifact is `GTA-SA-MotionBlur-Win32` and contains `MotionBlur.asi` and `MotionBlur.ini`.

## Install
Copy `MotionBlur.asi` and `MotionBlur.ini` into the GTA San Andreas folder used by your ASI loader.

## Configuration
Edit `MotionBlur.ini`:
- `Enabled`: 1/0
- `StrengthPercent`: overall effect strength
- `BlendPercent`: previous-frame contribution
- `PersistencePercent`: trail persistence
- `Quality`: 1-4 temporal strength preset

Recommended starting values for a 60 FPS target:
```ini
Enabled=1
StrengthPercent=70
BlendPercent=65
PersistencePercent=80
Quality=2
```

## How it works
The plugin hooks D3D9 `Present` and keeps the previous final GPU frame in a texture. The current frame and previous frame are blended on the GPU immediately before presentation, so the effect is applied to the final image rather than individual world objects.

## Limitation
This is **temporal frame accumulation**, not true motion-vector/per-pixel directional blur. It can create smooth trails and reduce the perceived judder of 60 FPS, but high persistence can produce ghosting around moving objects.

No ENB, ReShade or SweetFX is required.
