# ZeroFG (vendored)

- Source: https://github.com/hy300leosquizz-ctrl/ZeroFG, tag `v1.1.0`, commit
  `2b7711e47680c25dd461110c182ee5dd4a637f99`.
- Licence: ZeroFG is dual-licensed `Apache-2.0 OR GPL-2.0-or-later` (commit
  `10e68b6b7912f959f3ea026e0e5385e6f9ee2d2e`, after v1.1.0; v1.1.0 itself was Apache-2.0 only). These files
  are used under the **Apache License, Version 2.0**; its text is `LICENSE` here, copied unchanged.
  Commit 10e68b6b changes no file taken here.
- Taken unchanged: `LICENSE`, `include/`, `src/`, `shaders/prebuilt/rc3/` (the compiled SPIR-V the build uses),
  the GLSL sources `shaders/{df,rc7,hud,rc3}/` and `tools/compile_vulkan13_shader.py` (they produce
  `shaders/prebuilt`). Not taken: upstream's CMake files, README, INTEGRATION.md, docs and the `host/` presenters
  (including the files under Xenia's BSD 3-Clause licence).
- No file here is modified. `app/src/main/cpp/waylandcomp/CMakeLists.txt` builds `src/zerofg.cc`,
  `src/df/df_context.cc`, `src/rc7/rc7_context.cc` and `src/hud/hud_layer.cc` into the static library
  `droiddeck_zerofg` with the include paths upstream's CMakeLists.txt sets (C++20, shipped SPIR-V);
  `mali-g2/tools/fg-bench` (branch `framegen`) builds the same files from its own copy.
