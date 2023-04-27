#ifndef SHARED_H
#define SHARED_H

#define MAX_QUADS_COUNT 16
#define MAX_SPHERES_COUNT 16
#define MAX_MATERIAL_COUNT 16

#define MAX_RAY_BOUNCE 32
#define MAX_SAMPLE_PER_PIXEL 64

#ifdef NO_FSL_DEFINITIONS
#define f4x4 mat4
#define STATIC static
#define STRUCT(NAME) struct NAME
#define SHADER_TYPE_STR(TYPE) #TYPE
#define DATA(TYPE, NAME, SEM) TYPE NAME
#define PUSH_CONSTANT(NAME, REG) struct NAME
#define CBUFFER(NAME, FREQ, REG, BINDING) struct NAME
#endif // NO_FSL_DEFINITIONS

// enums
STATIC const uint SkyModeEnum_None = 0;
STATIC const uint SkyModeEnum_VerySimpleSkybox = 1;
STATIC const uint SkyModeEnum_SimpleSkybox = 2;
STATIC const uint SkyModeEnum_ExpensiveAtmosphere = 3;

// structs
PUSH_CONSTANT(RootConstants, b1)
{
    DATA(int4,      args,              None);
};

CBUFFER(FrameUB, UPDATE_FREQ_PER_FRAME, b0, binding = 0)
{
    DATA(f4x4,      viewProj,          None);
    DATA(f4x4,      invViewProj,       None);
    DATA(float4,    camPos,            None);
    DATA(float4,    sunLightDir,       None);
    DATA(float4,    groundColor,       None);
    DATA(float4,    skyZenithColor,    None);
    DATA(float4,    skyHorizonColor,   None);
    DATA(float2,    invScreenSize,     None);
    DATA(uint2,     screenSize,        None);
    DATA(float,     historyWeight,     None);
    DATA(float,     sunFocus,          None);
    DATA(float,     sunIntensity,      None);
    DATA(uint,      skyMode,           None);
    DATA(uint,      frameNumber,       None);
};

STRUCT(Material)
{
    DATA(float4,    spec,              None);
    DATA(float4,    color,             None);
    DATA(float4,    emission,          None);
    DATA(float4,    glossiness,        None);
};

STRUCT(QuadObject)
{
    // packing a, b, c, d
    DATA(float4,    aXYZbX,            None);
    DATA(float4,    bYZcXY,            None);
    DATA(float4,    cZdXYZ,            None);
    DATA(uint4,     matId,             None);
};

STRUCT(SphereObject)
{
    DATA(float4,    centerRadius,      None);
    DATA(uint4,     matId,             None);
};

#ifdef NO_FSL_DEFINITIONS
static_assert((sizeof(Material) % 16) == 0, "16-byte alignment requirement.");
static_assert((sizeof(QuadObject) % 16) == 0, "16-byte alignment requirement.");
static_assert((sizeof(SphereObject) % 16) == 0, "16-byte alignment requirement.");
#endif // NO_FSL_DEFINITIONS

#endif // SHARED_H