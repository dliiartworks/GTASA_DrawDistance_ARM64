#include <mod/amlmod.h>
#include <mod/iaml.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/interface.h>
#include <mod/isautils.h>
#include <aml-psdk/game_sa/other/Pools.h>
#include <stdio.h>
#include <stdint.h>

ISAUtils* sautils = nullptr;

MYMODCFG(net.rusjj.gtasa.drawdistance, GTA:SA Draw Distance, 1.1, RusJJ)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.0)
END_DEPLIST()

/* ============================================================ */
/*  Global variables                                             */
/* ============================================================ */
uintptr_t pGTASA = 0;
void* maincamera = nullptr;
unsigned int* streamingMemoryAvailable = nullptr;
unsigned int* streamingMemoryUsed = nullptr;

float fRealAspectRatio = 0.0f;

/* Pools for buildings/dummies */
CPool<CBuilding>** pBuildingPool = nullptr;
CPool<CDummy>** pDummyPool = nullptr;

/* Camera range globals */
float* CameraRangeMinX = nullptr;
float* CameraRangeMaxX = nullptr;
float* CameraRangeMinY = nullptr;
float* CameraRangeMaxY = nullptr;

/* Configs */
ConfigEntry* pNearClipOverride = nullptr;
ConfigEntry* pDrawDistanceOverride = nullptr;
ConfigEntry* pStreamingDistanceScale = nullptr;

/* ============================================================ */
/*  Constants & helpers                                          */
/* ============================================================ */
#define DEFAULT_DRAWDISTANCE 800.0f
#define MAX_DRAWDISTANCE     8000.0f   // 10x default
#define MAX_STREAMING_SCALE  1000      // 10x default (100)

static char szRetDrawDistanceSlider[16];
static char szRetScaleSlider[16];

using CalculateAspectRatioFn = float (*)();

/* ============================================================ */
/*  Pool expansion hooks                                         */
/* ============================================================ */
DECL_HOOKv(CPoolsInitialise)
{
    CPoolsInitialise();

    if (pBuildingPool && *pBuildingPool)
    {
        (*pBuildingPool)->Flush();
        delete *pBuildingPool;
        *pBuildingPool = new CPool<CBuilding>(64000, "BuildingsAreCool");
    }

    if (pDummyPool && *pDummyPool)
    {
        (*pDummyPool)->Flush();
        delete *pDummyPool;
        *pDummyPool = new CPool<CDummy>(48000, "DummiesAreCool");
    }
}

/* ============================================================ */
/*  Streaming memory management                                  */
/* ============================================================ */
static void UpdateStreamingMemory()
{
    if (!streamingMemoryAvailable || !streamingMemoryUsed)
        return;

    uint32_t used  = *streamingMemoryUsed;
    uint32_t avail = *streamingMemoryAvailable;

    // If available is nearly full, add 128MB up to 2GB
    if (avail <= used + 50000000) // 50MB spare
    {
        if (avail < (2048u * 1024u * 1024u))
        {
            avail += (128u * 1024u * 1024u);
            if (avail > (2048u * 1024u * 1024u))
                avail = (2048u * 1024u * 1024u);
            *streamingMemoryAvailable = avail;
        }
    }
}

/* ============================================================ */
/*  Range & scale helpers                                        */
/* ============================================================ */
static inline float GetRangeScale()
{
    float scale = pStreamingDistanceScale ? pStreamingDistanceScale->GetFloat() : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    float aspect = fRealAspectRatio;
    if (aspect > 0.0f && aspect < 1.0f) aspect = 1.0f / aspect;

    return aspect * 0.75f * scale;
}

