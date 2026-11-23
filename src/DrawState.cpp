// DrawState: a complete fixed-function state for our own draws (DrawState.h).
#define CINTERFACE // the C interface of d3d9.h (lpVtbl), as the other draw files
#include "DrawState.h"

namespace SwbfVr
{

namespace DrawState
{

namespace
{

/// The render states a fixed-function triangle draw depends on, with the value we want. Direct3D's defaults where we
/// have no reason to differ.
struct RenderStateValue
{
    D3DRENDERSTATETYPE state;
    DWORD value;
};

constexpr float ZERO_FLOAT = 0.0f;
constexpr float ONE_FLOAT = 1.0f;

DWORD FloatBits(const float& value)
{
    return *reinterpret_cast<const DWORD*>(&value);
}

} // namespace

void Reset(IDirect3DDevice9* device)
{
    if (!device)
    {
        return;
    }
    device->lpVtbl->SetVertexShader(device, nullptr);
    device->lpVtbl->SetPixelShader(device, nullptr);
    const RenderStateValue renderStates[] = {
        {D3DRS_ZENABLE, D3DZB_TRUE},
        {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRS_ZWRITEENABLE, TRUE},
        {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_LASTPIXEL, TRUE},
        {D3DRS_SRCBLEND, D3DBLEND_ONE},
        {D3DRS_DESTBLEND, D3DBLEND_ZERO},
        {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
        {D3DRS_ALPHAREF, 0},
        {D3DRS_ALPHAFUNC, D3DCMP_ALWAYS},
        {D3DRS_DITHERENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE},
        {D3DRS_FOGENABLE, FALSE},
        {D3DRS_SPECULARENABLE, FALSE},
        {D3DRS_RANGEFOGENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_TWOSIDEDSTENCILMODE, FALSE},
        {D3DRS_TEXTUREFACTOR, 0xFFFFFFFF},
        {D3DRS_WRAP0, 0},
        {D3DRS_WRAP1, 0},
        {D3DRS_CLIPPING, TRUE},
        {D3DRS_LIGHTING, FALSE},
        {D3DRS_AMBIENT, 0},
        {D3DRS_COLORVERTEX, TRUE},
        {D3DRS_LOCALVIEWER, TRUE},
        {D3DRS_NORMALIZENORMALS, FALSE},
        {D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1},
        {D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2},
        {D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL},
        {D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL},
        {D3DRS_VERTEXBLEND, D3DVBF_DISABLE},
        {D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE},
        {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_POINTSPRITEENABLE, FALSE},
        {D3DRS_MULTISAMPLEANTIALIAS, TRUE},
        {D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF},
        {D3DRS_COLORWRITEENABLE, 0xF},
        {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_SLOPESCALEDEPTHBIAS, FloatBits(ZERO_FLOAT)},
        {D3DRS_DEPTHBIAS, FloatBits(ZERO_FLOAT)},
        {D3DRS_ANTIALIASEDLINEENABLE, FALSE},
        {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
        {D3DRS_TWEENFACTOR, FloatBits(ZERO_FLOAT)},
        {D3DRS_POINTSIZE, FloatBits(ONE_FLOAT)},
    };
    for (const RenderStateValue& entry : renderStates)
    {
        device->lpVtbl->SetRenderState(device, entry.state, entry.value);
    }
    for (DWORD stage = 0; stage < 8; ++stage)
    {
        device->lpVtbl->SetTexture(device, stage, nullptr);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_COLOROP, stage ? D3DTOP_DISABLE : D3DTOP_MODULATE);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_COLORARG2, D3DTA_CURRENT);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_ALPHAOP, stage ? D3DTOP_DISABLE : D3DTOP_SELECTARG1);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_TEXCOORDINDEX, stage);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->lpVtbl->SetTextureStageState(device, stage, D3DTSS_RESULTARG, D3DTA_CURRENT);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MIPMAPLODBIAS, FloatBits(ZERO_FLOAT));
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MAXMIPLEVEL, 0); // 0: the largest level may be used
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_MAXANISOTROPY, 1);
        device->lpVtbl->SetSamplerState(device, stage, D3DSAMP_SRGBTEXTURE, FALSE);
    }
}

} // namespace DrawState

} // namespace SwbfVr
