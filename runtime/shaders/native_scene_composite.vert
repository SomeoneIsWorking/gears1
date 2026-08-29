#version 450

// Native vertex implementation of the observed full-screen scene composite.
//
// The module consumes the host renderer's shared guest-memory buffer and the
// ordinary system/float/fetch blocks. Its fetch layout is an observed contract:
// fetch constant 95 names a 12-dword vertex, with position at offset 0, UV at
// offset 4, and the colour/interpolator at offset 6. No title instructions or
// generated microcode are embedded here.

layout(location = 0) out vec4 out_interpolator_0;
layout(location = 1) out vec4 out_interpolator_1;

layout(set = 0, binding = 0, std430) readonly buffer SharedMemory
{
    uint words[];
} shared_memory;

layout(set = 1, binding = 0, std140) uniform SystemConstants
{
    uvec4 words[33];
} system_constants;

layout(set = 1, binding = 1, std140) uniform FloatConstants
{
    vec4 values[4];
} float_constants;

layout(set = 1, binding = 4, std140) uniform FetchConstants
{
    uvec4 words[48];
} fetch_constants;

uint FetchWord(uint index)
{
    return fetch_constants.words[index / 4u][index % 4u];
}

uint Swap8In16(uint value)
{
    return ((value >> 8u) & 0x00ff00ffu) | ((value & 0x00ff00ffu) << 8u);
}

uint Swap16In32(uint value)
{
    return (value >> 16u) | (value << 16u);
}

uint DecodeEndian(uint value, uint mode)
{
    if (mode == 1u || mode == 2u)
        value = Swap8In16(value);
    if (mode == 2u || mode == 3u)
        value = Swap16In32(value);
    return value;
}

vec4 LoadFloat4(uint address, uint mode)
{
    return uintBitsToFloat(uvec4(DecodeEndian(shared_memory.words[address], mode),
                                 DecodeEndian(shared_memory.words[address + 1u], mode),
                                 DecodeEndian(shared_memory.words[address + 2u], mode),
                                 DecodeEndian(shared_memory.words[address + 3u], mode)));
}

vec2 LoadFloat2(uint address, uint mode)
{
    return uintBitsToFloat(uvec2(DecodeEndian(shared_memory.words[address], mode),
                                 DecodeEndian(shared_memory.words[address + 1u], mode)));
}

void main()
{
    uint index = uint(gl_VertexIndex);
    uint indexMode = system_constants.words[0].z;
    index = DecodeEndian(index, indexMode);
    int signedIndex = int(index) + int(system_constants.words[0].w);

    uint fetchDword = FetchWord(190u);
    uint fetchMode = FetchWord(191u) & 3u;
    uint vertexAddress = (fetchDword >> 2u) + uint(signedIndex) * 12u;
    vec4 position = LoadFloat4(vertexAddress, fetchMode);
    vec2 textureCoordinate = LoadFloat2(vertexAddress + 4u, fetchMode);
    vec4 colour = LoadFloat4(vertexAddress + 6u, fetchMode);

    precise vec4 transformed;
    transformed = position.y * float_constants.values[1].wzyx;
    transformed = position.x * float_constants.values[0].xzwy + transformed.wyxz;
    transformed = position.z * float_constants.values[2].yzxw + transformed.wyxz;
    transformed = position.w * float_constants.values[3] + transformed.zxyw;

    uint flags = system_constants.words[0].x;
    float reciprocalW = (flags & 32u) != 0u ? transformed.w : 1.0 / transformed.w;
    vec2 correctedXY = (flags & 8u) != 0u ? transformed.xy * reciprocalW : transformed.xy;
    float correctedZ = (flags & 16u) != 0u ? transformed.z * reciprocalW : transformed.z;
    vec3 ndc = vec3(correctedXY, correctedZ) * uintBitsToFloat(system_constants.words[1].xyz) +
               uintBitsToFloat(system_constants.words[2].xyz);

    gl_Position = vec4(ndc, reciprocalW);
    out_interpolator_0 = vec4(textureCoordinate, 0.0, 0.0);
    out_interpolator_1 = colour;
}