/* ============================================================ */
/*  Rendering patches                                            */
/* ============================================================ */
static void UpdateRenderingPatches(float drawDist)
{
    // Clamp drawDist
    if (drawDist < 200.0f) drawDist = 200.0f;
    if (drawDist > MAX_DRAWDISTANCE) drawDist = MAX_DRAWDISTANCE;

    float scale = drawDist / DEFAULT_DRAWDISTANCE;
    if (scale < 1.0f) scale = 1.0f;

    // LOD / render distance
    *(float*)(pGTASA + 0x886364) = 50.0f * scale;       // CRenderer::ms_lodDistScale
    *(float*)(pGTASA + 0x886368) = 50.0f * scale;       // CRenderer::ms_lowLodDistScale
    *(float*)(pGTASA + 0xCC7EFC) = 30000.0f * scale;    // CDraw::ms_fLODDistance
    *(float*)(pGTASA + 0xBCF8E0) = 30000.0f * scale;    // CRenderer::ms_fFarClipPlane

    // Grass / vegetation
    *(float*)(pGTASA + 0x85EBD0) = 15000.0f * scale;    // CGrassRenderer::m_farDist

    // Vehicle LOD distances
    *(float*)(pGTASA + 0xD28974) = 12000.0f * scale;    // ms_vehicleLod0Dist
    *(float*)(pGTASA + 0xD28978) = 12000.0f * scale;    // ms_vehicleLod1Dist
    *(float*)(pGTASA + 0xD2897C) = 12000.0f * scale;    // ms_bigVehicleLod0Dist

    // Ped LOD / fade
    *(float*)(pGTASA + 0xD28980) = 6000.0f * scale;     // ms_pedLodDist
    *(float*)(pGTASA + 0xD28984) = 6500.0f * scale;     // ms_pedFadeDist

    // Cull / misc distance
    *(float*)(pGTASA + 0xD28988) = 12000.0f * scale;    // ms_cullCompsDist
    *(float*)(pGTASA + 0xD2898C) = 12000.0f * scale;    // ms_cullBigCompsDist

    // Streaming memory base
    if (streamingMemoryAvailable)
    {
        if (drawDist >= 3800.0f)
            *streamingMemoryAvailable = 2048u * 1024u * 1024u; // 2 GB
        else if (drawDist > 200.0f)
            *streamingMemoryAvailable = 1024u * 1024u * 1024u; // 1 GB
        else
            *streamingMemoryAvailable = 512u * 1024u * 1024u;  // 512 MB
    }
}

/* ============================================================ */
/*  Hooks                                                       */
/* ============================================================ */
DECL_HOOK(void*, RwCameraSetNearClipPlane, void* self, float a1)
{
    if (self == maincamera && pNearClipOverride && pNearClipOverride->GetFloat() > 0.0f)
        return RwCameraSetNearClipPlane(self, pNearClipOverride->GetFloat());
    return RwCameraSetNearClipPlane(self, a1);
}

DECL_HOOK(void*, RwCameraSetFarClipPlane, void* self, float a1)
{
    if (self == maincamera && pDrawDistanceOverride)
        return RwCameraSetFarClipPlane(self, pDrawDistanceOverride->GetFloat());
    return RwCameraSetFarClipPlane(self, a1);
}

DECL_HOOK(void*, CameraCreate, int a1, int a2, int a3)
{
    maincamera = CameraCreate(a1, a2, a3);
    return maincamera;
}

DECL_HOOKv(CStreamingUpdate, void* self)
{
    // Ensure memory is set based on draw distance (redundant but safe)
    if (streamingMemoryAvailable && pDrawDistanceOverride)
    {
        float drawDist = pDrawDistanceOverride->GetFloat();
        if (drawDist >= 3800.0f)
            *streamingMemoryAvailable = 2048u * 1024u * 1024u;
        else if (drawDist >= 2000.0f)
            *streamingMemoryAvailable = 1536u * 1024u * 1024u;
        else if (drawDist > 200.0f)
            *streamingMemoryAvailable = 1024u * 1024u * 1024u;
        else
            *streamingMemoryAvailable = 512u * 1024u * 1024u;
    }

    UpdateStreamingMemory();
    CStreamingUpdate(self);
}

DECL_HOOKv(CWaterLevelSetCameraRange)
{
    CWaterLevelSetCameraRange();

    if (CameraRangeMinX) *CameraRangeMinX = -60000.0f;
    if (CameraRangeMaxX) *CameraRangeMaxX =  60000.0f;
    if (CameraRangeMinY) *CameraRangeMinY = -60000.0f;
    if (CameraRangeMaxY) *CameraRangeMaxY =  60000.0f;

    if (CameraRangeMaxX && CameraRangeMaxY)
    {
        float scale = GetRangeScale();
        if (scale > 0.0f)
        {
            *CameraRangeMaxX *= scale;
            *CameraRangeMaxY *= scale;
        }
    }
}

/* ============================================================ */
/*  SAUtils callbacks                                            */
/* ============================================================ */
void RealDrawDistanceChanged(int oldVal, int newVal, void* data)
{
    (void)oldVal; (void)data;
    pDrawDistanceOverride->SetInt(newVal);
    cfg->Save();
    UpdateRenderingPatches((float)newVal);
}

const char* RealDrawDistanceDraw(int nNewValue, void* data)
{
    (void)data;
    snprintf(szRetDrawDistanceSlider, sizeof(szRetDrawDistanceSlider),
             "x%.2f", (nNewValue / DEFAULT_DRAWDISTANCE));
    return szRetDrawDistanceSlider;
}

void StreamingDistanceChanged(int oldVal, int newVal, void* data)
{
    (void)oldVal; (void)data;
    pStreamingDistanceScale->SetFloat(0.01f * newVal);
    cfg->Save();
}

