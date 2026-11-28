# CRT filter shaders

DOSBox Staging's VGA monitor shaders (MIT; see `LICENSE-shaders.txt`),
taken from https://github.com/dosbox-staging/dosbox-staging at commit
`edde12b88ef2124d474418c2d41f83b3e88d2eb1` (`resources/shaders/crt/`):

| File | Original | Used for |
|---|---|---|
| `vga-1080p.frag` | `vga-1080p.glsl` | exactly 2 screen rows per game line (1080p) |
| `crt-hyllian.frag` | `crt-hyllian.glsl` | 3 rows (preset `vga-1440p`), 4 or more (`vga-4k`) |

The presets' values (`resources/shader-presets/crt/crt-hyllian/vga-1440p.preset`,
`vga-4k.preset`) and the selection rule (`src/gui/render/auto_shader_switcher.cpp`)
are in `src/crt.cpp`.

Ported to Vulkan fragment shaders for SDL's GPU renderer: the OpenGL inputs
and uniforms were replaced by SDL's bindings (texture in set 2, parameters
in a uniform block in set 3) and a few `#define`s for values the original
vertex shader computed; the shader code itself is unchanged. CMake compiles
them to SPIR-V with glslang and embeds them (`src/crt_shaders.S.in`).
