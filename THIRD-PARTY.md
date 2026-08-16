# Third-party components

Everything in this repository is MIT licensed (see [LICENSE](LICENSE))
**except** the files listed below, which are third-party and carry their
own terms. Each one keeps its original license text in place -- either in
the file itself or in a `LICENSE-*.md` note beside it -- and this file is
just the index.

## Compiler

| Path | Component | License |
|---|---|---|
| `base/base.c`, `base/base.h` | Derived from Ryan Fleury's base layer (as published in [raddebugger](https://github.com/EpicGamesExt/raddebugger)) | MIT -- see [base/NOTICE.md](base/NOTICE.md) |

## Example programs

None of these are needed to build `3b` itself. They are vendored into
`examples/` so the SDL3/OpenGL demos are self-contained; `examples/game3d/`
reaches several of them by symlink rather than carrying a second copy.

| Path | Component | License |
|---|---|---|
| `examples/game/glad/gl.h`, `examples/game/glad/glad_impl.c` | [GLAD](https://github.com/Dav1dde/glad) OpenGL loader (generated) | `(WTFPL OR CC0-1.0) AND Apache-2.0` -- SPDX header in each file |
| `examples/game/stbimg/stb_image.h` | [stb_image](https://github.com/nothings/stb) by Sean Barrett | MIT OR Public Domain (Unlicense) -- notice at end of file |
| `examples/game/stbtt/stb_truetype.h` | [stb_truetype](https://github.com/nothings/stb) by Sean Barrett | MIT OR Public Domain (Unlicense) -- notice at end of file |
| `examples/game3d/gltf/cgltf.h` | [cgltf](https://github.com/jkuhlmann/cgltf) glTF 2.0 parser | MIT -- notice at end of file |

## Example assets

| Path | Component | License |
|---|---|---|
| `examples/game3d/CesiumMan.gltf` | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) model, by Cesium | CC-BY 4.0 -- see [LICENSE-cesium.md](examples/game3d/LICENSE-cesium.md) |
| `examples/game3d/RiggedFigure.gltf` | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) model, by Cesium | CC-BY 4.0 -- see [LICENSE-riggedfigure.md](examples/game3d/LICENSE-riggedfigure.md) |
| `examples/game3d/DejaVuSans.ttf` | [DejaVu Sans](https://dejavu-fonts.github.io/) regular | Bitstream Vera License -- see [LICENSE-dejavu.md](examples/game3d/LICENSE-dejavu.md) |

Everything else under `examples/` is this repository's own: the 2D demo's
character sheet (`examples/game/assets/hero_placeholder.png`) is drawn by
the generator checked in next to it, and the small textures
(`dots.png`, the checker/brick patterns) are produced in code.
