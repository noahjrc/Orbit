// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

//-------------------------------------------------------------------------------------
// CasePS.hlsl : a shelf case face (MeshBasicMaterial, no fog). Drawn straight onto
// the back buffer after bloom, like the front menu panels, so it takes the same TV
// trims and sRGB encode FinalPS gives the scene. Output premultiplied for
// SRCBLEND = ONE, DESTBLEND = INVSRCALPHA.
//
// A cover face can sit inside a plastic lip (the rim of the case around its insert)
// under a faint sheen (the clear sleeve), so it reads as a real 360 / Xbox case.
//-------------------------------------------------------------------------------------

#include "Common.hlsli"

sampler2D g_Tex     : register(s0);
float4    g_vColor  : register(c0);   // material colour (linear) and opacity
float4    g_vMode   : register(c1);   // x = 1 when the face has a map (sRGB art); y = 1 when its
                                      // see-through parts show the lip (a package icon)
float4    g_vTrims  : register(c2);   // y = red gain, z = gamma exponent
float4    g_vUV     : register(c3);   // the face's part of the map: uv * xy + zw (a box scan
                                      // holds back, spine and front side by side)
float4    g_vLip    : register(c4);   // x, y = lip width as a fraction of the face; z = sheen
float4    g_vLipCol : register(c5);   // lip plastic colour, linear

float4 main( float2 UV : TEXCOORD0 ) : COLOR
{
    float3 c = g_vColor.rgb;
    if( g_vMode.x > 0.5 )
    {
        // The insert inside the lip
        float2 lip = g_vLip.xy;
        float2 inner = ( UV - lip ) / ( 1.0 - 2.0 * lip );
        float4 map = tex2D( g_Tex, saturate( inner ) * g_vUV.xy + g_vUV.zw );
        float3 art = SRGBToLinear( map.rgb );
        if( g_vMode.y > 0.5 )
            art = lerp( g_vLipCol.rgb, art, map.a );
        float inside = step( lip.x, UV.x ) * step( UV.x, 1.0 - lip.x ) * step( lip.y, UV.y ) * step( UV.y, 1.0 - lip.y );
        c = lerp( g_vLipCol.rgb, art, inside ) * g_vColor.rgb;

        // Sleeve sheen: a soft diagonal band and a little light from the top
        float band = exp( -pow( ( UV.x * 0.7 + UV.y * 0.5 - 0.42 ) / 0.09, 2.0 ) );
        c += g_vLip.z * ( band + 0.35 * ( 1.0 - UV.y ) ) * g_vColor.rgb;
    }
    c.r *= g_vTrims.y;
    c = pow( LinearToSRGB( saturate( c ) ), g_vTrims.z );
    return float4( c * g_vColor.a, g_vColor.a );
}
