# Bridge shaders

SPIR-V for the OpenXR bridge device's shader copy (`../xr.cpp`), embedded in
`copy_spv.hpp`. After editing `copy.vert` or `copy.frag`, regenerate inside the
`melee-xr` toolbox (glslang-tools):

    glslangValidator -V --target-env vulkan1.1 copy.vert -o copy.vert.spv
    glslangValidator -V --target-env vulkan1.1 copy.frag -o copy.frag.spv

then convert each `.spv` to the `uint32_t` arrays in `copy_spv.hpp` (little-endian words).
