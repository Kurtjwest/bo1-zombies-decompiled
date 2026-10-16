#pragma once
#include <universal/ptr32.h>
#include <gfx_d3d/r_gfx.h>
#include <universal/com_stringtable.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_font.h>
#include <bgame/bg_weapons.h>
#include <qcommon/ent.h>
#include <bgame/bg_local.h>




struct WeaponOptions // sizeof=0x8500
{                                       // XREF: .data:weaponOptions/r
    struct WeaponOverride // sizeof=0x110
    {                                       // XREF: WeaponOptions/r
        struct ToGold // sizeof=0x14
        {                                       // XREF: WeaponOptions::WeaponOverride/r
                                                // WeaponOptions::WeaponOverride/r ...
            Ptr32<const Material> material;
            Ptr32<const GfxImage> diffuseTexture;
            Ptr32<const GfxImage> specularTexture;
            Ptr32<const GfxImage> reflectionTexture;
            Ptr32<const GfxImage> camoTexture;

            bool Init(const char *matName);
            int Set(int textureOverrideIndex, unsigned __int16 modelMask, const WeaponOptions::WeaponOverride::ToGold *gold);
        };

        int lastFrame;
        Ptr32<const WeaponVariantDef> weapon;
        Ptr32<const GfxImage> camoBaseTexture1;
        Ptr32<const GfxImage> camoBaseTexture2;
        unsigned int camoOverrideIndex1;
        unsigned int camoOverrideIndex2;
        unsigned int numToGold;
        unsigned int numToBlack;
        ToGold toGold[6];
        ToGold toBlack[6];

        void AddToGold(const char *matName, bool gold);
        void Init(const WeaponVariantDef *weapVarDef);
    };

    Ptr32<const GfxImage> camoTextureOverrides[10][64];
    Ptr32<const GfxImage> lensTextureOverrides[10][16];
    Ptr32<const GfxImage> reticleTextureOverrides[10][64];
    float reticleColors[16][4];
    float lensColors[16][4];
    Ptr32<Material> facepaintMaterials[64];
    float facepaintColors[16][4];
    Ptr32<Font_s> clanFont;
    Ptr32<const GfxImage> emblemClearTexture;
    WeaponOverride weaponOverrides[100];
    int numWeaponOverrides;
    int goldCamoIndex;
    WeaponOverride::ToGold gold;
    WeaponOverride::ToGold black;
    Ptr32<const GfxImage> lightClanTagFont;
    Ptr32<const GfxImage> darkClanTagFont;

    static const GfxImage *GetImageFromTable(const StringTable *table, int row, int column);
    void InitWeaponOptionTextures(
        const char *tableName,
        const char *label,
        int labelCol,
        int textureCol,
        Ptr32<const GfxImage> *overrides,
        unsigned int num);
    void InitWeaponOptionMaterials(
        const char *tableName,
        const char *label,
        int labelCol,
        int materialCol,
        Ptr32<Material> *overrides,
        unsigned int num);
    void InitWeaponOptionColors(
        const char *tableName,
        const char *label,
        int labelCol,
        int colorsCol,
        float (*colors)[4]);
    void InitWeaponOptions();
    unsigned int FindCamoOverrideIndex(
        const GfxImage *base,
        unsigned int defaultIndex);

    const WeaponOptions::WeaponOverride *__thiscall GetWeaponOverrides(const WeaponVariantDef *weapVarDef);

    int SetupWeaponOptionsRender(
        int localClientNum,
        const float *origin,
        const WeaponVariantDef *weapVarDef,
        renderOptions_s weaponOptions,
        unsigned __int16 modelMask,
        ShaderConstantSet *constantSet,
        const char *name,
        int textureOverrideIndex);
    bool IsValidRenderOption(renderOptions_s renderOptions);
    char GetWeaponOptionReticleColor(unsigned int reticleColorIndex, float *outColor);
    char GetWeaponOptionLensColor(unsigned int reticleColorIndex, float *outColor);
};


void __cdecl GC_InitWeaponOptionsDvars();

int __cdecl GetShaderConstant(ShaderConstantSet *constantSet, const char *name);


void __cdecl GC_InitWeaponOptions();
int __cdecl CG_SetupWeaponOptionsRender(
                int localClientNum,
                const float *origin,
                const WeaponVariantDef *weaponVarDef,
                renderOptions_s renderOptions,
                ShaderConstantSet *constantSet,
                const char *name);
int __cdecl CG_SetupWeaponOptionsRender(
                int localClientNum,
                const float *origin,
                const WeaponVariantDef *wvd,
                renderOptions_s wo,
                unsigned __int16 modelMask,
                ShaderConstantSet *constantSet,
                const char *name,
                int textureOverrideIndex);
bool __cdecl CG_IsValidRenderOption(renderOptions_s renderOptions);
char __cdecl CG_GetWeaponOptionReticleColor(unsigned int reticleColorIndex, float *outColor);
char __cdecl CG_GetWeaponOptionLensColor(unsigned int reticleColorIndex, float *outColor);


extern const dvar_t *weaponCamoLodDist;
extern const dvar_t *weaponEmblemLodDist;
extern const dvar_t *weaponClanTagLodDist;
extern const dvar_t *facepaintLodDist;