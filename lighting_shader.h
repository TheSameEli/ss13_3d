// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#pragma once
// D3D9 ps_2_0: native lighting sampled in floor coordinates, with local fog/emission.
static constexpr const char *lightingShaderSource = R"hlsl(
sampler2D art:register(s0);
    sampler2D nativeLight:register(s1);
    float4 lightUV:register(c0);
    float4 fogBounds:register(c1);
    float4 emission:register(c2);
    float4 lavaLight:register(c3);
    float4 viewFog:register(c4);
    float4 ambient:register(c5);
    float4 main(float4 tint:COLOR0,float2 uv:TEXCOORD0,float2 light:TEXCOORD1):COLOR0 {
    float4 a=tex2D(art,uv)*tint;
    float4 l=tex2D(nativeLight,light*lightUV.xy+lightUV.zw);
    float3 color=a.rgb*saturate(1-l.a+l.rgb);
    color=lerp(color,dot(color,float3(0.22,0.70,0.08)),0.24);
    color=pow(saturate(color),1.20)*float3(0.51,0.57,0.65);
    color=lerp(color,a.rgb,saturate((max(a.r,max(a.g,a.b))-.55)*4)*emission.x);
    float glow=saturate(1-length((light-lavaLight.xy)*fogBounds.xy)/64)*lavaLight.z;
    color+=a.rgb*float3(1,.28,.02)*glow*.8;
    color=lerp(color,saturate(a.rgb*1.5+float3(.12,.025,0)),emission.y);
    color=max(color,a.rgb*ambient.x);
    float2 edge=min(light,1-light)*fogBounds.xy;
    float fog=1-saturate(min(edge.x,edge.y)*fogBounds.z);
    fog=max(fog,saturate((length((light-viewFog.xy)*fogBounds.xy)-viewFog.z)*viewFog.w));
    return float4(lerp(color,float3(0,0,0),fog),a.a);
    }
)hlsl";