const char* StreamingDistanceDraw(int nNewValue, void* data)
{
    (void)data;
    snprintf(szRetScaleSlider, sizeof(szRetScaleSlider),
             "x%.2f", (nNewValue / 100.0f));
    return szRetScaleSlider;
}

/* ============================================================ */
/*  Mod entry point                                              */
/* ============================================================ */
extern "C" void OnModLoad()
{
    logger->SetTag("GTASA Draw Distance");

    pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) return;

    void* hGTASA = aml->GetLibHandle("libGTASA.so");
    if (!hGTASA) return;

    // Get pool symbols
    pBuildingPool = (CPool<CBuilding>**)aml->GetSym(hGTASA, "_ZN6CPools16ms_pBuildingPoolE");
    pDummyPool    = (CPool<CDummy>**)aml->GetSym(hGTASA, "_ZN6CPools13ms_pDummyPoolE");

    // Base pointers
    streamingMemoryAvailable = (unsigned int*)(pGTASA + 0x85EBD8);
    streamingMemoryUsed      = (unsigned int*)(pGTASA + 0x9729DC);
    CameraRangeMinX = (float*)(pGTASA + 0xCBE8A4);
    CameraRangeMaxX = (float*)(pGTASA + 0xCBE8A8);
    CameraRangeMinY = (float*)(pGTASA + 0xCBE8AC);
    CameraRangeMaxY = (float*)(pGTASA + 0xCBE8B0);

    // Config bindings
    pNearClipOverride       = cfg->Bind("NearClip", "0.1");
    pDrawDistanceOverride   = cfg->Bind("DrawDistance", "800.0");
    pStreamingDistanceScale = cfg->Bind("StreamingDistanceScale", "1.0");

    // Aspect ratio
    auto CalculateAspectRatio = (CalculateAspectRatioFn)(pGTASA + 0x6C9CC8);
    fRealAspectRatio = CalculateAspectRatio ? CalculateAspectRatio() : 1.0f;
    if (fRealAspectRatio < 1.0f && fRealAspectRatio > 0.0f)
        fRealAspectRatio = 1.0f / fRealAspectRatio;

    // Install hooks
    HOOKSYM(RwCameraSetNearClipPlane, pGTASA, "_Z24RwCameraSetNearClipPlaneP8RwCameraf");
    HOOKSYM(RwCameraSetFarClipPlane,  pGTASA, "_Z23RwCameraSetFarClipPlaneP8RwCameraf");
    HOOKSYM(CameraCreate,             pGTASA, "_Z12CameraCreateiii");
    HOOKSYM(CStreamingUpdate,         pGTASA, "_ZN10CStreaming6UpdateEv");
    HOOKSYM(CPoolsInitialise,         pGTASA, "_ZN6CPools10InitialiseEv");
    HOOKSYM(CWaterLevelSetCameraRange,pGTASA, "_ZN11CWaterLevel14SetCameraRangeEv");

    // Expand visibility/lod pools (optional)
    // Check if the pointers match known addresses and replace with larger arrays
    if (*(uintptr_t*)(pGTASA + 0x84D210) == (pGTASA + 0xBCF900))
    {
        static void** visiblesPool = new void*[12000] {};
        aml->WriteAddr(pGTASA + 0x84D210, (uintptr_t)visiblesPool);
    }
    if (*(uintptr_t*)(pGTASA + 0x84A958) == (pGTASA + 0xBD1840))
    {
        static void** lodsPool = new void*[12000] {};
        aml->WriteAddr(pGTASA + 0x84A958, (uintptr_t)lodsPool);
    }
    if (*(uintptr_t*)(pGTASA + 0x84F738) == (pGTASA + 0xBD3940))
    {
        static void** invisiblesPool = new void*[2000] {};
        aml->WriteAddr(pGTASA + 0x84F738, (uintptr_t)invisiblesPool);
    }

    // Apply initial rendering patches
    UpdateRenderingPatches(pDrawDistanceOverride->GetFloat());

    // SAUtils integration
    sautils = (ISAUtils*)GetInterface("SAUtils");
    if (sautils)
    {
        auto tab = sautils->AddSettingsTab("Draw Distance");

        sautils->AddSliderItem(tab, "Real Draw Distance",
            pDrawDistanceOverride->GetInt(), 200, (int)MAX_DRAWDISTANCE,
            RealDrawDistanceChanged, RealDrawDistanceDraw);

        sautils->AddSliderItem(tab, "Streaming Distance Scale",
            (int)(100.0f * pStreamingDistanceScale->GetFloat()),
            25, MAX_STREAMING_SCALE,
            StreamingDistanceChanged, StreamingDistanceDraw);
    }
}