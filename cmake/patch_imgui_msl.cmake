# Make Dear ImGui's SDL_GPU backend prefer runtime-compiled MSL over its precompiled .metallib on
# Metal. The bundled metallib is built for MSL 3.1, which needs macOS 14+; the MSL-source path is
# compiled by SDL at runtime to whatever the OS supports, so it runs on macOS 11+. We can't steer
# this through SDL_CreateGPUDevice — SDL's Metal backend always advertises BOTH formats and ignores
# the ones you requested (SDL_gpu_metal.m, unconditional), so the backend always picks the metallib
# and the pipeline fails to create on macOS < 14 ("language version 3.1 incompatible with this OS"),
# then the draw asserts "Graphics pipeline not found". So we flip the choice here.
#
# Applied as a FetchContent PATCH_COMMAND (cwd = the imgui source dir). Idempotent, and it hard-errors
# if the upstream line moves — so a future ImGui bump can't silently reintroduce the macOS-13 crash.
set(path "backends/imgui_impl_sdlgpu3.cpp")
file(READ "${path}" src)

if(src MATCHES "lsl: prefer MSL")
  return()   # already patched
endif()

set(orig "if (supported_formats & SDL_GPU_SHADERFORMAT_METALLIB)")
# Use the metallib only when MSL is NOT available (never, on Metal) -> always fall through to MSL.
set(repl "if ((supported_formats & SDL_GPU_SHADERFORMAT_METALLIB) && !(supported_formats & SDL_GPU_SHADERFORMAT_MSL)) /* lsl: prefer MSL for macOS <14 */")

string(FIND "${src}" "${orig}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "patch_imgui_msl: anchor not found in ${path}.\n"
    "Dear ImGui changed its SDL_GPU Metal shader selection; update this patch (and re-verify "
    "macOS < 14 still uses MSL source, not the macOS-14 metallib).")
endif()

# Anchor must be unique — REPLACE rewrites ALL occurrences, so a second (unrelated) use of the
# condition elsewhere would be mangled. Check the remainder after the first match.
string(LENGTH "${orig}" orig_len)
math(EXPR after_pos "${pos} + ${orig_len}")
string(SUBSTRING "${src}" ${after_pos} -1 rest)
string(FIND "${rest}" "${orig}" pos2)
if(NOT pos2 EQUAL -1)
  message(FATAL_ERROR
    "patch_imgui_msl: anchor occurs more than once in ${path}; REPLACE would rewrite an "
    "unintended site. Dear ImGui changed — make the anchor specific and re-verify the patch.")
endif()

string(REPLACE "${orig}" "${repl}" src "${src}")
file(WRITE "${path}" "${src}")
message(STATUS "Patched imgui_impl_sdlgpu3.cpp: prefer MSL over metallib (macOS 11+ Metal support)")
