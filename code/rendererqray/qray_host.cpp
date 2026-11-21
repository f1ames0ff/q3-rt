#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "q_shared.h"
#include "qfiles.h"
#include "surfaceflags.h"
#define USE_RENDERER_DLOPEN
#include "tr_public.h"
}

#include "qray/qray.h"
#include "stb_image.h"

refimport_t ri;

#ifndef _WIN32
// The shader-script and asset code compares Quake 3 names case-insensitively the
// way the original MSVC build did.
static inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;
// QuakeRay's reference host uses 40 Quake units per metre.
constexpr float kQuakeUnitToMeter = 0.025f;
constexpr uint32_t kReadbackFormat = QR_FRAME_READBACK_FORMAT_RGBA8_UNORM;
constexpr uint64_t kDynamicLightUniqueIdBase = 0x100000000ull;
// Q2_MAX_CLUSTERS of the SDK light list tables.
constexpr uint32_t kMaxLightClusters = 8192;

float QuakeUnitsToMeters(float value)
{
    return value * kQuakeUnitToMeter;
}

void QuakePositionToMeters(const float source[3], float destination[3])
{
    destination[0] = QuakeUnitsToMeters(source[0]);
    destination[1] = QuakeUnitsToMeters(source[1]);
    destination[2] = QuakeUnitsToMeters(source[2]);
}

void QuakePositionToMeters(const float source[3], QrFloat3D &destination)
{
    QuakePositionToMeters(source, destination.data);
}

std::vector<QrVertex> QuakeVerticesToMeters(const std::vector<QrVertex> &source)
{
    std::vector<QrVertex> result = source;
    for (QrVertex &vertex : result) {
        for (int axis = 0; axis < 3; ++axis) {
            vertex.position[axis] = QuakeUnitsToMeters(vertex.position[axis]);
        }
    }
    return result;
}

refexport_t re;
glconfig_t glConfig;

QrInstance qrayInstance = QR_NULL_HANDLE;

cvar_t *qrayShaderDir = nullptr;
cvar_t *qrayBlueNoise = nullptr;
cvar_t *qrayWaterNormal = nullptr;
cvar_t *qrayConfig = nullptr;
cvar_t *qraySunColor = nullptr;
cvar_t *qraySkybox = nullptr;
cvar_t *qraySkyboxRadius = nullptr;
cvar_t *qrayTonemapType = nullptr;
cvar_t *qrayTonemapPower = nullptr;
cvar_t *qrayWhitePoint = nullptr;
cvar_t *qrayExposureBias = nullptr;
cvar_t *qrayEyeAdaptation = nullptr;
cvar_t *qrayEnvShellTrace = nullptr;
cvar_t *qrayEnvMapRaster = nullptr;
cvar_t *qrayEnvMapScale = nullptr;
cvar_t *qrayEnvMapSaturation = nullptr;
cvar_t *qrayEnvShellScale = nullptr;
cvar_t *qrayLightScale = nullptr;
cvar_t *qrayLightRadiusScale = nullptr;
cvar_t *qrayLightRadius = nullptr;
cvar_t *qrayLightTopUp = nullptr;
cvar_t *qrayDlightScale = nullptr;
cvar_t *qrayViewMode = nullptr;
cvar_t *qrayModelWinding = nullptr;
cvar_t *qrayCullBackFaces = nullptr;
cvar_t *qrayDecalOffset = nullptr;
cvar_t *qrayDebugEntities = nullptr;
cvar_t *qrayNormalMode = nullptr;
cvar_t *qrayTestQuad = nullptr;
cvar_t *qrayPolyDebug = nullptr;
cvar_t *qraySkipEntities = nullptr;
cvar_t *qrayAlphaTest = nullptr;
cvar_t *qrayGiLevel = nullptr;
cvar_t *qrayDenoiser = nullptr;
cvar_t *qrayNeeSamples = nullptr;
cvar_t *qrayRestir = nullptr;
cvar_t *qrayRestirCandidates = nullptr;
cvar_t *qrayCellWorldSize = nullptr;
cvar_t *qraySunBounceRange = nullptr;
cvar_t *qrayPhysicalSky = nullptr;
cvar_t *qrayPhysicalSkySun = nullptr;
cvar_t *qraySkyColor = nullptr;
cvar_t *qraySunDiscSize = nullptr;
cvar_t *qrayClouds = nullptr;
cvar_t *qrayCloudColor = nullptr;
cvar_t *qrayCloudCoverage = nullptr;
cvar_t *qrayCloudDensity = nullptr;
cvar_t *qrayCloudSpeed = nullptr;
cvar_t *qrayCloudHeight = nullptr;
cvar_t *qrayCloudThickness = nullptr;
cvar_t *qrayCloudOpacity = nullptr;
cvar_t *qrayCloudQuality = nullptr;
cvar_t *qrayGodRays = nullptr;
cvar_t *qrayGodRaysIntensity = nullptr;
cvar_t *qrayGodRaysQuality = nullptr;
cvar_t *qrayUncapped = nullptr;
cvar_t *qrayPhysicalSkySunPitch = nullptr;
cvar_t *qrayPhysicalSkyYaw = nullptr;
cvar_t *qrayPhysicalSkyPower = nullptr;
cvar_t *qraySkyAmbientLod = nullptr;
cvar_t *qraySkyLightMultiplier = nullptr;
cvar_t *qrayIndirectHalfRes = nullptr;

QrMaterial defaultMaterial = QR_NO_MATERIAL;
std::vector<QrMaterial> shaderMaterials;
std::vector<uint8_t> shaderMaterialAttempted;

struct MapLight
{
    float position[3];
    float color[3];
    float radius;
};

struct DynamicLight
{
    float origin[3];
    float color[3];
    float radius;
    bool additive;
    bool linear;
};

struct GeometryBatch
{
    QrMaterial material = QR_NO_MATERIAL;
    bool alphaTested = false;
    std::vector<QrVertex> vertices;
    // Parallel to vertices. The renderer batches by shader, so triangles from many BSP
    // surfaces share one upload. The debug picker still needs the original dsurface_t.
    std::vector<int32_t> sourceSurfaceIndices;
    std::vector<uint32_t> indices;
};

struct WorldSurfaceInfo
{
    bool valid = false;
    int shaderNum = -1;
    int surfaceType = MST_BAD;
    int surfaceFlags = 0;
    int contentFlags = 0;
    int firstVert = 0;
    int numVerts = 0;
    int firstIndex = 0;
    int numIndexes = 0;
    int lightmapNum = 0;
    int lightmapWidth = 0;
    int lightmapHeight = 0;
    int patchWidth = 0;
    int patchHeight = 0;
    float planeDist = 0.0f;
    float planeNormal[3] = {0.0f, 0.0f, 0.0f};
    float generatedNormal[3] = {0.0f, 0.0f, 0.0f};
    float authoredNormal[3] = {0.0f, 0.0f, 0.0f};
    float originalWindingNormal[3] = {0.0f, 0.0f, 0.0f};
    float uploadedWindingNormal[3] = {0.0f, 0.0f, 0.0f};
    bool alphaTested = false;
    bool windingFlipped = false;
    bool planarWindingKept = false;
};

std::vector<MapLight> mapLights;
std::vector<DynamicLight> dynamicLights;
std::vector<WorldSurfaceInfo> worldSurfaceInfos;
std::vector<std::string> worldShaderNames;

struct BspClusterTree
{
    struct Node
    {
        float normal[3];
        float dist;
        int children[2];
    };

    std::vector<Node> nodes;
    std::vector<int> leafClusters;

    int ClusterForPoint(const float point[3]) const
    {
        if (nodes.empty()) {
            return -1;
        }

        int index = 0;
        for (int depth = 0; depth < 1024; ++depth) {
            const Node &node = nodes[static_cast<size_t>(index)];
            const float distance = node.normal[0] * point[0] + node.normal[1] * point[1] +
                node.normal[2] * point[2] - node.dist;
            const int child = node.children[distance < 0.0f ? 1 : 0];
            if (child >= 0) {
                if (child >= static_cast<int>(nodes.size())) {
                    return -1;
                }
                index = child;
                continue;
            }

            const int leaf = -(child + 1);
            return (leaf >= 0 && leaf < static_cast<int>(leafClusters.size()))
                ? leafClusters[static_cast<size_t>(leaf)]
                : -1;
        }

        return -1;
    }
};

struct WorldClusterTables
{
    bool uploaded = false;
    uint32_t clusterCount = 0;
    uint32_t pvsRowBytes = 0;
    std::vector<QrFloat3D> mins;
    std::vector<QrFloat3D> maxs;
    std::vector<uint8_t> flags;
    std::vector<uint8_t> bounded;
    std::vector<int32_t> visOffsets;
    std::vector<uint8_t> visData;
    std::vector<uint8_t> skyVis;
    std::vector<uint8_t> skyFlags;
};

BspClusterTree bspClusterTree;
WorldClusterTables worldClusters;
std::vector<uint32_t> mapLightClusters;
uint32_t worldClusterGeneration = 0;


std::unordered_map<qhandle_t, QrMaterial> uiMaterials;
std::unordered_map<qhandle_t, std::string> uiShaderNames;
std::unordered_map<std::string, qhandle_t> uiShaderHandles;
qhandle_t nextUiHandle = 50000;
float currentColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

struct ModelSurface
{
    std::string shaderName;
    std::vector<uint32_t> indices;
    std::vector<float> texCoords;
    std::vector<float> positions;
    std::vector<float> normals;
    int numVerts = 0;
};

struct ModelAsset
{
    bool valid = false;
    int numFrames = 0;
    int numTags = 0;
    float bounds[2][3] = {};
    float radius = 0.0f;
    std::vector<ModelSurface> surfaces;
    std::vector<md3Tag_t> tags;
};

struct SkinAsset
{
    std::unordered_map<std::string, std::string> shaderBySurface;
};

enum ShaderWaveType
{
    SHADER_WAVE_NONE,
    SHADER_WAVE_SIN,
    SHADER_WAVE_SQUARE,
    SHADER_WAVE_TRIANGLE,
    SHADER_WAVE_SAWTOOTH,
    SHADER_WAVE_INVERSE_SAWTOOTH,
    SHADER_WAVE_NOISE
};

struct ShaderStage
{
    std::string map;
    std::vector<std::string> animationMaps;
    float animationFps = 0.0f;
    QrGeometryMaterialBlendType blend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    QrBlendFactor blendSrc = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDst = QR_BLEND_FACTOR_ZERO;
    bool blended = false;
    bool darkenDecal = false;
    bool alphaTested = false;
    bool environmentMap = false;
    bool lightmap = false;
    bool dynamic = false;
    bool animated = false;
    float scroll[2] = {0.0f, 0.0f};
    ShaderWaveType rgbWave = SHADER_WAVE_NONE;
    float rgbWaveBase = 1.0f;
    float rgbWaveAmplitude = 0.0f;
    float rgbWavePhase = 0.0f;
    float rgbWaveFrequency = 0.0f;
    ShaderWaveType alphaWave = SHADER_WAVE_NONE;
    float alphaWaveBase = 1.0f;
    float alphaWaveAmplitude = 0.0f;
    float alphaWavePhase = 0.0f;
    float alphaWaveFrequency = 0.0f;
};

struct ShaderDefinition
{
    std::string map;
    QrGeometryMaterialBlendType blend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    QrBlendFactor blendSrc = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDst = QR_BLEND_FACTOR_ZERO;
    bool darkenDecal = false;
    bool alphaTested = false;
    // The chosen stage is a tcGen environment pass, so its texcoords come from the eye
    // reflection vector instead of the surface UVs.
    bool environmentMap = false;
    bool usesOverlay = false;
    bool dynamic = false;
    bool transparentSurface = false;
    bool tracedBase = false;
    size_t baseStageIndex = static_cast<size_t>(-1);
    std::vector<ShaderStage> stages;
};

// Q3 skies are either a six sided box (skyparms <box> ...) or a stack of scrolling
// cloud layers projected onto a dome. Both are baked into a cubemap on map load.
struct SkyStage
{
    std::string map;
    float scale[2] = {1.0f, 1.0f};
    bool additive = false;
};

struct SkyShaderInfo
{
    std::string outerBox;
    float cloudHeight = 512.0f;
    std::vector<SkyStage> stages;
    // q3map_sun, taken from the sky shader of the loaded map. Retail Q3 bakes it into the
    // lightmaps, an RT renderer has to place the sun itself.
    bool hasSun = false;
    float sunColor[3] = {1.0f, 1.0f, 1.0f};
    float sunIntensity = 0.0f;
    float sunDegrees = 0.0f;
    float sunElevation = 0.0f;
};

std::unordered_map<qhandle_t, ModelAsset> modelAssets;
std::unordered_map<std::string, qhandle_t> modelHandles;
std::unordered_map<std::string, QrMaterial> textureMaterials;
std::unordered_map<std::string, QrMaterial> textureMaterialsRaw;
std::unordered_map<qhandle_t, QrMaterial> uiMaterialsRaw;
std::unordered_map<std::string, QrMaterial> scaledEnvMaterials;
// The native window outlives the QuakeRay instance across REF_KEEP_WINDOW restarts.
bool windowInitialized = false;
std::unordered_map<std::string, bool> textureHasAlpha;
std::unordered_map<std::string, ShaderDefinition> shaderDefinitions;
std::unordered_map<std::string, SkyShaderInfo> skyShaders;

struct WorldOverlayBatch
{
    std::string shaderName;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
};

std::vector<WorldOverlayBatch> worldOverlayBatches;

std::string mapSkyShaderName;
QrCubemap skyCubemap = QR_EMPTY_CUBEMAP;
float skyAverageColor[3] = {0.20f, 0.25f, 0.35f};
// QuakeRay only samples the cubemap its own sky pass rasterizes, so the sky of the map is also
// composed into a flat atlas here and drawn as six inward facing quads by the overlay pass.
QrMaterial skyAtlasMaterial = QR_NO_MATERIAL;
int skyAtlasFaceSize = 0;
bool skyAtlasAttempted = false;
std::unordered_map<qhandle_t, SkinAsset> skinAssets;
std::unordered_map<std::string, qhandle_t> skinHandles;
qhandle_t nextModelHandle = 1000;
qhandle_t nextSkinHandle = 20000;
bool shaderScriptsLoaded = false;

struct PendingPoly
{
    qhandle_t shader = 0;
    std::vector<polyVert_t> verts;
};

// Impact marks are built by clipping a projected polygon against the world, so the host needs
// its own copy of the world triangles plus a coarse grid to keep the per-mark ray casts cheap.
struct MarkTriangle
{
    float points[3][3] = {};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float texCoords[3][2] = {};
    float vertexNormals[3][3] = {};
    int32_t sourceSurfaceIndex = -1;
    uint32_t sourceTriangleIndex = 0;
};

std::vector<MarkTriangle> markTriangles;
std::unordered_map<int64_t, std::vector<uint32_t>> markGrid;
constexpr float kMarkCellSize = 256.0f;

int64_t MarkCellKey(int x, int y, int z)
{
    return static_cast<int64_t>(x) * 73856093ll ^ static_cast<int64_t>(y) * 19349663ll ^
        static_cast<int64_t>(z) * 83492791ll;
}

std::vector<refEntity_t> pendingEntities;
std::vector<PendingPoly> pendingPolys;
uint64_t dynamicGeometryId = 0x4000000000000000ull;

void NormalizeVector(float vector[3]);

bool frameStarted = false;
bool sceneValid = false;
bool sceneRendered = false;
bool frameHasWorldScene = false;
refdef_t sceneDef;
refdef_t probeSceneDef;
bool probeSceneValid = false;
std::string pendingWorldName;
bool worldLoaded = false;
float worldMin[3] = {1.0e30f, 1.0e30f, 1.0e30f};
float worldMax[3] = {-1.0e30f, -1.0e30f, -1.0e30f};

std::vector<uint8_t> readbackBuffer;
std::vector<uint8_t> readbackReadyPixels;
uint32_t readbackWidth = 0;
uint32_t readbackHeight = 0;
uint32_t readbackRowPitch = 0;
uint64_t readbackBufferSize = 0;
uint64_t readbackRequest = 0;
bool readbackPending = false;
bool readbackValid = false;
// Full-frame readbacks cost a GPU copy plus a CPU copy every frame, so they only run
// while a consumer (AVI capture, qray_readback) asked for one recently.
constexpr int kReadbackKeepAliveFrames = 120;
int readbackWantedFrames = 0;

bool capturePending = false;
int captureWidth = 0;
int captureHeight = 0;
byte *captureBuffer = nullptr;
byte *captureEncodeBuffer = nullptr;
bool captureMotionJpeg = false;

bool commandsRegistered = false;
bool hostLogEnabled = false;

void QrayScreenshot_f();
void QrayReadbackDump_f();
void QrayProbe_f();
bool RayIntersectsTriangle(const MarkTriangle &triangle, const float origin[3],
    const float direction[3], float maxDistance, float &hitDistance,
    float *outU, float *outV);

// Directory for the diagnostic logs: %TEMP% on Windows, $TMPDIR or /tmp elsewhere.
bool TempFilePath(const char *fileName, char *out, size_t outSize)
{
    if (out == nullptr || outSize == 0) {
        return false;
    }
    out[0] = '\0';
#ifdef _WIN32
    char directory[MAX_PATH] = {};
    const DWORD length = GetTempPathA(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const int written = std::snprintf(out, outSize, "%s%s", directory, fileName);
#else
    const char *directory = std::getenv("TMPDIR");
    if (directory == nullptr || directory[0] == '\0') {
        directory = "/tmp";
    }
    const size_t directoryLength = std::strlen(directory);
    const bool needsSeparator = directoryLength > 0 && directory[directoryLength - 1] != '/';
    const int written = std::snprintf(out, outSize, "%s%s%s", directory, needsSeparator ? "/" : "", fileName);
#endif
    if (written <= 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return false;
    }
    return true;
}

void HostLog(const char *format, ...)
{
    if (!hostLogEnabled) {
        return;
    }

    static char path[1024] = {};
    if (path[0] == '\0' && !TempFilePath("qray_host.log", path, sizeof(path))) {
        return;
    }

    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        return;
    }

    char message[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    stream << message << "\n";
}

const char *ProbeLogPath()
{
    static char path[1024] = {};
    if (path[0] == '\0') {
        TempFilePath("qray_probe.log", path, sizeof(path));
    }
    return path;
}

void ProbeLog(const char *format, ...)
{
    const char *path = ProbeLogPath();
    if (path[0] == '\0') {
        return;
    }

    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        return;
    }

    char message[4096] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char timestamp[64] = {};
#ifdef _WIN32
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
#else
    struct timeval now {};
    gettimeofday(&now, nullptr);
    struct tm local {};
    const time_t seconds = now.tv_sec;
    localtime_r(&seconds, &local);
    std::snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
        local.tm_sec, static_cast<int>(now.tv_usec / 1000));
#endif
    stream << timestamp << " " << message << "\n";
}

const char *SurfaceTypeName(int surfaceType)
{
    switch (surfaceType) {
    case MST_PLANAR:
        return "PLANAR";
    case MST_PATCH:
        return "PATCH";
    case MST_TRIANGLE_SOUP:
        return "TRIANGLE_SOUP";
    case MST_FLARE:
        return "FLARE";
    default:
        return "BAD";
    }
}

#ifdef _WIN32
int HostExceptionFilter(EXCEPTION_POINTERS *exceptionInfo)
{
    if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr) {
        HostLog("qray host exception 0x%08lx", static_cast<unsigned long>(exceptionInfo->ExceptionRecord->ExceptionCode));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

QrResult CreateInstanceProtected(const QrInstanceCreateInfo *info, QrInstance *instance)
{
    __try {
        return qrCreateInstance(info, instance);
    } __except (HostExceptionFilter(GetExceptionInformation())) {
        *instance = QR_NULL_HANDLE;
        return QR_GRAPHICS_API_ERROR;
    }
}
#else
// There is no structured exception handling outside MSVC; C++ exceptions are still
// caught by the caller.
QrResult CreateInstanceProtected(const QrInstanceCreateInfo *info, QrInstance *instance)
{
    return qrCreateInstance(info, instance);
}
#endif

const char *CvarText(cvar_t *cvar, const char *fallback)
{
    if (cvar == nullptr || cvar->string == nullptr || cvar->string[0] == '\0') {
        return fallback;
    }
    return cvar->string;
}

float CvarFloat(cvar_t *cvar, float fallback)
{
    return cvar != nullptr ? cvar->value : fallback;
}

void CvarColor(cvar_t *cvar, const float fallback[3], QrFloat3D &color)
{
    color.data[0] = fallback[0];
    color.data[1] = fallback[1];
    color.data[2] = fallback[2];
    if (cvar == nullptr || cvar->string == nullptr || cvar->string[0] == '\0') {
        return;
    }

    float parsed[3] = {};
    if (std::sscanf(cvar->string, "%f %f %f", &parsed[0], &parsed[1], &parsed[2]) == 3) {
        const bool usesByteRange = parsed[0] > 1.0f || parsed[1] > 1.0f || parsed[2] > 1.0f;
        const float scale = usesByteRange ? 1.0f / 255.0f : 1.0f;
        color.data[0] = std::clamp(parsed[0] * scale, 0.0f, 1.0f);
        color.data[1] = std::clamp(parsed[1] * scale, 0.0f, 1.0f);
        color.data[2] = std::clamp(parsed[2] * scale, 0.0f, 1.0f);
    }
}

void CopyString(char *destination, const char *source, size_t size)
{
    if (destination == nullptr || size == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    std::strncpy(destination, source, size - 1);
    destination[size - 1] = '\0';
}

void QrayPrint(const char *message, void *userData)
{
    (void)userData;
    if (message != nullptr) {
        HostLog("qray: %s", message);
        ri.Printf(PRINT_ALL, "%s", message);
    }
}

struct LoadedFile
{
    std::vector<uint8_t> data;
    void *q3Data;
};

void QrayOpenFile(const char *filePath, void *userData, const void **outData, uint32_t *outSize, void **outHandle)
{
    (void)userData;
    *outData = nullptr;
    *outSize = 0;
    *outHandle = nullptr;

    if (filePath == nullptr || filePath[0] == '\0') {
        return;
    }

    std::ifstream stream(filePath, std::ios::binary | std::ios::ate);
    if (stream.is_open()) {
        const std::streamsize length = stream.tellg();
        if (length > 0) {
            auto *file = new LoadedFile();
            file->q3Data = nullptr;
            file->data.resize(static_cast<size_t>(length));
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char *>(file->data.data()), length);
            if (stream.good() || stream.eof()) {
                *outData = file->data.data();
                *outSize = static_cast<uint32_t>(file->data.size());
                *outHandle = file;
                return;
            }
            delete file;
        }
    }

    void *buffer = nullptr;
    const int length = ri.FS_ReadFile(filePath, &buffer);
    if (length > 0) {
        auto *file = new LoadedFile();
        file->q3Data = buffer;
        *outData = buffer;
        *outSize = static_cast<uint32_t>(length);
        *outHandle = file;
    }
}

void QrayCloseFile(void *fileHandle, void *userData)
{
    (void)userData;
    auto *file = static_cast<LoadedFile *>(fileHandle);
    if (file == nullptr) {
        return;
    }
    if (file->q3Data != nullptr) {
        ri.FS_FreeFile(file->q3Data);
    }
    delete file;
}

void RegisterCvars()
{
    cvar_t *debugLog = ri.Cvar_Get("qray_debugLog", "0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(debugLog, "Write QuakeRay host diagnostics to the system temporary directory.");
    hostLogEnabled = debugLog->integer != 0;

    qrayShaderDir = ri.Cvar_Get("qray_shaderDir", "qray/shaders/", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayShaderDir, "Directory containing the compiled QuakeRay SPIR-V shaders.");

    qrayBlueNoise = ri.Cvar_Get("qray_blueNoise", "qray/blue_noise.png", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayBlueNoise, "Path to the QuakeRay blue noise texture.");

    qrayWaterNormal = ri.Cvar_Get("qray_waterNormal", "", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayWaterNormal, "Optional path to the QuakeRay water normal texture.");

    qrayConfig = ri.Cvar_Get("qray_config", "qray.cfg", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayConfig, "Path to the QuakeRay renderer settings file.");

    qraySunColor = ri.Cvar_Get("qray_sunColor", "", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySunColor,
        "QuakeRay directional light color; empty uses the q3map_sun of the map.");

    // The SDK only applies a tonemap operator when the draw info carries the parameters, so
    // without these it falls back to a white point of one and clips every highlight.
    qrayTonemapType = ri.Cvar_Get("qray_tonemapType", "4", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayTonemapType,
        "QuakeRay tonemap operator: 0 none, 1 Reinhard, 2 extended Reinhard, 3 ACES, 4 AgX.");

    qrayTonemapPower = ri.Cvar_Get("qray_tonemapPower", "0.9", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayTonemapPower,
        "Blend towards the tonemap operator; 0 keeps the filmic curve alone.");

    qrayWhitePoint = ri.Cvar_Get("qray_whitePoint", "10.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayWhitePoint,
        "Luminance mapped to white by the tonemap operator, higher values keep more highlights.");

    qrayExposureBias = ri.Cvar_Get("qray_exposureBias", "-2.5", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayExposureBias,
        "Exposure bias in stops applied by QuakeRay; -2.5 matches the baked Quake 3 brightness.");

    qrayEyeAdaptation = ri.Cvar_Get("qray_eyeAdaptation", "0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEyeAdaptation,
        "Let QuakeRay adapt exposure to the frame; Quake 3 kept a fixed exposure, so automatic adaptation shifts the look.");

    qrayEnvShellTrace = ri.Cvar_Get("qray_envShellTrace", "0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEnvShellTrace,
        "Draw additive environment shells (powerup glass) through the ray tracer instead of the unlit overlay; 2 skips them.");

    qrayEnvMapRaster = ri.Cvar_Get("qray_envMapRaster", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEnvMapRaster,
        "Draw opaque environment map stages (powerup bodies) as unlit overlays, the way the fixed function pipeline did.");

    qrayEnvMapScale = ri.Cvar_Get("qray_envMapScale", "1.15", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEnvMapScale,
        "Brightness of the opaque environment map stages; the baked maps read darker than in the retail frame buffer.");

    qrayEnvMapSaturation = ri.Cvar_Get("qray_envMapSaturation", "1.6", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEnvMapSaturation,
        "Saturation of the opaque environment map stages; the additive shell desaturates the powerup bodies in the HDR pipeline.");

    qrayEnvShellScale = ri.Cvar_Get("qray_envShellScale", "0.1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayEnvShellScale,
        "Brightness of the additive glass shells; the fixed function blend overshoots in the HDR pipeline.");

    qraySkybox = ri.Cvar_Get("qray_skybox", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySkybox, "Draw the sky of the map as rasterized geometry.");

    qraySkyboxRadius = ri.Cvar_Get("qray_skyboxRadius", "2000", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySkyboxRadius, "Distance of the rasterized sky box in map units.");

    qrayLightScale = ri.Cvar_Get("qray_lightScale", "200", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightScale, "Intensity scale applied to Quake 3 map light entities.");

    qrayLightRadiusScale = ri.Cvar_Get("qray_lightRadiusScale", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightRadiusScale, "Radius scale applied to Quake 3 map light entities.");

    qrayLightRadius = ri.Cvar_Get("qray_lightRadius", "16", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightRadius,
        "Emission sphere radius of a Quake 3 map light, in Quake units. Small values behave like point lights.");

    qrayLightTopUp = ri.Cvar_Get("qray_lightTopUp", "1024", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightTopUp,
        "Distance in Quake units within which clusters receive lights their PVS row does not list.");

    qrayDlightScale = ri.Cvar_Get("qray_dlightScale", "200", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayDlightScale,
        "Intensity scale applied to per-frame Quake 3 dynamic lights.");

    qrayViewMode = ri.Cvar_Get("qray_viewMode", "7", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayViewMode, "Debug camera matrix layout selection for QuakeRay.");

    qrayModelWinding = ri.Cvar_Get("qray_modelWinding", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayModelWinding, "Reverse MD3 triangle winding for QuakeRay ray culling.");

    qrayCullBackFaces = ri.Cvar_Get("qray_cullBackFaces", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCullBackFaces, "Cull back-facing triangles in QuakeRay.");

    qrayDecalOffset = ri.Cvar_Get("qray_decalOffset", "0.25", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayDecalOffset,
        "Distance polygon effects (bullet marks, scorches) are pushed off the surface they sit on.");

    qrayDebugEntities = ri.Cvar_Get("qray_debugEntities", "0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayDebugEntities,
        "Log entity material resolution to the host log (requires qray_log).");

    qrayNormalMode = ri.Cvar_Get("qray_normalMode", "0", CVAR_TEMP);
    ri.Cvar_SetDescription(qrayNormalMode,
        "Debug: 0 = flipped winding + generated normals, 1 = flipped winding + "
        "generated inverted normals, 2 = original winding + generated normals, "
        "3 = original winding + generated inverted normals.");

    qrayTestQuad = ri.Cvar_Get("qray_testQuad", "0", CVAR_TEMP);
    ri.Cvar_SetDescription(qrayTestQuad,
        "Debug: draw an unlit quad in front of the camera through the rasterized overlay.");

    qrayPolyDebug = ri.Cvar_Get("qray_polyDebug", "0", CVAR_TEMP);
    ri.Cvar_SetDescription(qrayPolyDebug,
        "Debug: 0 = normal decal path, 1 = decals as opaque default-material quads, "
        "2 = decals with blend but no depth test.");

    qraySkipEntities = ri.Cvar_Get("qray_skipEntities", "0", CVAR_TEMP);
    ri.Cvar_SetDescription(qraySkipEntities,
        "Debug: bit 1 = models, bit 2 = sprites, bit 4 = beams, bit 8 = polygons.");

    qrayAlphaTest = ri.Cvar_Get("qray_alphaTest", "1", CVAR_TEMP);
    ri.Cvar_SetDescription(qrayAlphaTest,
        "Debug: 1 = honour alphaFunc stages through the alpha tested hit group, 0 = opaque.");

    qrayGiLevel = ri.Cvar_Get("qray_giLevel", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayGiLevel,
        "Indirect lighting level: 0 disables GI, 1 uses one bounce, 2 enables the second bounce.");

    qrayDenoiser = ri.Cvar_Get("qray_denoiser", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayDenoiser, "Enable the QuakeRay temporal denoiser.");

    qrayNeeSamples = ri.Cvar_Get("qray_neeSamples", "2", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayNeeSamples, "Direct-light samples per pixel: 1 or 2.");

    qrayRestir = ri.Cvar_Get("qray_restir", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayRestir,
        "Use ReSTIR over the global light list instead of per-cluster light sampling.");

    qrayRestirCandidates = ri.Cvar_Get("qray_restirCandidates", "8", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayRestirCandidates, "ReSTIR candidates per light sample, from 1 to 64.");

    qrayCellWorldSize = ri.Cvar_Get("qray_cellWorldSize", "2.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCellWorldSize, "ReSTIR and GI spatial cell size in metres.");

    qraySunBounceRange = ri.Cvar_Get("qray_sunBounceRange", "50.8", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySunBounceRange,
        "Sun reach into an indirect bounce in metres; 0 disables indirect sunlight.");

    qrayPhysicalSky = ri.Cvar_Get("qray_physicalSky", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayPhysicalSky,
        "Use QuakeRay's procedural sky instead of the map's rasterized sky cubemap.");

    qrayPhysicalSkySun = ri.Cvar_Get("qray_physicalSkySun", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayPhysicalSkySun,
        "Upload the map sun as the physical directional light and procedural sun disc.");

    qraySkyColor = ri.Cvar_Get("qray_skyColor", "auto", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySkyColor,
        "Procedural sky colour as R G B in the 0..255 range; auto derives the tint from "
        "the map sky and uses QuakeRay's blue profile for black space skies.");

    qraySunDiscSize = ri.Cvar_Get("qray_sunDiscSize", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySunDiscSize, "Scale of the visible procedural sun disc.");

    qrayClouds = ri.Cvar_Get("qray_clouds", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayClouds, "Enable QuakeRay's volumetric cloud layer.");

    qrayCloudColor = ri.Cvar_Get("qray_cloudColor", "255 255 255", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudColor,
        "Cloud colour as R G B in the 0..255 range; legacy 0..1 values are accepted.");

    qrayCloudCoverage = ri.Cvar_Get("qray_cloudCoverage", "0.2", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudCoverage, "Volumetric cloud coverage in the range 0..1.");

    qrayCloudDensity = ri.Cvar_Get("qray_cloudDensity", "0.8", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudDensity, "Volumetric cloud density.");

    qrayCloudSpeed = ri.Cvar_Get("qray_cloudSpeed", "0.3", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudSpeed, "Volumetric cloud motion rate.");

    qrayCloudHeight = ri.Cvar_Get("qray_cloudHeight", "3500", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudHeight, "Cloud layer base altitude in metres.");

    qrayCloudThickness = ri.Cvar_Get("qray_cloudThickness", "2250", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudThickness, "Cloud layer thickness in metres.");

    qrayCloudOpacity = ri.Cvar_Get("qray_cloudOpacity", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudOpacity, "Cloud compositing opacity in the range 0..1.");

    qrayCloudQuality = ri.Cvar_Get("qray_cloudQuality", "2", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCloudQuality, "Cloud quality: 0 low through 3 ultra.");

    qrayGodRays = ri.Cvar_Get("qray_godRays", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayGodRays, "Enable procedural sun shafts.");

    qrayGodRaysIntensity = ri.Cvar_Get("qray_godRaysIntensity", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayGodRaysIntensity, "Procedural sun-shaft intensity.");

    qrayGodRaysQuality = ri.Cvar_Get("qray_godRaysQuality", "2", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayGodRaysQuality, "Procedural sun-shaft quality.");

    qrayUncapped = ri.Cvar_Get("qray_uncapped", "0", CVAR_TEMP);
    ri.Cvar_SetDescription(qrayUncapped,
        "Benchmark only: use IMMEDIATE present and zero present wait instead of FIFO with latency 1.");

    qrayPhysicalSkySunPitch = ri.Cvar_Get("qray_physicalSkySunPitch", "-1000", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayPhysicalSkySunPitch,
        "Physical sun elevation in degrees; -1000 uses the map q3map_sun.");

    qrayPhysicalSkyYaw = ri.Cvar_Get("qray_physicalSkyYaw", "-1000", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayPhysicalSkyYaw,
        "Physical sun yaw in degrees; -1000 uses the map q3map_sun.");

    qrayPhysicalSkyPower = ri.Cvar_Get("qray_physicalSkyPower", "3", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayPhysicalSkyPower,
        "Physical sun power from 0 to 100; 0 keeps the sun disabled.");

    qraySkyAmbientLod = ri.Cvar_Get("qray_skyAmbientLod", "4", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySkyAmbientLod,
        "Cubemap/procedural-sky mip level used for ambient lighting.");

    qraySkyLightMultiplier = ri.Cvar_Get("qray_skyLightMultiplier", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySkyLightMultiplier,
        "Multiplier on the light physical sky contributes to the scene, from 0 to 8.");

    qrayIndirectHalfRes = ri.Cvar_Get("qray_indirectHalfRes", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayIndirectHalfRes,
        "Trace indirect lighting at half resolution while keeping the primary visibility pass full resolution.");
}

bool CreateDefaultMaterial()
{
    static uint8_t albedo[4] = {255, 255, 255, 255};
    static uint8_t roughMetalEmission[4] = {160, 0, 0, 255};
    static uint8_t normal[4] = {128, 128, 255, 255};

    QrMaterialCreateInfo info{};
    info.size = {1, 1};
    info.textures.pDataAlbedoAlpha = albedo;
    info.textures.pDataRoughnessMetallicEmission = roughMetalEmission;
    info.textures.pDataNormal = normal;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    QrResult result = qrCreateMaterial(qrayInstance, &info, &defaultMaterial);
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrCreateMaterial failed: %d\n", static_cast<int>(result));
        defaultMaterial = QR_NO_MATERIAL;
        return false;
    }
    return true;
}

bool LoadQ3Texture(const char *shaderName, int &width, int &height, std::vector<uint8_t> &pixels){
    width = 0;
    height = 0;
    pixels.clear();

    if (shaderName == nullptr || shaderName[0] == '\0' || shaderName[0] == '$') {
        return false;
    }

    std::vector<std::string> candidates;
    const char *lastDot = std::strrchr(shaderName, '.');
    const char *lastSlash = std::strrchr(shaderName, '/');
    if (lastDot != nullptr && (lastSlash == nullptr || lastDot > lastSlash)) {
        const std::string base(shaderName, static_cast<size_t>(lastDot - shaderName));
        candidates.emplace_back(shaderName);
        candidates.emplace_back(base + ".tga");
        candidates.emplace_back(base + ".jpg");
        candidates.emplace_back(base + ".jpeg");
        candidates.emplace_back(base + ".png");
    } else {
        candidates.emplace_back(std::string(shaderName) + ".tga");
        candidates.emplace_back(std::string(shaderName) + ".jpg");
        candidates.emplace_back(std::string(shaderName) + ".jpeg");
        candidates.emplace_back(std::string(shaderName) + ".png");
    }

    for (const std::string &candidate : candidates) {
        void *fileData = nullptr;
        const int fileLength = ri.FS_ReadFile(candidate.c_str(), &fileData);
        if (fileLength <= 0 || fileData == nullptr) {
            continue;
        }

        int decodedWidth = 0;
        int decodedHeight = 0;
        int channels = 0;
        stbi_uc *decoded = stbi_load_from_memory(
            static_cast<const stbi_uc *>(fileData), fileLength,
            &decodedWidth, &decodedHeight, &channels, 4);
        ri.FS_FreeFile(fileData);

        if (decoded != nullptr && decodedWidth > 0 && decodedHeight > 0) {
            width = decodedWidth;
            height = decodedHeight;
            pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
            stbi_image_free(decoded);
            std::string key = candidate;
            for (char &character : key) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            bool hasTransparentPixel = false;
            if (channels == 4 || channels == 2) {
                for (size_t index = 3; index < pixels.size(); index += 4) {
                    if (pixels[index] < 250) {
                        hasTransparentPixel = true;
                        break;
                    }
                }
            }
            // Several retail effect TGAs are 32-bit even though every texel is opaque. Treating
            // the mere presence of an alpha channel as a transparency mask leaves their black
            // background as a solid card instead of selecting the original additive blend.
            textureHasAlpha[key] = hasTransparentPixel;
            return true;
        }
        if (decoded != nullptr) {
            stbi_image_free(decoded);
        }
    }

    return false;
}

QrMaterial CreateMaterialFromPixels(const char *name, int width, int height,
    const std::vector<uint8_t> &albedo, QrSamplerAddressMode addressMode)
{
    if (width <= 0 || height <= 0 || albedo.size() < static_cast<size_t>(width) * height * 4) {
        return QR_NO_MATERIAL;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> roughnessMetallicEmission(pixelCount * 4, 255);
    std::vector<uint8_t> normal(pixelCount * 4, 255);
    for (size_t i = 0; i < pixelCount; ++i) {
        roughnessMetallicEmission[i * 4 + 0] = 200;
        roughnessMetallicEmission[i * 4 + 1] = 0;
        roughnessMetallicEmission[i * 4 + 2] = 0;
        roughnessMetallicEmission[i * 4 + 3] = 255;

        normal[i * 4 + 0] = 128;
        normal[i * 4 + 1] = 128;
        normal[i * 4 + 2] = 255;
        normal[i * 4 + 3] = 255;
    }

    QrMaterialCreateInfo info{};
    info.size.width = static_cast<uint32_t>(width);
    info.size.height = static_cast<uint32_t>(height);
    info.textures.pDataAlbedoAlpha = albedo.data();
    info.textures.pDataRoughnessMetallicEmission = roughnessMetallicEmission.data();
    info.textures.pDataNormal = normal.data();
    info.pRelativePath = name;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = addressMode;
    info.addressModeV = addressMode;

    QrMaterial material = QR_NO_MATERIAL;
    const QrResult result = qrCreateMaterial(qrayInstance, &info, &material);
    if (result != QR_SUCCESS) {
        HostLog("CreateMaterialFromPixels: %s failed result=%d", name != nullptr ? name : "(null)",
            static_cast<int>(result));
        return QR_NO_MATERIAL;
    }
    return material;
}

std::string NormalizeShaderName(const std::string &name)
{
    std::string result = name;
    for (char &character : result) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const size_t slash = result.find_last_of('/');
    const size_t dot = result.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        result.resize(dot);
    }
    return result;
}

QrGeometryMaterialBlendType BlendTypeForFactors(const std::string &source, const std::string &destination)
{
    const bool sourceOne = _stricmp(source.c_str(), "GL_ONE") == 0;
    const bool sourceAlpha = _stricmp(source.c_str(), "GL_SRC_ALPHA") == 0;
    const bool sourceZero = _stricmp(source.c_str(), "GL_ZERO") == 0;
    const bool destinationOne = _stricmp(destination.c_str(), "GL_ONE") == 0;
    const bool destinationInverseColor = _stricmp(destination.c_str(), "GL_ONE_MINUS_SRC_COLOR") == 0;
    const bool destinationInverseAlpha = _stricmp(destination.c_str(), "GL_ONE_MINUS_SRC_ALPHA") == 0;
    if (sourceZero && destinationInverseColor) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE;
    }
    if (sourceOne && (destinationOne || destinationInverseColor || destinationInverseAlpha)) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD;
    }
    // GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA is regular transparency, not additive glow.
    if (sourceAlpha) {
        return destinationOne ? QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD
                              : QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA;
    }
    return QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
}

QrGeometryMaterialBlendType BlendTypeForMnemonic(const std::string &mode)
{
    if (_stricmp(mode.c_str(), "add") == 0) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD;
    }
    if (_stricmp(mode.c_str(), "blend") == 0) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA;
    }
    if (_stricmp(mode.c_str(), "filter") == 0) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE;
    }
    return QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
}

// The rasterized overlay reproduces the GL blend equation itself, so the stage factors are kept
// verbatim instead of being collapsed into one of the four material blend classes.
QrBlendFactor BlendFactorForName(const std::string &name, QrBlendFactor fallback)
{
    if (_stricmp(name.c_str(), "GL_ONE") == 0) {
        return QR_BLEND_FACTOR_ONE;
    }
    if (_stricmp(name.c_str(), "GL_ZERO") == 0) {
        return QR_BLEND_FACTOR_ZERO;
    }
    if (_stricmp(name.c_str(), "GL_SRC_COLOR") == 0) {
        return QR_BLEND_FACTOR_SRC_COLOR;
    }
    if (_stricmp(name.c_str(), "GL_ONE_MINUS_SRC_COLOR") == 0) {
        return QR_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    }
    if (_stricmp(name.c_str(), "GL_DST_COLOR") == 0) {
        return QR_BLEND_FACTOR_DST_COLOR;
    }
    if (_stricmp(name.c_str(), "GL_ONE_MINUS_DST_COLOR") == 0) {
        return QR_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    }
    if (_stricmp(name.c_str(), "GL_SRC_ALPHA") == 0) {
        return QR_BLEND_FACTOR_SRC_ALPHA;
    }
    if (_stricmp(name.c_str(), "GL_ONE_MINUS_SRC_ALPHA") == 0) {
        return QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return fallback;
}

QrBlendFactor BlendFactorForMnemonic(const std::string &mode, bool source)
{
    if (_stricmp(mode.c_str(), "add") == 0) {
        return QR_BLEND_FACTOR_ONE;
    }
    if (_stricmp(mode.c_str(), "blend") == 0) {
        return source ? QR_BLEND_FACTOR_SRC_ALPHA : QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    if (_stricmp(mode.c_str(), "filter") == 0) {
        return source ? QR_BLEND_FACTOR_ZERO : QR_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    }
    return source ? QR_BLEND_FACTOR_ONE : QR_BLEND_FACTOR_ZERO;
}

// Q3 uses GL_ZERO/GL_ONE_MINUS_SRC_COLOR for impact decals: the result is dst*(1-src.rgb),
// which no QuakeRay blend mode expresses directly. Such textures are converted to a black
// image whose alpha is the source luminance, turning the decal into regular transparency.
bool BlendFactorsAreDarkenDecal(const std::string &source, const std::string &destination)
{
    return _stricmp(source.c_str(), "GL_ZERO") == 0 &&
        _stricmp(destination.c_str(), "GL_ONE_MINUS_SRC_COLOR") == 0;
}

bool IsShaderStageKeyword(const std::string &token)
{
    static const char *keywords[] = {
        "map",
        "clampmap",
        "animmap",
        "blendfunc",
        "alphafunc",
        "alphagen",
        "rgbgen",
        "tcmod",
        "tcgen",
        "depthwrite",
        "depthfunc",
        "detail",
        "deformvertexes",
        "sort",
        "nopicmip",
        "nomipmaps"
    };
    for (const char *keyword : keywords) {
        if (_stricmp(token.c_str(), keyword) == 0) {
            return true;
        }
    }
    return false;
}

ShaderWaveType ParseShaderWave(const std::string &name)
{
    if (_stricmp(name.c_str(), "sin") == 0) {
        return SHADER_WAVE_SIN;
    }
    if (_stricmp(name.c_str(), "square") == 0) {
        return SHADER_WAVE_SQUARE;
    }
    if (_stricmp(name.c_str(), "triangle") == 0) {
        return SHADER_WAVE_TRIANGLE;
    }
    if (_stricmp(name.c_str(), "sawtooth") == 0) {
        return SHADER_WAVE_SAWTOOTH;
    }
    if (_stricmp(name.c_str(), "inversesawtooth") == 0) {
        return SHADER_WAVE_INVERSE_SAWTOOTH;
    }
    if (_stricmp(name.c_str(), "noise") == 0) {
        return SHADER_WAVE_NOISE;
    }
    return SHADER_WAVE_NONE;
}

float EvaluateShaderWave(ShaderWaveType type, float base, float amplitude, float phase,
    float frequency, float timeSeconds)
{
    if (type == SHADER_WAVE_NONE) {
        return 1.0f;
    }

    const float argument = timeSeconds * frequency + phase;
    float value = 0.0f;
    switch (type) {
    case SHADER_WAVE_SIN:
        value = std::sin(argument * 2.0f * kPi);
        break;
    case SHADER_WAVE_SQUARE:
        value = std::sin(argument * 2.0f * kPi) >= 0.0f ? 1.0f : -1.0f;
        break;
    case SHADER_WAVE_TRIANGLE: {
        const float cycle = argument - std::floor(argument);
        value = cycle < 0.5f ? cycle * 4.0f - 1.0f : 3.0f - cycle * 4.0f;
        break;
    }
    case SHADER_WAVE_SAWTOOTH:
        value = (argument - std::floor(argument)) * 2.0f - 1.0f;
        break;
    case SHADER_WAVE_INVERSE_SAWTOOTH:
        value = 1.0f - (argument - std::floor(argument)) * 2.0f;
        break;
    case SHADER_WAVE_NOISE: {
        const uint32_t bits = static_cast<uint32_t>(argument * 4096.0f);
        uint32_t hash = bits * 747796405u + 2891336453u;
        hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
        hash = (hash >> 22u) ^ hash;
        value = static_cast<float>(hash & 0x00FFFFFFu) / 16777215.0f;
        break;
    }
    case SHADER_WAVE_NONE:
    default:
        return 1.0f;
    }
    return std::clamp(base + amplitude * value, 0.0f, 8.0f);
}

void ConvertToDarkenDecalTexture(std::vector<uint8_t> &pixels)
{
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const float luminance = 0.299f * static_cast<float>(pixels[index + 0]) +
            0.587f * static_cast<float>(pixels[index + 1]) +
            0.114f * static_cast<float>(pixels[index + 2]);
        const float scaled = luminance * static_cast<float>(pixels[index + 3]) / 255.0f;
        pixels[index + 0] = 0;
        pixels[index + 1] = 0;
        pixels[index + 2] = 0;
        pixels[index + 3] = static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
    }
}

void LoadShaderScripts()
{
    if (shaderScriptsLoaded) {
        return;
    }
    shaderScriptsLoaded = true;

    static const char *scriptNames[] = {
        "scripts/models.shader",
        "scripts/common.shader",
        "scripts/gfx.shader",
        "scripts/sfx.shader",
        "scripts/sky.shader",
        "scripts/skin.shader",
        "scripts/base.shader",
        "scripts/base_button.shader",
        "scripts/base_floor.shader",
        "scripts/base_light.shader",
        "scripts/base_object.shader",
        "scripts/base_support.shader",
        "scripts/base_trim.shader",
        "scripts/base_wall.shader",
        "scripts/ctf.shader",
        "scripts/eerie.shader",
        "scripts/gothic_block.shader",
        "scripts/gothic_floor.shader",
        "scripts/gothic_light.shader",
        "scripts/gothic_trim.shader",
        "scripts/gothic_wall.shader",
        "scripts/hell.shader",
        "scripts/liquid.shader",
        "scripts/menu.shader",
        "scripts/organics.shader",
        "scripts/shrine.shader",
        "scripts/explode1.shader",
        "scripts/lightningnew.shader",
        "scripts/tim.shader",
        "scripts/test.shader"
    };

    // Retail scripts first, in their established order, then every other scripts/*.shader
    // from the search path (custom maps, mods). Additional scripts only add definitions so
    // they cannot change how retail content resolves.
    std::vector<std::string> scriptList(std::begin(scriptNames), std::end(scriptNames));
    const size_t retailScriptCount = scriptList.size();
    {
        std::unordered_set<std::string> known;
        for (const std::string &name : scriptList) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            known.insert(lower);
        }
        int listed = 0;
        char **files = ri.FS_ListFiles != nullptr ? ri.FS_ListFiles("scripts", ".shader", &listed) : nullptr;
        for (int i = 0; files != nullptr && i < listed; ++i) {
            if (files[i] == nullptr || files[i][0] == '\0') {
                continue;
            }
            std::string path = std::string("scripts/") + files[i];
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (known.insert(lower).second) {
                scriptList.push_back(std::move(path));
            }
        }
        if (files != nullptr && ri.FS_FreeFileList != nullptr) {
            ri.FS_FreeFileList(files);
        }
    }

    for (size_t scriptIndex = 0; scriptIndex < scriptList.size(); ++scriptIndex) {
        const char *scriptName = scriptList[scriptIndex].c_str();
        const bool overrideExisting = scriptIndex < retailScriptCount;
        void *fileData = nullptr;
        const int fileLength = ri.FS_ReadFile(scriptName, &fileData);
        if (fileLength <= 0 || fileData == nullptr) {
            continue;
        }

        std::string text(static_cast<const char *>(fileData), static_cast<size_t>(fileLength));
        ri.FS_FreeFile(fileData);

        std::vector<std::string> tokens;
        for (size_t index = 0; index < text.size();) {
            const char character = text[index];
            if (std::isspace(static_cast<unsigned char>(character))) {
                ++index;
                continue;
            }
            if (character == '/' && index + 1 < text.size() && text[index + 1] == '/') {
                while (index < text.size() && text[index] != '\n') {
                    ++index;
                }
                continue;
            }
            if (character == '/' && index + 1 < text.size() && text[index + 1] == '*') {
                index += 2;
                while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/')) {
                    ++index;
                }
                index = std::min(text.size(), index + 2);
                continue;
            }
            if (character == '{' || character == '}') {
                tokens.emplace_back(1, character);
                ++index;
                continue;
            }
            const size_t start = index;
            while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index])) &&
                text[index] != '{' && text[index] != '}') {
                ++index;
            }
            tokens.emplace_back(text.substr(start, index - start));
        }

        std::string pendingName;
        std::string currentName;
        const size_t definitionsBefore = shaderDefinitions.size();
        std::string stageMap;
        std::string preferredMap;
        bool stageHasEnvironment = false;
        QrGeometryMaterialBlendType stageBlend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        QrGeometryMaterialBlendType preferredBlend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        QrBlendFactor stageBlendSrc = QR_BLEND_FACTOR_ONE;
        QrBlendFactor stageBlendDst = QR_BLEND_FACTOR_ZERO;
        QrBlendFactor preferredBlendSrc = QR_BLEND_FACTOR_ONE;
        QrBlendFactor preferredBlendDst = QR_BLEND_FACTOR_ZERO;
        bool stageDarkenDecal = false;
        bool preferredDarkenDecal = false;
        bool stageAlphaTested = false;
        bool preferredAlphaTested = false;
        bool preferredEnvironment = false;
        bool preferredStageChosen = false;
        size_t preferredStageIndex = static_cast<size_t>(-1);
        bool currentTransparentSurface = false;
        std::vector<ShaderStage> currentStages;
        ShaderStage currentStage;
        SkyShaderInfo currentSky;
        bool currentSkyParsed = false;
        bool currentSkyFlag = false;
        float stageScale[2] = {1.0f, 1.0f};
        int depth = 0;
        // Arguments never extend past a brace. Q3 parses shader keywords per line; this
        // token stream has no line breaks, so a short or malformed line must not consume
        // the closing '}' and desynchronise every later definition in the file.
        const auto argsAvailable = [&tokens](size_t at, size_t count) {
            for (size_t k = 1; k <= count; ++k) {
                if (at + k >= tokens.size() || tokens[at + k] == "{" || tokens[at + k] == "}") {
                    return false;
                }
            }
            return true;
        };
        for (size_t index = 0; index < tokens.size(); ++index) {
            const std::string &token = tokens[index];
            if (token == "{") {
                if (depth == 0) {
                    currentName = pendingName;
                    stageMap.clear();
                    preferredMap.clear();
                    preferredBlend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
                    preferredBlendSrc = QR_BLEND_FACTOR_ONE;
                    preferredBlendDst = QR_BLEND_FACTOR_ZERO;
                    preferredDarkenDecal = false;
                    preferredAlphaTested = false;
                    preferredEnvironment = false;
                    preferredStageChosen = false;
                    preferredStageIndex = static_cast<size_t>(-1);
                    currentTransparentSurface = false;
                    currentStages.clear();
                    currentSky = SkyShaderInfo{};
                    currentSkyParsed = false;
                    currentSkyFlag = false;
                    depth = 1;
                } else {
                    if (depth == 1) {
                        stageMap.clear();
                        stageHasEnvironment = false;
                        stageBlend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
                        stageBlendSrc = QR_BLEND_FACTOR_ONE;
                        stageBlendDst = QR_BLEND_FACTOR_ZERO;
                        stageDarkenDecal = false;
                        stageAlphaTested = false;
                        currentStage = ShaderStage{};
                        stageScale[0] = 1.0f;
                        stageScale[1] = 1.0f;
                    }
                    ++depth;
                }
                continue;
            }
            if (token == "}") {
                if (depth == 2) {
                    if (!stageMap.empty()) {
                        currentStage.map = stageMap;
                        currentStage.blend = stageBlend;
                        currentStage.blendSrc = stageBlendSrc;
                        currentStage.blendDst = stageBlendDst;
                        currentStage.blended = stageBlend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE ||
                            stageBlendSrc != QR_BLEND_FACTOR_ONE ||
                            stageBlendDst != QR_BLEND_FACTOR_ZERO;
                        currentStage.darkenDecal = stageDarkenDecal;
                        currentStage.alphaTested = stageAlphaTested;
                        currentStage.environmentMap = stageHasEnvironment;
                        currentStage.lightmap = _stricmp(stageMap.c_str(), "$lightmap") == 0;
                        currentStage.dynamic = currentStage.dynamic ||
                            currentStage.blended || currentStage.animationMaps.size() > 1u;
                        currentStages.push_back(currentStage);
                        const size_t currentStageIndex = currentStages.size() - 1u;

                        const bool betterBaseStage = !preferredStageChosen ||
                            preferredEnvironment ||
                            (stageBlend == QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE &&
                             preferredBlend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE);
                        if (!stageHasEnvironment && !currentStage.lightmap && betterBaseStage) {
                            preferredMap = stageMap;
                            preferredBlend = stageBlend;
                            preferredBlendSrc = stageBlendSrc;
                            preferredBlendDst = stageBlendDst;
                            preferredDarkenDecal = stageDarkenDecal;
                            preferredEnvironment = false;
                            preferredStageChosen = true;
                            preferredStageIndex = currentStageIndex;
                            // alphaFunc is usually on the base stage while the lightmap stage
                            // that follows clears the flag again, so it has to stick.
                            preferredAlphaTested = preferredAlphaTested || stageAlphaTested;
                        } else if (stageHasEnvironment && !preferredStageChosen) {
                            preferredMap = stageMap;
                            preferredBlend = stageBlend;
                            preferredBlendSrc = stageBlendSrc;
                            preferredBlendDst = stageBlendDst;
                            preferredDarkenDecal = stageDarkenDecal;
                            preferredEnvironment = true;
                            preferredStageChosen = true;
                            preferredStageIndex = currentStageIndex;
                            preferredAlphaTested = preferredAlphaTested || stageAlphaTested;
                        }
                        if (!currentStage.lightmap) {
                            SkyStage stage;
                            stage.map = stageMap;
                            stage.scale[0] = stageScale[0];
                            stage.scale[1] = stageScale[1];
                            stage.additive = stageBlend == QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD;
                            currentSky.stages.push_back(stage);
                        }
                    }
                    stageMap.clear();
                    stageHasEnvironment = false;
                    stageBlend = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
                    stageBlendSrc = QR_BLEND_FACTOR_ONE;
                    stageBlendDst = QR_BLEND_FACTOR_ZERO;
                    stageDarkenDecal = false;
                    stageAlphaTested = false;
                    currentStage = ShaderStage{};
                    stageScale[0] = 1.0f;
                    stageScale[1] = 1.0f;
                    depth = 1;
                } else if (depth == 1) {
                    if (!currentName.empty() && !currentStages.empty()) {
                        ShaderDefinition definition;
                        definition.map = preferredMap;
                        definition.blend = preferredBlend;
                        definition.blendSrc = preferredBlendSrc;
                        definition.blendDst = preferredBlendDst;
                        definition.darkenDecal = preferredDarkenDecal;
                        definition.alphaTested = preferredAlphaTested;
                        definition.environmentMap = preferredEnvironment;
                        definition.transparentSurface = currentTransparentSurface;
                        definition.baseStageIndex = preferredStageIndex;
                        const bool baseStageAnimated = preferredStageIndex < currentStages.size() &&
                            currentStages[preferredStageIndex].animated;
                        definition.tracedBase = preferredStageIndex < currentStages.size() &&
                            !currentTransparentSurface && !baseStageAnimated &&
                            (preferredBlend == QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE ||
                             preferredAlphaTested);
                        definition.stages = currentStages;
                        for (size_t stageIndex = 0; stageIndex < currentStages.size(); ++stageIndex) {
                            const ShaderStage &stage = currentStages[stageIndex];
                            const bool baseStage = definition.tracedBase &&
                                stageIndex == definition.baseStageIndex;
                            if (!baseStage && (stage.blended || stage.dynamic)) {
                                definition.usesOverlay = true;
                            }
                            definition.dynamic = definition.dynamic || stage.dynamic;
                        }
                        if (!definition.tracedBase) {
                            definition.usesOverlay = true;
                        }
                        if (overrideExisting) {
                            shaderDefinitions[NormalizeShaderName(currentName)] = std::move(definition);
                        } else {
                            shaderDefinitions.emplace(NormalizeShaderName(currentName), std::move(definition));
                        }
                    }
                    if (!currentName.empty() && currentSkyParsed) {
                        skyShaders[NormalizeShaderName(currentName)] = currentSky;
                    } else if (!currentName.empty() && currentSkyFlag) {
                        // Skies without skyparms are drawn from their stage maps alone.
                        skyShaders[NormalizeShaderName(currentName)] = currentSky;
                    }
                    currentName.clear();
                    currentStages.clear();
                    currentSky = SkyShaderInfo{};
                    currentSkyParsed = false;
                    depth = 0;
                } else if (depth > 2) {
                    --depth;
                }
                continue;
            }
            if (depth == 0) {
                pendingName = token;
                continue;
            }
            if (depth == 1) {
                // q3map_sun <r> <g> <b> <intensity> <degrees> <elevation>; q3map_sunExt appends
                // deviance and sample count after the same six arguments.
                if (_strnicmp(token.c_str(), "q3map_sun", 9) == 0 && argsAvailable(index, 6)) {
                    currentSky.hasSun = true;
                    currentSky.sunColor[0] = static_cast<float>(std::atof(tokens[index + 1].c_str()));
                    currentSky.sunColor[1] = static_cast<float>(std::atof(tokens[index + 2].c_str()));
                    currentSky.sunColor[2] = static_cast<float>(std::atof(tokens[index + 3].c_str()));
                    currentSky.sunIntensity = static_cast<float>(std::atof(tokens[index + 4].c_str()));
                    currentSky.sunDegrees = static_cast<float>(std::atof(tokens[index + 5].c_str()));
                    currentSky.sunElevation = static_cast<float>(std::atof(tokens[index + 6].c_str()));
                    index += 6;
                } else if (_stricmp(token.c_str(), "skyparms") == 0 && argsAvailable(index, 3)) {
                    if (tokens[index + 1] != "-") {
                        currentSky.outerBox = tokens[index + 1];
                    }
                    const float cloudHeight = static_cast<float>(std::atof(tokens[index + 2].c_str()));
                    currentSky.cloudHeight = cloudHeight > 0.0f ? cloudHeight : 512.0f;
                    if (tokens[index + 3] != "-" && currentSky.outerBox.empty()) {
                        currentSky.outerBox = tokens[index + 3];
                    }
                    currentSkyParsed = true;
                    index += 3;
                } else if (_stricmp(token.c_str(), "surfaceparm") == 0 && argsAvailable(index, 1)) {
                    if (_stricmp(tokens[index + 1].c_str(), "sky") == 0) {
                        currentSkyFlag = true;
                    } else if (_stricmp(tokens[index + 1].c_str(), "trans") == 0) {
                        currentTransparentSurface = true;
                    }
                    index += 1;
                }
                continue;
            }
            if (depth == 2) {
                if (_stricmp(token.c_str(), "tcGen") == 0 && argsAvailable(index, 1) &&
                    _stricmp(tokens[index + 1].c_str(), "environment") == 0) {
                    stageHasEnvironment = true;
                    currentStage.environmentMap = true;
                } else if (_stricmp(token.c_str(), "blendFunc") == 0 && argsAvailable(index, 1)) {
                    const std::string &first = tokens[index + 1];
                    // blendFunc either takes a factor pair (GL_ONE GL_ONE) or a single
                    // mnemonic (add/blend/filter); consuming the wrong count eats a brace
                    // and desynchronises the rest of the file.
                    if (_strnicmp(first.c_str(), "GL_", 3) == 0 && argsAvailable(index, 2)) {
                        stageBlend = BlendTypeForFactors(first, tokens[index + 2]);
                        stageBlendSrc = BlendFactorForName(first, QR_BLEND_FACTOR_ONE);
                        stageBlendDst = BlendFactorForName(tokens[index + 2], QR_BLEND_FACTOR_ZERO);
                        stageDarkenDecal = BlendFactorsAreDarkenDecal(first, tokens[index + 2]);
                        currentStage.blend = stageBlend;
                        currentStage.blendSrc = stageBlendSrc;
                        currentStage.blendDst = stageBlendDst;
                        currentStage.blended = stageBlend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE ||
                            stageBlendSrc != QR_BLEND_FACTOR_ONE ||
                            stageBlendDst != QR_BLEND_FACTOR_ZERO;
                        currentStage.darkenDecal = stageDarkenDecal;
                        index += 2;
                    } else {
                        stageBlend = BlendTypeForMnemonic(first);
                        stageBlendSrc = BlendFactorForMnemonic(first, true);
                        stageBlendDst = BlendFactorForMnemonic(first, false);
                        currentStage.blend = stageBlend;
                        currentStage.blendSrc = stageBlendSrc;
                        currentStage.blendDst = stageBlendDst;
                        currentStage.blended = stageBlend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE ||
                            stageBlendSrc != QR_BLEND_FACTOR_ONE ||
                            stageBlendDst != QR_BLEND_FACTOR_ZERO;
                        ++index;
                    }
                } else if (_stricmp(token.c_str(), "alphaFunc") == 0 && argsAvailable(index, 1)) {
                    // Q3 cuts these stages out of the rasterized surface (grates, foliage,
                    // chains). QuakeRay has a dedicated alpha tested hit group for that.
                    stageAlphaTested = true;
                    currentStage.alphaTested = true;
                    // alphaFunc uses one comparison token (GT0, GE128, ...). Consuming two
                    // swallowed the stage's closing brace and desynchronised every following
                    // shader definition in the file.
                    ++index;
                } else if (_stricmp(token.c_str(), "map") == 0 || _stricmp(token.c_str(), "clampmap") == 0) {
                    if (argsAvailable(index, 1)) {
                        stageMap = tokens[index + 1];
                        currentStage.map = stageMap;
                        currentStage.lightmap = _stricmp(stageMap.c_str(), "$lightmap") == 0;
                        ++index;
                    }
                } else if (_stricmp(token.c_str(), "animMap") == 0 && argsAvailable(index, 2)) {
                    currentStage.animationFps = std::max(
                        static_cast<float>(std::atof(tokens[index + 1].c_str())), 0.001f);
                    currentStage.animationMaps.clear();
                    size_t frameIndex = index + 2;
                    while (frameIndex < tokens.size() && tokens[frameIndex] != "}" &&
                        !IsShaderStageKeyword(tokens[frameIndex])) {
                        currentStage.animationMaps.push_back(tokens[frameIndex]);
                        ++frameIndex;
                    }
                    if (!currentStage.animationMaps.empty()) {
                        stageMap = currentStage.animationMaps.front();
                        currentStage.map = stageMap;
                        currentStage.lightmap = _stricmp(stageMap.c_str(), "$lightmap") == 0;
                    }
                    currentStage.dynamic = currentStage.dynamic ||
                        currentStage.animationMaps.size() > 1u;
                    currentStage.animated = currentStage.animated ||
                        currentStage.animationMaps.size() > 1u;
                    index = frameIndex - 1u;
                } else if (_stricmp(token.c_str(), "tcMod") == 0 && argsAvailable(index, 1)) {
                    // tcMod sub-commands take a variable number of arguments, so only the
                    // ones that matter here are consumed; eating a fixed count would
                    // swallow the closing brace of the stage.
                    const char *subCommand = tokens[index + 1].c_str();
                    currentStage.dynamic = true;
                    currentStage.animated = true;
                    if (_stricmp(subCommand, "scale") == 0 && argsAvailable(index, 3)) {
                        stageScale[0] = static_cast<float>(std::atof(tokens[index + 2].c_str()));
                        stageScale[1] = static_cast<float>(std::atof(tokens[index + 3].c_str()));
                        if (stageScale[0] == 0.0f) {
                            stageScale[0] = 1.0f;
                        }
                        if (stageScale[1] == 0.0f) {
                            stageScale[1] = 1.0f;
                        }
                        index += 3;
                    } else if (_stricmp(subCommand, "scroll") == 0 && argsAvailable(index, 3)) {
                        currentStage.scroll[0] +=
                            static_cast<float>(std::atof(tokens[index + 2].c_str()));
                        currentStage.scroll[1] +=
                            static_cast<float>(std::atof(tokens[index + 3].c_str()));
                        index += 3;
                    } else if (_stricmp(subCommand, "rotate") == 0 && argsAvailable(index, 2)) {
                        index += 2;
                    } else if (_stricmp(subCommand, "entityTranslate") == 0) {
                        index += 1;
                    }
                } else if ((_stricmp(token.c_str(), "rgbGen") == 0 ||
                    _stricmp(token.c_str(), "alphaGen") == 0) &&
                    index + 1 < tokens.size() &&
                    _stricmp(tokens[index + 1].c_str(), "wave") == 0 &&
                    argsAvailable(index, 6)) {
                    const ShaderWaveType wave = ParseShaderWave(tokens[index + 2]);
                    if (wave != SHADER_WAVE_NONE) {
                        const bool alpha = _stricmp(token.c_str(), "alphaGen") == 0;
                        ShaderWaveType &destination = alpha ? currentStage.alphaWave
                                                           : currentStage.rgbWave;
                        float &base = alpha ? currentStage.alphaWaveBase
                                            : currentStage.rgbWaveBase;
                        float &amplitude = alpha ? currentStage.alphaWaveAmplitude
                                                 : currentStage.rgbWaveAmplitude;
                        float &phase = alpha ? currentStage.alphaWavePhase
                                             : currentStage.rgbWavePhase;
                        float &frequency = alpha ? currentStage.alphaWaveFrequency
                                                 : currentStage.rgbWaveFrequency;
                        destination = wave;
                        base = static_cast<float>(std::atof(tokens[index + 3].c_str()));
                        amplitude = static_cast<float>(std::atof(tokens[index + 4].c_str()));
                        phase = static_cast<float>(std::atof(tokens[index + 5].c_str()));
                        frequency = static_cast<float>(std::atof(tokens[index + 6].c_str()));
                        currentStage.dynamic = true;
                        currentStage.animated = true;
                    }
                    index += 6;
                }
            }
        }

        if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
            HostLog("LoadShaderScripts: %s definitions=%zu total=%zu", scriptName,
                shaderDefinitions.size() - definitionsBefore, shaderDefinitions.size());
        }
    }

    if (hostLogEnabled) {
        size_t alphaTestedCount = 0;
        std::string alphaTestedNames;
        for (const auto &entry : shaderDefinitions) {
            if (entry.second.alphaTested) {
                ++alphaTestedCount;
                if (alphaTestedCount <= 64) {
                    alphaTestedNames += entry.first;
                    alphaTestedNames += ' ';
                }
            }
        }
        HostLog("LoadShaderScripts: definitions=%zu alphaTested=%zu [%s]",
            shaderDefinitions.size(), alphaTestedCount, alphaTestedNames.c_str());
        static const char *reportedShaders[] = {
            "textures/sfx/launchpad_diamond",
            "textures/sfx/diamond2cjumppad",
            "models/mapobjects/spotlamp/beam",
        };
        for (const char *reportedShader : reportedShaders) {
            const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(reportedShader));
            HostLog("LoadShaderScripts: probe shader='%s' found=%d map='%s' stages=%zu overlay=%d",
                reportedShader, definitionIt != shaderDefinitions.end() ? 1 : 0,
                definitionIt != shaderDefinitions.end() ? definitionIt->second.map.c_str() : "",
                definitionIt != shaderDefinitions.end() ? definitionIt->second.stages.size() : 0u,
                definitionIt != shaderDefinitions.end() && definitionIt->second.usesOverlay ? 1 : 0);
        }
    }

}

QrMaterial GetMaterialForTextureNameInternal(const char *name, bool rawTexture)
{
    if (name == nullptr || name[0] == '\0') {
        return defaultMaterial;
    }

    LoadShaderScripts();
    const std::string key = NormalizeShaderName(name);
    std::unordered_map<std::string, QrMaterial> &cache =
        rawTexture ? textureMaterialsRaw : textureMaterials;
    const auto existing = cache.find(key);
    if (existing != cache.end()) {
        return existing->second;
    }

    const auto shaderIt = shaderDefinitions.find(key);
    const char *textureName = shaderIt != shaderDefinitions.end() ? shaderIt->second.map.c_str() : name;
    const bool darkenDecal = !rawTexture && shaderIt != shaderDefinitions.end() &&
        shaderIt->second.darkenDecal;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> albedo;
    QrMaterial material = QR_NO_MATERIAL;
    if (LoadQ3Texture(textureName, width, height, albedo)) {
        if (darkenDecal) {
            ConvertToDarkenDecalTexture(albedo);
        }
        material = CreateMaterialFromPixels(textureName, width, height, albedo,
            QR_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    if (material == QR_NO_MATERIAL) {
        if (hostLogEnabled) {
            HostLog("GetMaterialForTextureName: %s has no texture, using the default material",
                textureName);
        }
        material = defaultMaterial;
    }
    cache.emplace(key, material);
    return material;
}

QrMaterial GetMaterialForTextureName(const char *name)
{
    return GetMaterialForTextureNameInternal(name, false);
}

// The blend-equation path needs the untouched image: a darken decal is expressed by its
// ZERO/ONE_MINUS_SRC_COLOR factors instead of by a luminance-to-alpha conversion.
QrMaterial GetRawMaterialForTextureName(const char *name)
{
    return GetMaterialForTextureNameInternal(name, true);
}

// The fixed function blend adds the glass shell in an 8 bit frame buffer, where the bright pixels
// are already clamped. The HDR pipeline adds the same shell into an unclamped buffer, so the shell
// has to be dimmed before the tone mapper sees it. The same helper scales the opaque body stages,
// whose baked environment map ends up too dark and too washed out once the pipeline treats it as a
// lit albedo and the additive shell sits on top of it.
QrMaterial GetScaledEnvMaterialForTextureName(const char *name, float scale, float saturation = 1.0f)
{
    const float factor = std::clamp(scale, 0.0f, 8.0f);
    const float saturationFactor = std::clamp(saturation, 0.0f, 4.0f);
    if (name == nullptr || name[0] == '\0' ||
        (fabsf(factor - 1.0f) < 0.01f && fabsf(saturationFactor - 1.0f) < 0.01f)) {
        return GetMaterialForTextureName(name);
    }

    char key[540] = {};
    snprintf(key, sizeof(key), "%s|%.3f|%.3f",
        NormalizeShaderName(name == nullptr ? "" : name).c_str(), factor, saturationFactor);
    const auto existing = scaledEnvMaterials.find(key);
    if (existing != scaledEnvMaterials.end()) {
        return existing->second;
    }

    int width = 0;
    int height = 0;
    std::vector<uint8_t> albedo;
    QrMaterial material = QR_NO_MATERIAL;
    if (LoadQ3Texture(name, width, height, albedo)) {
        for (size_t i = 0; i + 3 < albedo.size(); i += 4) {
            float channels[3] = {
                albedo[i + 0] * factor,
                albedo[i + 1] * factor,
                albedo[i + 2] * factor};
            const float luminance =
                0.299f * channels[0] + 0.587f * channels[1] + 0.114f * channels[2];
            for (int channel = 0; channel < 3; ++channel) {
                const float value =
                    luminance + (channels[channel] - luminance) * saturationFactor;
                albedo[i + channel] =
                    static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
            }
        }
        material = CreateMaterialFromPixels(name, width, height, albedo,
            QR_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    if (material == QR_NO_MATERIAL) {
        material = GetMaterialForTextureName(name);
    }
    scaledEnvMaterials.emplace(key, material);
    return material;
}

QrMaterial GetMaterialForHandle(qhandle_t handle)
{    const auto existing = uiMaterials.find(handle);
    if (existing != uiMaterials.end() && existing->second != QR_NO_MATERIAL) {
        return existing->second;
    }

    // Materials belong to the current QuakeRay instance, so a handle registered before the
    // renderer was re-initialized has to be re-resolved from its shader name.
    const auto nameIt = uiShaderNames.find(handle);
    if (nameIt != uiShaderNames.end() && !nameIt->second.empty()) {
        const QrMaterial material = GetMaterialForTextureName(nameIt->second.c_str());
        if (material != QR_NO_MATERIAL) {
            uiMaterials[handle] = material;
            if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
                HostLog("GetMaterialForHandle: handle=%d re-resolved from '%s'",
                    static_cast<int>(handle), nameIt->second.c_str());
            }
            return material;
        }
    }

    if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
        static std::unordered_set<qhandle_t> reported;
        if (reported.insert(handle).second) {
            HostLog("GetMaterialForHandle: handle=%d unknown, using default material",
                static_cast<int>(handle));
        }
    }
    return defaultMaterial;
}

uint32_t PackColor(const color4ub_t &color)
{
    return static_cast<uint32_t>(color.rgba[0]) |
        (static_cast<uint32_t>(color.rgba[1]) << 8) |
        (static_cast<uint32_t>(color.rgba[2]) << 16) |
        (static_cast<uint32_t>(color.rgba[3]) << 24);
}

const char *ShaderNameForHandle(qhandle_t handle)
{
    const auto nameIt = uiShaderNames.find(handle);
    return nameIt != uiShaderNames.end() ? nameIt->second.c_str() : "";
}

int ShaderBlendTypeForHandle(qhandle_t handle)
{
    const auto nameIt = uiShaderNames.find(handle);
    if (nameIt == uiShaderNames.end()) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    }
    LoadShaderScripts();
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(nameIt->second));
    return definitionIt != shaderDefinitions.end() ? definitionIt->second.blend
                                                   : QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
}

bool ShaderUsesAlphaTest(const char *shaderName)
{
    if (shaderName == nullptr || shaderName[0] == '\0') {
        return false;
    }
    LoadShaderScripts();
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
    return definitionIt != shaderDefinitions.end() && definitionIt->second.alphaTested;
}

bool EffectTextureHasAlpha(const char *shaderName)
{
    if (shaderName == nullptr || shaderName[0] == '\0') {
        return true;
    }
    std::string base = shaderName;
    for (char &character : base) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const size_t slash = base.find_last_of('/');
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        base.resize(dot);
    }

    static const char *extensions[] = {"", ".tga", ".jpg", ".jpeg", ".png"};
    for (const char *extension : extensions) {
        const auto entry = textureHasAlpha.find(base + extension);
        if (entry != textureHasAlpha.end()) {
            return entry->second;
        }
    }
    return true;
}

// Effect shaders usually have no script at all (decals), so the blending mode has to come from
// the image itself: Q3 stores impact marks and explosion frames as black-background glow maps
// without an alpha channel, which only look right when they are added to the surface.
QrGeometryMaterialBlendType EffectBlendForHandle(qhandle_t handle)
{
    const char *shaderName = ShaderNameForHandle(handle);
    LoadShaderScripts();
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
    if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
        static std::unordered_set<std::string> reported;
        if (reported.insert(shaderName).second) {
            HostLog("EffectBlendForHandle: name='%s' found=%d map='%s' blend=%d darkenDecal=%d",
                shaderName, definitionIt != shaderDefinitions.end() ? 1 : 0,
                definitionIt != shaderDefinitions.end() ? definitionIt->second.map.c_str() : "",
                definitionIt != shaderDefinitions.end() ? static_cast<int>(definitionIt->second.blend) : -1,
                definitionIt != shaderDefinitions.end() ? (definitionIt->second.darkenDecal ? 1 : 0) : -1);
        }
    }
    if (definitionIt != shaderDefinitions.end() && definitionIt->second.darkenDecal) {
        return QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA;
    }
    const QrGeometryMaterialBlendType declared =
        static_cast<QrGeometryMaterialBlendType>(ShaderBlendTypeForHandle(handle));
    if (declared != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE) {
        return declared;
    }
    return EffectTextureHasAlpha(ShaderNameForHandle(handle))
        ? QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA
        : QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD;
}

// Several impact marks are black-background images without an alpha channel and have no shader
// script at all, so the alpha blend they would inherit turns the black border into a solid card.
// Those marks are glow maps of the retail renderer and belong to the additive path.
bool EffectNeedsAdditiveFallback(qhandle_t handle)
{
    const char *shaderName = ShaderNameForHandle(handle);
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
    if (definitionIt != shaderDefinitions.end() && !definitionIt->second.map.empty()) {
        return !EffectTextureHasAlpha(definitionIt->second.map.c_str());
    }
    return !EffectTextureHasAlpha(shaderName);
}

// The GL blend equation of an effect shader, as the rasterized overlay expects it. Returns false
// when the effect is opaque and must not take the blending path at all.
bool EffectBlendFactorsForHandle(qhandle_t handle, QrBlendFactor &source, QrBlendFactor &destination)
{
    const char *shaderName = ShaderNameForHandle(handle);
    LoadShaderScripts();
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
    if (definitionIt != shaderDefinitions.end() &&
        definitionIt->second.blend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE) {
        source = definitionIt->second.blendSrc;
        destination = definitionIt->second.blendDst;
        if (source == QR_BLEND_FACTOR_SRC_ALPHA &&
            destination == QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
            EffectNeedsAdditiveFallback(handle)) {
            source = QR_BLEND_FACTOR_ONE;
            destination = QR_BLEND_FACTOR_ONE;
        }
        return true;
    }

    switch (EffectBlendForHandle(handle)) {
    case QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA:
        if (EffectNeedsAdditiveFallback(handle)) {
            source = QR_BLEND_FACTOR_ONE;
            destination = QR_BLEND_FACTOR_ONE;
            return true;
        }
        source = QR_BLEND_FACTOR_SRC_ALPHA;
        destination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        return true;
    case QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD:
        source = QR_BLEND_FACTOR_ONE;
        destination = QR_BLEND_FACTOR_ONE;
        return true;
    case QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE:
        source = QR_BLEND_FACTOR_ZERO;
        destination = QR_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        return true;
    default:
        return false;
    }
}

QrMaterial GetRawMaterialForHandle(qhandle_t handle)
{
    const auto existing = uiMaterialsRaw.find(handle);
    if (existing != uiMaterialsRaw.end()) {
        return existing->second;
    }

    QrMaterial material = QR_NO_MATERIAL;
    const auto nameIt = uiShaderNames.find(handle);
    if (nameIt != uiShaderNames.end() && !nameIt->second.empty()) {
        material = GetRawMaterialForTextureName(nameIt->second.c_str());
    }
    if (material == QR_NO_MATERIAL) {
        material = defaultMaterial;
    }
    uiMaterialsRaw.emplace(handle, material);
    return material;
}

QrMaterial CreateMaterialForShader(int shaderNum, const char *shaderName)
{
    if (shaderNum < 0 || static_cast<size_t>(shaderNum) >= shaderMaterials.size()) {
        return defaultMaterial;
    }
    if (shaderMaterialAttempted[static_cast<size_t>(shaderNum)] != 0) {
        return shaderMaterials[static_cast<size_t>(shaderNum)];
    }
    shaderMaterialAttempted[static_cast<size_t>(shaderNum)] = 1;

    // BSP shader names frequently have no image of their own. Resolve the script's base
    // stage first instead of dropping every such surface to the white fallback material.
    const QrMaterial material = GetMaterialForTextureName(shaderName);
    shaderMaterials[static_cast<size_t>(shaderNum)] = material;
    return material;
}

qhandle_t RegisterUiShader(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return 0;
    }

    const auto existing = uiShaderHandles.find(name);
    if (existing != uiShaderHandles.end()) {
        return existing->second;
    }

    const qhandle_t handle = nextUiHandle++;
    uiShaderHandles.emplace(name, handle);
    uiShaderNames[handle] = name;

    const QrMaterial material = GetMaterialForTextureName(name);
    if (material != QR_NO_MATERIAL) {
        uiMaterials.emplace(handle, material);
    }
    return handle;
}

void ResetMaterialState()
{
    // Handles stay valid because the shader names are owned by the host, but every
    // QrMaterial was destroyed together with the old QuakeRay instance.
    uiMaterials.clear();
    uiMaterialsRaw.clear();
    textureMaterials.clear();
    textureMaterialsRaw.clear();
    scaledEnvMaterials.clear();
    shaderMaterials.clear();
    shaderMaterialAttempted.clear();
    defaultMaterial = QR_NO_MATERIAL;
    skyAtlasMaterial = QR_NO_MATERIAL;
    skyAtlasAttempted = false;
    skyAtlasFaceSize = 0;
    worldClusters.uploaded = false;
    markTriangles.clear();
    markGrid.clear();
    worldSurfaceInfos.clear();
    worldShaderNames.clear();
    worldOverlayBatches.clear();
    skyCubemap = QR_EMPTY_CUBEMAP;
}

void RebuildUiMaterials()
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    int rebuilt = 0;
    for (const auto &entry : uiShaderHandles) {
        const QrMaterial material = GetMaterialForTextureName(entry.first.c_str());
        if (material != QR_NO_MATERIAL) {
            uiMaterials[entry.second] = material;
            ++rebuilt;
        }
    }
    HostLog("RebuildUiMaterials: rebuilt=%d of %zu", rebuilt, uiShaderHandles.size());
}

bool CreateQrayInstance()
{
    HostLog("CreateQrayInstance: start");

    QrInstanceCreateInfo info{};
#if defined(QR_USE_SURFACE_WIN32)
    // Keep using GetWindowHandle on Windows: it predates GetNativeWindow, so a new
    // renderer DLL stays compatible with an older client executable.
    void *windowHandle = ri.GetWindowHandle != nullptr ? ri.GetWindowHandle() : nullptr;
    if (windowHandle == nullptr) {
        HostLog("CreateQrayInstance: no window handle");
        ri.Printf(PRINT_WARNING, "rendererqray: no native window handle available\n");
        return false;
    }
    HostLog("CreateQrayInstance: hwnd=%p", windowHandle);

    QrWin32SurfaceCreateInfo surfaceInfo{};
    surfaceInfo.hinstance = GetModuleHandleA(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(windowHandle);
    info.pWin32SurfaceInfo = &surfaceInfo;
#elif defined(QR_USE_SURFACE_XLIB)
    nativeWindow_t nativeWindow{};
    if (ri.GetNativeWindow == nullptr || !ri.GetNativeWindow(&nativeWindow)) {
        HostLog("CreateQrayInstance: no native window");
        ri.Printf(PRINT_WARNING, "rendererqray: no native window available\n");
        return false;
    }
    if (nativeWindow.type != NATIVE_WINDOW_X11 || nativeWindow.display == nullptr || nativeWindow.window == 0) {
        HostLog("CreateQrayInstance: unsupported window system type=%d", static_cast<int>(nativeWindow.type));
        ri.Printf(PRINT_WARNING,
            "rendererqray: QuakeRay needs an X11 window (got window system %d). "
            "On Wayland run with SDL_VIDEODRIVER=x11 to use XWayland.\n",
            static_cast<int>(nativeWindow.type));
        return false;
    }
    HostLog("CreateQrayInstance: x11 display=%p window=0x%llx", nativeWindow.display, nativeWindow.window);

    QrXlibSurfaceCreateInfo surfaceInfo{};
    surfaceInfo.dpy = nativeWindow.display;
    surfaceInfo.window = static_cast<unsigned long>(nativeWindow.window);
    info.pXlibSurfaceCreateInfo = &surfaceInfo;
#else
#error "rendererqray: QuakeRay SDK was built without a supported surface type"
#endif

    info.pAppName = "QuakeRay";
    info.pAppGUID = "6f6f4e42-2f8b-4d5f-9c8b-4a7b1c2d3e4f";
    info.pConfigPath = CvarText(qrayConfig, "qray.cfg");
    info.pfnPrint = QrayPrint;
    info.pShaderFolderPath = CvarText(qrayShaderDir, "qray/shaders/");
    info.pBlueNoiseFilePath = CvarText(qrayBlueNoise, "qray/blue_noise.png");
    info.pfnOpenFile = QrayOpenFile;
    info.pfnCloseFile = QrayCloseFile;
    info.primaryRaysMaxAlbedoLayers = 2;
    info.indirectIlluminationMaxAlbedoLayers = 1;
    info.rayCullBackFacingTriangles = (qrayCullBackFaces != nullptr && qrayCullBackFaces->integer == 0)
        ? QR_FALSE : QR_TRUE;
    info.allowGeometryWithSkyFlag = QR_TRUE;
    info.rasterizedMaxVertexCount = 1u << 18;
    info.rasterizedMaxIndexCount = 1u << 19;
    info.rasterizedVertexColorGamma = QR_TRUE;
    info.rasterizedSkyCubemapSize = 256;
    info.maxTextureCount = 4096;
    info.textureSamplerForceMinificationFilterLinear = QR_TRUE;
    info.textureSamplerForceNormalMapFilterLinear = QR_TRUE;
    info.originalAlbedoAlphaTextureIsSRGB = QR_TRUE;
    info.originalRoughnessMetallicEmissionTextureIsSRGB = QR_FALSE;
    info.originalNormalTextureIsSRGB = QR_FALSE;
    info.overridenAlbedoAlphaTextureIsSRGB = QR_TRUE;
    info.overridenRoughnessMetallicEmissionTextureIsSRGB = QR_FALSE;
    info.overridenNormalTextureIsSRGB = QR_FALSE;
    info.pWaterNormalTexturePath = CvarText(qrayWaterNormal, "");
    info.pbrTextureSwizzling = QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE;
    info.effectWipeIsUsed = QR_FALSE;
    info.godRaysQuality = 1;

    HostLog("CreateQrayInstance: shaders=%s blueNoise=%s water=%s config=%s",
        CvarText(qrayShaderDir, "qray/shaders/"), CvarText(qrayBlueNoise, "qray/blue_noise.png"),
        CvarText(qrayWaterNormal, ""), CvarText(qrayConfig, "qray.cfg"));

    QrResult result = QR_GRAPHICS_API_ERROR;
    try {
        result = CreateInstanceProtected(&info, &qrayInstance);
    } catch (const std::exception &exception) {
        HostLog("CreateQrayInstance: exception: %s", exception.what());
        qrayInstance = QR_NULL_HANDLE;
        return false;
    } catch (...) {
        HostLog("CreateQrayInstance: unknown exception");
        qrayInstance = QR_NULL_HANDLE;
        return false;
    }
    HostLog("CreateQrayInstance: qrCreateInstance result=%d instance=%p", static_cast<int>(result), qrayInstance);
    if (result != QR_SUCCESS) {
        qrayInstance = QR_NULL_HANDLE;
        ri.Printf(PRINT_WARNING, "rendererqray: qrCreateInstance failed: %d\n", static_cast<int>(result));
        return false;
    }

    return true;
}

bool ConfigureSpatialConvention()
{
    QrSpatialConvention spatial{};
    spatial.size = sizeof(spatial);
    spatial.version = QR_SPATIAL_CONVENTION_VERSION;
    spatial.sceneXAxis = QR_AXIS_POSITIVE_X;
    spatial.sceneYAxis = QR_AXIS_POSITIVE_Y;
    spatial.sceneZAxis = QR_AXIS_POSITIVE_Z;
    spatial.hostIsRightHanded = QR_TRUE;

    const QrResult result = qrSetSpatialConvention(qrayInstance, &spatial);
    HostLog("ConfigureSpatialConvention: identity Z-up right-handed result=%d scale=%f",
        static_cast<int>(result), kQuakeUnitToMeter);
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrSetSpatialConvention failed: %d\n",
            static_cast<int>(result));
        return false;
    }
    return true;
}

// Retail Q3 bakes the sun into the lightmaps, so the sidecar has to place it from the sky
// shader. Maps without q3map_sun fall back to the default that matches the retail lighting
// direction of an unmodified Quake 3.
constexpr float kQ3MapSunDefaultDegrees = 232.0f;
constexpr float kQ3MapSunDefaultElevation = 55.0f;
constexpr float kQ3MapSunReferenceIntensity = 80.0f;
// QuakeRay's host scales the physical sky radiance by 0.1 before it reaches the SDK.
// Passing a 0..1 sky texture straight through made the Q3 sidecar ten times brighter.
constexpr float kQuakeRaySkyRadianceScale = 0.1f;
constexpr float kQuakeRaySkyLightMultiplierMax = 8.0f;

// The map's q3map_sun is the source of truth for the sun, qray_sunColor and qray_sunIntensity
// are the manual overrides. Shared by the directional light and by the disc that gets burned
// into the sky atlas, so both always agree on where the sun sits.
bool ResolveSun(float lightDirection[3], float color[3], float &intensity)
{
    float r = 1.0f;
    float g = 0.95f;
    float b = 0.85f;
    float degrees = kQ3MapSunDefaultDegrees;
    float elevation = kQ3MapSunDefaultElevation;
    float scale = 1.0f;

    LoadShaderScripts();
    const auto skyIt = skyShaders.find(NormalizeShaderName(mapSkyShaderName));
    const bool hasSun = skyIt != skyShaders.end() && skyIt->second.hasSun;
    if (hasSun) {
        const SkyShaderInfo &sky = skyIt->second;
        r = sky.sunColor[0];
        g = sky.sunColor[1];
        b = sky.sunColor[2];
        degrees = sky.sunDegrees;
        elevation = sky.sunElevation;
        scale = std::clamp(sky.sunIntensity / kQ3MapSunReferenceIntensity, 0.0f, 8.0f);
        static bool sunReported = false;
        if (!sunReported) {
            sunReported = true;
            HostLog("ResolveSun: q3map_sun rgb=(%.2f %.2f %.2f) intensity=%.0f dir=%.0f/%.0f",
                r, g, b, sky.sunIntensity, degrees, elevation);
        }
    }
    if (qraySunColor != nullptr && qraySunColor->string[0] != '\0') {
        scale = 1.0f;
        float parsed[3] = {};
        if (std::sscanf(qraySunColor->string, "%f %f %f", &parsed[0], &parsed[1], &parsed[2]) != 3) {
            r = 1.0f;
            g = 0.95f;
            b = 0.85f;
        } else {
            const bool usesByteRange = parsed[0] > 1.0f || parsed[1] > 1.0f || parsed[2] > 1.0f;
            const float colorScale = usesByteRange ? 1.0f / 255.0f : 1.0f;
            r = std::clamp(parsed[0] * colorScale, 0.0f, 1.0f);
            g = std::clamp(parsed[1] * colorScale, 0.0f, 1.0f);
            b = std::clamp(parsed[2] * colorScale, 0.0f, 1.0f);
        }
    }

    color[0] = r;
    color[1] = g;
    color[2] = b;
    intensity = std::clamp(CvarFloat(qrayPhysicalSkyPower, 3.0f), 0.0f, 100.0f) * scale;

    lightDirection[0] = 0.35f;
    lightDirection[1] = 0.45f;
    lightDirection[2] = -0.82f;
    const float directionOverride = -999.0f;
    const float pitchOverride = CvarFloat(qrayPhysicalSkySunPitch, -1000.0f);
    const float yawOverride = CvarFloat(qrayPhysicalSkyYaw, -1000.0f);
    const bool hasDirectionOverride = pitchOverride > directionOverride ||
        yawOverride > directionOverride;
    if (hasSun || hasDirectionOverride) {
        const float toRadians = 3.14159265358979f / 180.0f;
        const float sunYaw = yawOverride > directionOverride ? yawOverride : degrees;
        const float sunPitch = std::clamp(
            pitchOverride > directionOverride ? pitchOverride : elevation, -89.0f, 89.0f);
        const float yaw = sunYaw * toRadians;
        const float pitch = sunPitch * toRadians;
        // degrees/elevation describe where the sun sits in the sky, the light travels the
        // opposite way.
        lightDirection[0] = -std::cos(yaw) * std::cos(pitch);
        lightDirection[1] = -std::sin(yaw) * std::cos(pitch);
        lightDirection[2] = -std::sin(pitch);
        NormalizeVector(lightDirection);
    }
    return hasSun;
}

void UploadSunLight()
{
    if (qrayPhysicalSkySun != nullptr && qrayPhysicalSkySun->integer == 0) {
        return;
    }

    float color[3] = {1.0f, 0.95f, 0.85f};
    float sunDirection[3] = {0.35f, 0.45f, -0.82f};
    float intensity = 3.0f;
    ResolveSun(sunDirection, color, intensity);
    if (intensity <= 0.0f) {
        return;
    }

    QrDirectionalLightUploadInfo light{};
    light.uniqueID = 1;
    light.color.data[0] = color[0] * intensity;
    light.color.data[1] = color[1] * intensity;
    light.color.data[2] = color[2] * intensity;
    light.direction.data[0] = sunDirection[0];
    light.direction.data[1] = sunDirection[1];
    light.direction.data[2] = sunDirection[2];
    light.angularDiameterDegrees = 0.53f;

    const QrResult result = qrUploadDirectionalLight(qrayInstance, &light);
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrUploadDirectionalLight failed: %d\n", static_cast<int>(result));
    }
}

void UploadMapLights()
{
    if (qrayInstance == QR_NULL_HANDLE || mapLights.empty()) {
        return;
    }

    const float intensityScale = CvarFloat(qrayLightScale, 200.0f);
    const float sphereRadius = std::max(1.0f, CvarFloat(qrayLightRadius, 16.0f));

    for (size_t i = 0; i < mapLights.size(); ++i) {
        const MapLight &source = mapLights[i];
        QrSphericalLightUploadInfo light{};
        light.uniqueID = 1000 + i;
        light.color.data[0] = source.color[0] * intensityScale;
        light.color.data[1] = source.color[1] * intensityScale;
        light.color.data[2] = source.color[2] * intensityScale;
        QuakePositionToMeters(source.position, light.position);
        light.radius = QuakeUnitsToMeters(sphereRadius);

        const QrResult result = qrUploadSphericalLight(qrayInstance, &light);
        if (result != QR_SUCCESS) {
            HostLog("UploadMapLights: light %zu failed result=%d", i, static_cast<int>(result));
            break;
        }
    }
}

void UploadDynamicLights()
{
    static size_t lastLightCount = 0;
    if (dynamicLights.size() != lastLightCount) {
        lastLightCount = dynamicLights.size();
        size_t additive = 0;
        size_t linear = 0;
        for (const DynamicLight &light : dynamicLights) {
            additive += light.additive ? 1u : 0u;
            linear += light.linear ? 1u : 0u;
        }
        HostLog("UploadDynamicLights: count=%zu additive=%zu linear=%zu",
            dynamicLights.size(), additive, linear);
    }
    if (qrayInstance == QR_NULL_HANDLE || dynamicLights.empty()) {
        return;
    }

    const float intensityScale = CvarFloat(qrayDlightScale, 200.0f);
    const float emissionRadius = QuakeUnitsToMeters(
        std::max(1.0f, CvarFloat(qrayLightRadius, 16.0f)));

    for (size_t i = 0; i < dynamicLights.size(); ++i) {
        const DynamicLight &source = dynamicLights[i];
        QrSphericalLightUploadInfo light{};
        light.uniqueID = kDynamicLightUniqueIdBase + i;
        light.color.data[0] = source.color[0] * intensityScale;
        light.color.data[1] = source.color[1] * intensityScale;
        light.color.data[2] = source.color[2] * intensityScale;
        QuakePositionToMeters(source.origin, light.position);
        light.radius = emissionRadius;

        const QrResult result = qrUploadSphericalLight(qrayInstance, &light);
        if (result != QR_SUCCESS) {
            HostLog("UploadDynamicLights: light %zu failed result=%d", i, static_cast<int>(result));
            break;
        }
    }
}

void UploadClusterLightSources();

bool GlobalLightSamplingEnabled()
{
    return qrayRestir != nullptr && qrayRestir->integer != 0;
}

void UploadSceneLights()
{
    UploadSunLight();
    UploadMapLights();
    UploadDynamicLights();
    const bool globalSampling = GlobalLightSamplingEnabled();
    static int reportedSamplingMode = -1;
    if (reportedSamplingMode != static_cast<int>(globalSampling)) {
        reportedSamplingMode = globalSampling ? 1 : 0;
        HostLog("Lighting: %s", globalSampling
            ? "ReSTIR over the global light list (cluster sources disabled)"
            : "per-cluster light sources");
    }
    if (!globalSampling) {
        UploadClusterLightSources();
    }
}

void RotationMatrixQ1(float matrix[16], float angle, float x, float y, float z)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);

    matrix[0 * 4 + 0] = x * x * (1.0f - c) + c;
    matrix[0 * 4 + 1] = y * x * (1.0f - c) + z * s;
    matrix[0 * 4 + 2] = x * z * (1.0f - c) - y * s;
    matrix[0 * 4 + 3] = 0.0f;

    matrix[1 * 4 + 0] = x * y * (1.0f - c) - z * s;
    matrix[1 * 4 + 1] = y * y * (1.0f - c) + c;
    matrix[1 * 4 + 2] = y * z * (1.0f - c) + x * s;
    matrix[1 * 4 + 3] = 0.0f;

    matrix[2 * 4 + 0] = x * z * (1.0f - c) + y * s;
    matrix[2 * 4 + 1] = y * z * (1.0f - c) - x * s;
    matrix[2 * 4 + 2] = z * z * (1.0f - c) + c;
    matrix[2 * 4 + 3] = 0.0f;

    matrix[3 * 4 + 0] = 0.0f;
    matrix[3 * 4 + 1] = 0.0f;
    matrix[3 * 4 + 2] = 0.0f;
    matrix[3 * 4 + 3] = 1.0f;
}

void TranslationMatrixQ1(float matrix[16], float x, float y, float z)
{
    std::memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    matrix[15] = 1.0f;
}

void MultiplyMatrixQ1(float left[16], const float right[16])
{
    float temp[16];
    std::memcpy(temp, left, 16 * sizeof(float));

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = 0.0f;
            for (int i = 0; i < 4; ++i) {
                value += temp[i * 4 + row] * right[column * 4 + i];
            }
            left[column * 4 + row] = value;
        }
    }
}

void BuildQ1ViewMatrix(const refdef_t &def, float out[16])
{
    const float *forward = def.viewaxis[0];
    float yaw = 0.0f;
    float pitch = 0.0f;

    if (forward[0] == 0.0f && forward[1] == 0.0f) {
        yaw = 0.0f;
        pitch = forward[2] > 0.0f ? 90.0f : 270.0f;
    } else {
        yaw = std::atan2(forward[1], forward[0]) * 180.0f / kPi;
        if (yaw < 0.0f) {
            yaw += 360.0f;
        }
        const float horizontal = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        pitch = std::atan2(forward[2], horizontal) * 180.0f / kPi;
        if (pitch < 0.0f) {
            pitch += 360.0f;
        }
    }
    pitch = -pitch;

    float rotation[16] = {};
    RotationMatrixQ1(out, -kPi / 2.0f, 1.0f, 0.0f, 0.0f);
    RotationMatrixQ1(rotation, kPi / 2.0f, 0.0f, 0.0f, 1.0f);
    MultiplyMatrixQ1(out, rotation);
    RotationMatrixQ1(rotation, -(0.0f) * (kPi / 180.0f), 1.0f, 0.0f, 0.0f);
    MultiplyMatrixQ1(out, rotation);
    RotationMatrixQ1(rotation, -pitch * (kPi / 180.0f), 0.0f, 1.0f, 0.0f);
    MultiplyMatrixQ1(out, rotation);
    RotationMatrixQ1(rotation, -yaw * (kPi / 180.0f), 0.0f, 0.0f, 1.0f);
    MultiplyMatrixQ1(out, rotation);

    float translation[16] = {};
    TranslationMatrixQ1(translation, -def.vieworg[0], -def.vieworg[1], -def.vieworg[2]);
    MultiplyMatrixQ1(out, translation);
}

void BuildViewMatrix(const refdef_t &def, float out[16])
{
    const float *axis0 = def.viewaxis[0];
    const float *axis1 = def.viewaxis[1];
    const float *axis2 = def.viewaxis[2];
    const float *origin = def.vieworg;
    const int mode = qrayViewMode != nullptr ? qrayViewMode->integer : 7;

    if (mode == 4) {
        BuildQ1ViewMatrix(def, out);
        return;
    }

    if (mode == 7) {
        const float rightVector[3] = {-axis1[0], -axis1[1], -axis1[2]};
        const float upVector[3] = {axis2[0], axis2[1], axis2[2]};
        const float backVector[3] = {-axis0[0], -axis0[1], -axis0[2]};
        out[0] = rightVector[0];
        out[1] = upVector[0];
        out[2] = backVector[0];
        out[3] = 0.0f;
        out[4] = rightVector[1];
        out[5] = upVector[1];
        out[6] = backVector[1];
        out[7] = 0.0f;
        out[8] = rightVector[2];
        out[9] = upVector[2];
        out[10] = backVector[2];
        out[11] = 0.0f;
        out[12] = -(origin[0] * rightVector[0] + origin[1] * rightVector[1] + origin[2] * rightVector[2]);
        out[13] = -(origin[0] * upVector[0] + origin[1] * upVector[1] + origin[2] * upVector[2]);
        out[14] = -(origin[0] * backVector[0] + origin[1] * backVector[1] + origin[2] * backVector[2]);
        out[15] = 1.0f;
        return;
    }

    if (mode == 6) {
        const float rightVector[3] = {-axis1[0], -axis1[1], -axis1[2]};
        const float upVector[3] = {axis2[0], axis2[1], axis2[2]};
        const float backVector[3] = {-axis0[0], -axis0[1], -axis0[2]};
        out[0] = rightVector[0];
        out[1] = rightVector[1];
        out[2] = rightVector[2];
        out[3] = 0.0f;
        out[4] = upVector[0];
        out[5] = upVector[1];
        out[6] = upVector[2];
        out[7] = 0.0f;
        out[8] = backVector[0];
        out[9] = backVector[1];
        out[10] = backVector[2];
        out[11] = 0.0f;
        out[12] = -(origin[0] * rightVector[0] + origin[1] * upVector[0] + origin[2] * backVector[0]);
        out[13] = -(origin[0] * rightVector[1] + origin[1] * upVector[1] + origin[2] * backVector[1]);
        out[14] = -(origin[0] * rightVector[2] + origin[1] * upVector[2] + origin[2] * backVector[2]);
        out[15] = 1.0f;
        return;
    }

    if (mode == 5) {
        float viewer[16] = {
            axis0[0], axis1[0], axis2[0], 0.0f,
            axis0[1], axis1[1], axis2[1], 0.0f,
            axis0[2], axis1[2], axis2[2], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        const float flip[16] = {
            0.0f, 0.0f, -1.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                out[row * 4 + column] =
                    viewer[row * 4 + 0] * flip[0 * 4 + column] +
                    viewer[row * 4 + 1] * flip[1 * 4 + column] +
                    viewer[row * 4 + 2] * flip[2 * 4 + column] +
                    viewer[row * 4 + 3] * flip[3 * 4 + column];
            }
        }

        for (int column = 0; column < 3; ++column) {
            out[12 + column] = -(
                origin[0] * out[0 + column] +
                origin[1] * out[4 + column] +
                origin[2] * out[8 + column]);
        }
        return;
    }

    if (mode == 0) {
        float viewer[16] = {
            axis0[0], axis1[0], axis2[0], -(origin[0] * axis0[0] + origin[1] * axis0[1] + origin[2] * axis0[2]),
            axis0[1], axis1[1], axis2[1], -(origin[0] * axis1[0] + origin[1] * axis1[1] + origin[2] * axis1[2]),
            axis0[2], axis1[2], axis2[2], -(origin[0] * axis2[0] + origin[1] * axis2[1] + origin[2] * axis2[2]),
            0.0f, 0.0f, 0.0f, 1.0f
        };

        const float flip[16] = {
            0.0f, 0.0f, -1.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                out[row * 4 + column] =
                    viewer[row * 4 + 0] * flip[0 * 4 + column] +
                    viewer[row * 4 + 1] * flip[1 * 4 + column] +
                    viewer[row * 4 + 2] * flip[2 * 4 + column] +
                    viewer[row * 4 + 3] * flip[3 * 4 + column];
            }
        }
        return;
    }

    const float right[3] = {-axis1[0], -axis1[1], -axis1[2]};
    const float up[3] = {axis2[0], axis2[1], axis2[2]};
    const float back[3] = {-axis0[0], -axis0[1], -axis0[2]};

    if (mode == 1) {
        const float row0[3] = {-up[0], -up[1], -up[2]};
        const float row1[3] = {-back[0], -back[1], -back[2]};
        const float row2[3] = {right[0], right[1], right[2]};

        out[0] = row0[0];
        out[1] = row0[1];
        out[2] = row0[2];
        out[3] = 0.0f;
        out[4] = row1[0];
        out[5] = row1[1];
        out[6] = row1[2];
        out[7] = 0.0f;
        out[8] = row2[0];
        out[9] = row2[1];
        out[10] = row2[2];
        out[11] = 0.0f;
        out[12] = -(row0[0] * origin[0] + row0[1] * origin[1] + row0[2] * origin[2]);
        out[13] = -(row1[0] * origin[0] + row1[1] * origin[1] + row1[2] * origin[2]);
        out[14] = -(row2[0] * origin[0] + row2[1] * origin[1] + row2[2] * origin[2]);
        out[15] = 1.0f;
        return;
    }

    if (mode == 2) {
        out[0] = right[0];
        out[1] = right[1];
        out[2] = right[2];
        out[3] = 0.0f;
        out[4] = up[0];
        out[5] = up[1];
        out[6] = up[2];
        out[7] = 0.0f;
        out[8] = back[0];
        out[9] = back[1];
        out[10] = back[2];
        out[11] = 0.0f;
        out[12] = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
        out[13] = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
        out[14] = -(back[0] * origin[0] + back[1] * origin[1] + back[2] * origin[2]);
        out[15] = 1.0f;
        return;
    }

    out[0] = right[0];
    out[1] = right[1];
    out[2] = right[2];
    out[3] = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
    out[4] = up[0];
    out[5] = up[1];
    out[6] = up[2];
    out[7] = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
    out[8] = back[0];
    out[9] = back[1];
    out[10] = back[2];
    out[11] = -(back[0] * origin[0] + back[1] * origin[1] + back[2] * origin[2]);
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

void BuildOverlayMatrix(float out[16], QrViewport &viewport)
{
    const float width = static_cast<float>(std::max(1, glConfig.vidWidth));
    const float height = static_cast<float>(std::max(1, glConfig.vidHeight));

    out[0] = 2.0f / width;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;

    out[4] = 0.0f;
    out[5] = 2.0f / height;
    out[6] = 0.0f;
    out[7] = 0.0f;

    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = -1.0f;
    out[11] = 0.0f;

    out[12] = -1.0f;
    out[13] = -1.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
}

QrTransform IdentityTransform()
{
    QrTransform result{};
    result.matrix[0][0] = 1.0f;
    result.matrix[1][1] = 1.0f;
    result.matrix[2][2] = 1.0f;
    return result;
}

int ParseFloats(const char *text, float *values, int maxValues)
{
    if (text == nullptr || maxValues <= 0) {
        return 0;
    }
    int count = 0;
    const char *cursor = text;
    while (count < maxValues) {
        char *end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor) {
            break;
        }
        values[count++] = value;
        cursor = end;
    }
    return count;
}

std::vector<std::string> TokenizeEntities(const char *data, size_t length)
{
    std::vector<std::string> tokens;
    size_t index = 0;
    while (index < length) {
        while (index < length && std::isspace(static_cast<unsigned char>(data[index]))) {
            ++index;
        }
        if (index >= length) {
            break;
        }
        if (data[index] == '{' || data[index] == '}') {
            tokens.emplace_back(1, data[index]);
            ++index;
            continue;
        }
        if (data[index] == '"') {
            ++index;
            const size_t start = index;
            while (index < length && data[index] != '"') {
                ++index;
            }
            tokens.emplace_back(data + start, index - start);
            if (index < length) {
                ++index;
            }
            continue;
        }
        const size_t start = index;
        while (index < length && !std::isspace(static_cast<unsigned char>(data[index])) &&
            data[index] != '{' && data[index] != '}') {
            ++index;
        }
        tokens.emplace_back(data + start, index - start);
    }
    return tokens;
}

void LoadMapLights(const char *entityString, size_t entityLength)
{
    mapLights.clear();
    const std::vector<std::string> tokens = TokenizeEntities(entityString, entityLength);
    std::unordered_map<std::string, std::string> keys;

    size_t index = 0;
    while (index < tokens.size()) {
        if (tokens[index] != "{") {
            ++index;
            continue;
        }
        ++index;
        keys.clear();
        while (index < tokens.size() && tokens[index] != "}") {
            const std::string &key = tokens[index++];
            if (index >= tokens.size() || tokens[index] == "}") {
                break;
            }
            keys[key] = tokens[index++];
        }
        if (index < tokens.size() && tokens[index] == "}") {
            ++index;
        }

        const auto classname = keys.find("classname");
        if (classname == keys.end() || (classname->second != "light" && classname->second != "lightJunior")) {
            continue;
        }
        const auto originIt = keys.find("origin");
        const auto lightIt = keys.find("light");
        if (originIt == keys.end() || lightIt == keys.end()) {
            continue;
        }

        float origin[3] = {};
        if (ParseFloats(originIt->second.c_str(), origin, 3) < 3) {
            continue;
        }

        float lightValues[3] = {};
        const int lightCount = ParseFloats(lightIt->second.c_str(), lightValues, 3);
        if (lightCount <= 0) {
            continue;
        }

        float color[3] = {1.0f, 1.0f, 1.0f};
        float intensity = lightValues[0];
        if (lightCount >= 3) {
            color[0] = lightValues[0];
            color[1] = lightValues[1];
            color[2] = lightValues[2];
            intensity = 1.0f;
        }

        const auto colorIt = keys.find("_color");
        if (colorIt != keys.end()) {
            float parsedColor[3] = {};
            if (ParseFloats(colorIt->second.c_str(), parsedColor, 3) >= 3) {
                color[0] = parsedColor[0];
                color[1] = parsedColor[1];
                color[2] = parsedColor[2];
            }
        }

        float radius = 300.0f;
        const auto radiusIt = keys.find("radius");
        if (radiusIt != keys.end()) {
            float parsedRadius = 0.0f;
            if (ParseFloats(radiusIt->second.c_str(), &parsedRadius, 1) == 1 && parsedRadius > 0.0f) {
                radius = parsedRadius;
            }
        }

        if (intensity <= 0.0f) {
            continue;
        }

        MapLight light{};
        light.position[0] = origin[0];
        light.position[1] = origin[1];
        light.position[2] = origin[2];
        light.color[0] = color[0] * intensity;
        light.color[1] = color[1] * intensity;
        light.color[2] = color[2] * intensity;
        light.radius = radius;
        mapLights.push_back(light);
    }

    HostLog("LoadMapLights: %zu light entities", mapLights.size());
}

bool HostPointInLeafReady()
{
    return !bspClusterTree.leafClusters.empty();
}

int NearbyBspClusterForPoint(const float point[3], bool *neededNudge = nullptr)
{
    if (neededNudge != nullptr) {
        *neededNudge = false;
    }
    if (!HostPointInLeafReady()) {
        return -1;
    }

    int cluster = bspClusterTree.ClusterForPoint(point);
    if (cluster >= 1 && static_cast<uint32_t>(cluster) < worldClusters.clusterCount) {
        return cluster;
    }

    // Q3 surface vertices and light entities routinely lie in a solid leaf on the boundary
    // of the lit volume. Probe a few short axis offsets so they inherit a real adjacent leaf.
    static const float kProbeOffsets[] = {0.25f, 1.0f, 4.0f, 16.0f, 48.0f, 128.0f};
    for (float offset : kProbeOffsets) {
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign = -1; sign <= 1; sign += 2) {
                float moved[3] = {point[0], point[1], point[2]};
                moved[axis] += offset * static_cast<float>(sign);
                cluster = bspClusterTree.ClusterForPoint(moved);
                if (cluster >= 1 && static_cast<uint32_t>(cluster) < worldClusters.clusterCount) {
                    if (neededNudge != nullptr) {
                        *neededNudge = true;
                    }
                    return cluster;
                }
            }
        }
    }

    return -1;
}

uint32_t ClusterIndexForPoint(const float point[3])
{
    const int cluster = NearbyBspClusterForPoint(point);
    if (cluster < 1 || static_cast<uint32_t>(cluster) >= worldClusters.clusterCount) {
        return 0u;
    }
    return static_cast<uint32_t>(cluster);
}

void AccumulateClusterBounds(uint32_t cluster, const float position[3])
{
    if (cluster >= worldClusters.clusterCount) {
        return;
    }

    QrFloat3D &mins = worldClusters.mins[cluster];
    QrFloat3D &maxs = worldClusters.maxs[cluster];
    for (int axis = 0; axis < 3; ++axis) {
        mins.data[axis] = std::min(mins.data[axis], position[axis]);
        maxs.data[axis] = std::max(maxs.data[axis], position[axis]);
    }
    worldClusters.bounded[cluster] = 1;
}

void MarkSkyCluster(uint32_t cluster)
{
    if (cluster < worldClusters.skyFlags.size() && worldClusters.skyFlags[cluster] == 0) {
        worldClusters.skyFlags[cluster] = 1;
    }
}

void ResetWorldClusters()
{
    bspClusterTree.nodes.clear();
    bspClusterTree.leafClusters.clear();
    worldClusters = WorldClusterTables{};
    mapLightClusters.clear();
}

void LoadBspClusterTree(const dplane_t *planes, int planeCount, const dnode_t *nodes, int nodeCount,
    const dleaf_t *leaves, int leafCount)
{
    bspClusterTree.leafClusters.assign(static_cast<size_t>(leafCount), -1);

    uint32_t clusterCount = 0;
    for (int leaf = 0; leaf < leafCount; ++leaf) {
        const int cluster = leaves[leaf].cluster;
        if (cluster < 0) {
            // Solid leaves carry no visibility row of their own.
            continue;
        }
        // The SDK reserves cluster 0. Q3's PVS row bit c maps to SDK cluster c + 1.
        const uint32_t sdkCluster = static_cast<uint32_t>(cluster) + 1u;
        bspClusterTree.leafClusters[static_cast<size_t>(leaf)] = static_cast<int>(sdkCluster);
        clusterCount = std::max(clusterCount, sdkCluster + 1u);
    }

    if (clusterCount > kMaxLightClusters) {
        ri.Printf(PRINT_WARNING,
            "rendererqray: the map has %u light clusters, only the first %u get a light list\n",
            clusterCount - 1u, kMaxLightClusters - 1u);
        clusterCount = kMaxLightClusters;
    }

    worldClusters.clusterCount = clusterCount;
    worldClusters.mins.assign(clusterCount, QrFloat3D{});
    worldClusters.maxs.assign(clusterCount, QrFloat3D{});
    worldClusters.flags.assign(clusterCount, 0);
    worldClusters.bounded.assign(clusterCount, 0);
    worldClusters.skyFlags.assign(clusterCount, 0);
    for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
        for (int axis = 0; axis < 3; ++axis) {
            worldClusters.mins[cluster].data[axis] = FLT_MAX;
            worldClusters.maxs[cluster].data[axis] = -FLT_MAX;
        }
    }

    bspClusterTree.nodes.resize(static_cast<size_t>(nodeCount));
    for (int node = 0; node < nodeCount; ++node) {
        const dnode_t &source = nodes[node];
        BspClusterTree::Node &destination = bspClusterTree.nodes[static_cast<size_t>(node)];
        destination.children[0] = source.children[0];
        destination.children[1] = source.children[1];
        if (source.planeNum < 0 || source.planeNum >= planeCount) {
            destination.normal[0] = 0.0f;
            destination.normal[1] = 0.0f;
            destination.normal[2] = 0.0f;
            destination.dist = 0.0f;
            continue;
        }
        const dplane_t &plane = planes[source.planeNum];
        destination.normal[0] = plane.normal[0];
        destination.normal[1] = plane.normal[1];
        destination.normal[2] = plane.normal[2];
        destination.dist = plane.dist;
    }

    HostLog("LoadBspClusterTree: %u clusters, %d nodes, %d leaves", worldClusters.clusterCount,
        nodeCount, leafCount);
}

void ResolveMapLightClusters()
{
    mapLightClusters.assign(mapLights.size(), 0u);
    if (!HostPointInLeafReady()) {
        return;
    }

    int resolved = 0;
    int displaced = 0;

    for (size_t index = 0; index < mapLights.size(); ++index) {
        const MapLight &light = mapLights[index];
        bool nudged = false;
        const int cluster = NearbyBspClusterForPoint(light.position, &nudged);
        if (nudged) {
            ++displaced;
        }

        if (cluster < 1 || static_cast<uint32_t>(cluster) >= worldClusters.clusterCount) {
            continue;
        }

        mapLightClusters[index] = static_cast<uint32_t>(cluster);
        ++resolved;
    }

    HostLog("ResolveMapLightClusters: %d of %zu light entities resolved, %d needed a nudge",
        resolved, mapLights.size(), displaced);
}

void BuildClusterPvs(const uint8_t *visibility, uint32_t visibilitySize)
{
    const uint32_t clusterCount = worldClusters.clusterCount;
    worldClusters.pvsRowBytes = (clusterCount + 7) / 8;
    worldClusters.visOffsets.assign(clusterCount, -1);
    worldClusters.visData.clear();
    worldClusters.skyVis.assign((clusterCount + 7) / 8, 0);

    if (visibility == nullptr || visibilitySize < 8) {
        HostLog("BuildClusterPvs: no visibility lump, lights fall back to the cluster top up");
        return;
    }

    int32_t visClusters = 0;
    int32_t visRowBytes = 0;
    std::memcpy(&visClusters, visibility, sizeof(visClusters));
    std::memcpy(&visRowBytes, visibility + sizeof(visClusters), sizeof(visRowBytes));
    const uint64_t neededBytes = 8ull + static_cast<uint64_t>(visClusters) * static_cast<uint64_t>(visRowBytes);
    if (visClusters <= 0 || visRowBytes <= 0 || neededBytes > visibilitySize) {
        HostLog("BuildClusterPvs: malformed visibility lump clusters=%d rowBytes=%d size=%u",
            visClusters, visRowBytes, visibilitySize);
        return;
    }

    uint32_t skyClusters = 0;
    uint32_t encodedRows = 0;
    const uint32_t rowBytes = worldClusters.pvsRowBytes;
    std::vector<uint8_t> row(rowBytes);
    for (uint32_t cluster = 0; cluster + 1u < clusterCount && static_cast<int32_t>(cluster) < visClusters; ++cluster) {
        const uint32_t sdkCluster = cluster + 1u;
        const uint8_t *source = visibility + 8 +
            static_cast<size_t>(cluster) * static_cast<size_t>(visRowBytes);

        if (sdkCluster < worldClusters.skyFlags.size() && worldClusters.skyFlags[sdkCluster] != 0) {
            ++skyClusters;
            for (uint32_t byte = 0; byte < rowBytes; ++byte) {
                uint32_t bits = 0;
                if (byte < static_cast<uint32_t>(visRowBytes)) {
                    bits |= static_cast<uint32_t>(source[byte]) << 1;
                }
                if (byte > 0 && byte - 1u < static_cast<uint32_t>(visRowBytes)) {
                    bits |= static_cast<uint32_t>(source[byte - 1u]) >> 7;
                }
                worldClusters.skyVis[byte] |= static_cast<uint8_t>(bits & 0xFFu);
            }
        }

        // Q3 row bit c names Q3 cluster c. The SDK stores that row at cluster c + 1 and
        // itself maps row bit c to SDK cluster c + 1 during the PVS walk.
        std::fill(row.begin(), row.end(), 0);
        std::memcpy(row.data(), source, std::min(rowBytes, static_cast<uint32_t>(visRowBytes)));

        worldClusters.visOffsets[sdkCluster] = static_cast<int32_t>(worldClusters.visData.size());
        for (uint32_t byte = 0; byte < rowBytes;) {
            if (row[byte] != 0) {
                worldClusters.visData.push_back(row[byte]);
                ++byte;
                continue;
            }
            uint32_t run = 0;
            while (byte < rowBytes && row[byte] == 0 && run < 255u) {
                ++byte;
                ++run;
            }
            worldClusters.visData.push_back(0);
            worldClusters.visData.push_back(static_cast<uint8_t>(run));
        }
        ++encodedRows;
    }

    HostLog("BuildClusterPvs: %u Q3 rows into %u SDK clusters, %u sky clusters, %zu encoded bytes, row %u",
        encodedRows, clusterCount, skyClusters, worldClusters.visData.size(), rowBytes);
}

void FinalizeWorldClusters(const float worldMin[3], const float worldMax[3], const uint8_t *visibility,
    uint32_t visibilitySize)
{
    if (worldClusters.clusterCount == 0) {
        return;
    }

    uint32_t unbounded = 0;
    for (uint32_t cluster = 0; cluster < worldClusters.clusterCount; ++cluster) {
        if (worldClusters.bounded[cluster] != 0) {
            continue;
        }
        ++unbounded;
        for (int axis = 0; axis < 3; ++axis) {
            worldClusters.mins[cluster].data[axis] = worldMin[axis];
            worldClusters.maxs[cluster].data[axis] = worldMax[axis];
        }
    }

    BuildClusterPvs(visibility, visibilitySize);
    HostLog("FinalizeWorldClusters: %u clusters, %u without geometry", worldClusters.clusterCount, unbounded);
}

void UploadWorldClusters()
{
    if (qrayInstance == QR_NULL_HANDLE || worldClusters.clusterCount == 0) {
        return;
    }

    QrWorldLightsUploadInfo info{};
    info.flags = QR_WORLD_LIGHTS_UPLOAD_PRINT_STATS_BIT;
    info.numClusters = worldClusters.clusterCount;
    static std::vector<QrFloat3D> meterClusterMins;
    static std::vector<QrFloat3D> meterClusterMaxs;
    meterClusterMins = worldClusters.mins;
    meterClusterMaxs = worldClusters.maxs;
    for (uint32_t cluster = 0; cluster < worldClusters.clusterCount; ++cluster) {
        for (int axis = 0; axis < 3; ++axis) {
            meterClusterMins[cluster].data[axis] =
                QuakeUnitsToMeters(meterClusterMins[cluster].data[axis]);
            meterClusterMaxs[cluster].data[axis] =
                QuakeUnitsToMeters(meterClusterMaxs[cluster].data[axis]);
        }
    }
    info.pClusterMins = meterClusterMins.data();
    info.pClusterMaxs = meterClusterMaxs.data();
    info.pClusterFlags = worldClusters.flags.data();
    info.numFaces = 0;
    info.pFaces = nullptr;
    info.numFaceVertices = 0;
    info.pFaceVertices = nullptr;
    info.pVisData = worldClusters.visData.empty() ? nullptr : worldClusters.visData.data();
    info.visDataSize = static_cast<uint32_t>(worldClusters.visData.size());
    info.pvsRowBytes = worldClusters.pvsRowBytes;
    info.pVisOffsets = worldClusters.visOffsets.data();
    info.pClusterSkyVisibility = worldClusters.skyVis.empty() ? nullptr : worldClusters.skyVis.data();
    info.clusterGeneration = ++worldClusterGeneration;

    const QrResult result = qrUploadWorldLights(qrayInstance, &info);
    worldClusters.uploaded = result == QR_SUCCESS;
    HostLog("UploadWorldClusters: clusters=%u pvs=%u bytes row=%u generation=%u result=%d",
        worldClusters.clusterCount, info.visDataSize, info.pvsRowBytes, info.clusterGeneration,
        static_cast<int>(result));
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrUploadWorldLights failed: %d\n", static_cast<int>(result));
    }
}

void UploadClusterLightSources()
{
    if (qrayInstance == QR_NULL_HANDLE || !worldClusters.uploaded ||
        (mapLights.empty() && dynamicLights.empty())) {
        return;
    }

    const float lightScale = CvarFloat(qrayLightScale, 200.0f);
    const float dlightScale = CvarFloat(qrayDlightScale, 200.0f);
    const float reachScale = CvarFloat(qrayLightRadiusScale, 1.0f);
    const float topUpReach = QuakeUnitsToMeters(CvarFloat(qrayLightTopUp, 1024.0f));

    static std::vector<QrClusterLightSource> sources;
    sources.resize(mapLights.size() + dynamicLights.size());

    for (size_t index = 0; index < mapLights.size(); ++index) {
        const MapLight &light = mapLights[index];
        QrClusterLightSource &source = sources[index];
        source = QrClusterLightSource{};
        source.uniqueID = 1000 + index;
        QuakePositionToMeters(light.position, source.origin);
        source.cluster = index < mapLightClusters.size()
            ? mapLightClusters[index]
            : static_cast<uint32_t>(QR_CLUSTER_LIGHT_NO_CLUSTER);
        source.reach = 0.0f;
        source.radius = QuakeUnitsToMeters(std::max(1.0f, light.radius * reachScale));
        source.clusterCount = 0;
        source.pClusters = nullptr;
        source.power = (light.color[0] + light.color[1] + light.color[2]) * lightScale / 3.0f;
    }

    for (size_t index = 0; index < dynamicLights.size(); ++index) {
        const DynamicLight &light = dynamicLights[index];
        QrClusterLightSource &source = sources[mapLights.size() + index];
        source = QrClusterLightSource{};
        source.uniqueID = kDynamicLightUniqueIdBase + index;
        QuakePositionToMeters(light.origin, source.origin);
        const int cluster = NearbyBspClusterForPoint(light.origin);
        source.cluster = cluster >= 1 ? static_cast<uint32_t>(cluster)
                                      : static_cast<uint32_t>(QR_CLUSTER_LIGHT_NO_CLUSTER);
        source.reach = 0.0f;
        source.radius = QuakeUnitsToMeters(std::max(1.0f, light.radius));
        source.clusterCount = 0;
        source.pClusters = nullptr;
        source.power = (light.color[0] + light.color[1] + light.color[2]) * dlightScale / 3.0f;
    }

    QrClusterLightSourcesUploadInfo info{};
    info.numLights = static_cast<uint32_t>(sources.size());
    info.pLights = sources.data();
    info.topUpReach = topUpReach;
    info.allowIncremental = 1;
    info.allowOverflow = 1;
    info.validate = 0;

    const QrResult result = qrUploadClusterLightSources(qrayInstance, &info);
    if (result != QR_SUCCESS) {
        HostLog("UploadClusterLightSources: %zu sources failed result=%d", sources.size(),
            static_cast<int>(result));
    } else {
        static uint32_t loggedClusterGeneration = 0;
        static uint32_t loggedSourceCount = 0;
        const uint32_t sourceCount = static_cast<uint32_t>(sources.size());
        if (loggedClusterGeneration != worldClusterGeneration || loggedSourceCount != sourceCount) {
            loggedClusterGeneration = worldClusterGeneration;
            loggedSourceCount = sourceCount;
            QrClusterLightStats stats{};
            if (qrGetClusterLightStats(qrayInstance, &stats) == QR_SUCCESS) {
                HostLog("ClusterLightStats: clusters=%u sources=%u unresolved=%u entries=%u "
                    "grants=%u topUp=%u reachGated=%u full=%u",
                    stats.clusters, stats.sources, stats.unresolved, stats.listEntries,
                    stats.grants, stats.topUpGrants, stats.reachGated, stats.fullClusters);
            }
        }
    }
}

// A Quake 3 patch is a grid of biquadratic Bezier sub-patches that share their edge
// rows and columns (control points step by two), not one Bezier of degree (W-1, H-1).
// Evaluating it as a single high-degree surface pulls wide patches away from their
// shared control points and opens cracks against the neighbouring brushwork.
void EvaluatePatch(const drawVert_t *control, int width, int height,
    float u, float v, float outPosition[3], float outTexCoord[2])
{
    outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
    outTexCoord[0] = outTexCoord[1] = 0.0f;

    const int subPatchesU = std::max(1, (width - 1) / 2);
    const int subPatchesV = std::max(1, (height - 1) / 2);
    float fu = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(subPatchesU);
    float fv = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(subPatchesV);
    const int patchU = std::min(static_cast<int>(fu), subPatchesU - 1);
    const int patchV = std::min(static_cast<int>(fv), subPatchesV - 1);
    fu -= static_cast<float>(patchU);
    fv -= static_cast<float>(patchV);

    const float basisU[3] = {(1.0f - fu) * (1.0f - fu), 2.0f * fu * (1.0f - fu), fu * fu};
    const float basisV[3] = {(1.0f - fv) * (1.0f - fv), 2.0f * fv * (1.0f - fv), fv * fv};
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            const float weight = basisU[x] * basisV[y];
            const drawVert_t &point = control[(patchV * 2 + y) * width + (patchU * 2 + x)];
            outPosition[0] += point.xyz[0] * weight;
            outPosition[1] += point.xyz[1] * weight;
            outPosition[2] += point.xyz[2] * weight;
            outTexCoord[0] += point.st[0] * weight;
            outTexCoord[1] += point.st[1] * weight;
        }
    }
}

struct BspSurfaceRange
{
    int firstSurface;
    int numSurfaces;
};

// Q3 sky face order follows the skyparms suffixes; the (u, v) remap turns each
// Q3 face into the axis orientation a cubemap sampler expects.
struct SkyFaceMapping
{
    const char *suffix;
    int remap;
};

constexpr SkyFaceMapping kSkyFaceMappings[6] = {
    {"rt", 0}, // +X
    {"lf", 1}, // -X
    {"bk", 2}, // +Y
    {"ft", 3}, // -Y
    {"up", 0}, // +Z
    {"dn", 0}, // -Z
};

constexpr int kSkyCubeFaceSize = 256;
constexpr float kSkyBoxRadius = 2048.0f;

void SampleSkyFace(const std::vector<uint8_t> &source, int sourceWidth, int sourceHeight,
    int remap, int destinationX, int destinationY, int destinationSize, uint8_t *outPixel)
{
    const float u = (static_cast<float>(destinationX) + 0.5f) / static_cast<float>(destinationSize);
    const float v = (static_cast<float>(destinationY) + 0.5f) / static_cast<float>(destinationSize);
    float sourceU = u;
    float sourceV = v;
    switch (remap) {
    case 0:
        sourceU = v;
        sourceV = 1.0f - u;
        break;
    case 1:
        sourceU = 1.0f - v;
        sourceV = u;
        break;
    case 2:
        break;
    case 3:
        sourceU = 1.0f - u;
        sourceV = 1.0f - v;
        break;
    default:
        break;
    }

    const int x = std::clamp(static_cast<int>(sourceU * static_cast<float>(sourceWidth)),
        0, sourceWidth - 1);
    const int y = std::clamp(static_cast<int>(sourceV * static_cast<float>(sourceHeight)),
        0, sourceHeight - 1);
    const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(sourceWidth) +
        static_cast<size_t>(x)) * 4;
    outPixel[0] = source[offset + 0];
    outPixel[1] = source[offset + 1];
    outPixel[2] = source[offset + 2];
    outPixel[3] = source[offset + 3];
}

// Direction reconstruction per cubemap face, matching the layout the sampler uses.
void SkyFaceDirection(int face, float s, float t, float direction[3])
{
    switch (face) {
    case 0: // +X
        direction[0] = 1.0f;
        direction[1] = -t;
        direction[2] = -s;
        break;
    case 1: // -X
        direction[0] = -1.0f;
        direction[1] = -t;
        direction[2] = s;
        break;
    case 2: // +Y
        direction[0] = s;
        direction[1] = 1.0f;
        direction[2] = t;
        break;
    case 3: // -Y
        direction[0] = s;
        direction[1] = -1.0f;
        direction[2] = -t;
        break;
    case 4: // +Z
        direction[0] = s;
        direction[1] = -t;
        direction[2] = 1.0f;
        break;
    default: // -Z
        direction[0] = -s;
        direction[1] = -t;
        direction[2] = -1.0f;
        break;
    }
}

void SampleCloudStage(const std::vector<uint8_t> &pixels, int width, int height,
    const SkyStage &stage, const float direction[3], float cloudHeight, uint8_t *outPixel)
{
    const float denominator = std::max(std::fabs(direction[2]), 0.05f);
    const float scale = cloudHeight / denominator;
    const float inverseBox = 1.0f / (2.0f * kSkyBoxRadius);
    float u = 0.5f + direction[0] * scale * inverseBox;
    float v = 0.5f + direction[1] * scale * inverseBox;
    u *= stage.scale[0];
    v *= stage.scale[1];
    u -= std::floor(u);
    v -= std::floor(v);

    const int x = std::clamp(static_cast<int>(u * static_cast<float>(width)), 0, width - 1);
    const int y = std::clamp(static_cast<int>(v * static_cast<float>(height)), 0, height - 1);
    const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) +
        static_cast<size_t>(x)) * 4;
    outPixel[0] = pixels[offset + 0];
    outPixel[1] = pixels[offset + 1];
    outPixel[2] = pixels[offset + 2];
    outPixel[3] = 255;
}

void DestroySkyCubemap()
{
    if (skyCubemap != QR_EMPTY_CUBEMAP && qrayInstance != QR_NULL_HANDLE) {
        qrDestroyCubemap(qrayInstance, skyCubemap);
    }
    skyCubemap = QR_EMPTY_CUBEMAP;
    skyAverageColor[0] = 0.20f;
    skyAverageColor[1] = 0.25f;
    skyAverageColor[2] = 0.35f;
}

bool CreateSkyCubemapFromBox(const SkyShaderInfo &sky, std::vector<std::vector<uint8_t>> &faces)
{
    int faceWidth = 0;
    int faceHeight = 0;
    std::vector<int> widths(6, 0);
    std::vector<int> heights(6, 0);
    std::vector<std::vector<uint8_t>> sources(6);
    for (int face = 0; face < 6; ++face) {
        const std::string textureName = sky.outerBox + "_" + kSkyFaceMappings[face].suffix;
        if (!LoadQ3Texture(textureName.c_str(), widths[face], heights[face], sources[face])) {
            HostLog("CreateSkyCubemapFromBox: missing %s", textureName.c_str());
            return false;
        }
        if (faceWidth == 0) {
            faceWidth = widths[face];
            faceHeight = heights[face];
        }
    }
    if (faceWidth <= 0 || faceHeight <= 0) {
        return false;
    }

    const int size = std::max(faceWidth, faceHeight);
    faces.assign(6, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4, 255));
    for (int face = 0; face < 6; ++face) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                uint8_t pixel[4] = {0, 0, 0, 255};
                SampleSkyFace(sources[face], widths[face], heights[face],
                    kSkyFaceMappings[face].remap, x, y, size, pixel);
                const size_t offset = (static_cast<size_t>(y) * size + static_cast<size_t>(x)) * 4;
                faces[face][offset + 0] = pixel[0];
                faces[face][offset + 1] = pixel[1];
                faces[face][offset + 2] = pixel[2];
                faces[face][offset + 3] = pixel[3];
            }
        }
    }
    return true;
}

bool CreateSkyCubemapFromClouds(const SkyShaderInfo &sky, std::vector<std::vector<uint8_t>> &faces)
{
    struct CloudLayer
    {
        SkyStage stage;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    std::vector<CloudLayer> layers;
    for (const SkyStage &stage : sky.stages) {
        CloudLayer layer;
        layer.stage = stage;
        if (!LoadQ3Texture(stage.map.c_str(), layer.width, layer.height, layer.pixels)) {
            continue;
        }
        layers.push_back(std::move(layer));
    }
    if (layers.empty()) {
        return false;
    }

    const int size = kSkyCubeFaceSize;
    faces.assign(6, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4, 255));
    for (int face = 0; face < 6; ++face) {
        for (int y = 0; y < size; ++y) {
            const float t = 2.0f * ((static_cast<float>(y) + 0.5f) / static_cast<float>(size)) - 1.0f;
            for (int x = 0; x < size; ++x) {
                const float s = 2.0f * ((static_cast<float>(x) + 0.5f) / static_cast<float>(size)) - 1.0f;
                float direction[3] = {};
                SkyFaceDirection(face, s, t, direction);
                const size_t offset = (static_cast<size_t>(y) * size + static_cast<size_t>(x)) * 4;
                int accum[3] = {0, 0, 0};
                for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
                    const CloudLayer &layer = layers[layerIndex];
                    uint8_t pixel[4] = {0, 0, 0, 255};
                    SampleCloudStage(layer.pixels, layer.width, layer.height, layer.stage,
                        direction, sky.cloudHeight, pixel);
                    for (int channel = 0; channel < 3; ++channel) {
                        accum[channel] = layerIndex == 0
                            ? pixel[channel]
                            : std::min(255, accum[channel] + pixel[channel]);
                    }
                }
                faces[face][offset + 0] = static_cast<uint8_t>(accum[0]);
                faces[face][offset + 1] = static_cast<uint8_t>(accum[1]);
                faces[face][offset + 2] = static_cast<uint8_t>(accum[2]);
                faces[face][offset + 3] = 255;
            }
        }
    }
    return true;
}

void ComputeSkyAverageColor(const std::vector<std::vector<uint8_t>> &faces)
{
    double sum[3] = {0.0, 0.0, 0.0};
    size_t samples = 0;
    for (const std::vector<uint8_t> &face : faces) {
        const size_t pixelCount = face.size() / 4;
        const size_t step = std::max<size_t>(1, pixelCount / 4096);
        for (size_t index = 0; index < pixelCount; index += step) {
            sum[0] += face[index * 4 + 0];
            sum[1] += face[index * 4 + 1];
            sum[2] += face[index * 4 + 2];
            ++samples;
        }
    }
    if (samples == 0) {
        return;
    }
    for (int channel = 0; channel < 3; ++channel) {
        const float srgb = static_cast<float>(sum[channel] / static_cast<double>(samples) / 255.0);
        skyAverageColor[channel] = std::clamp(std::pow(srgb, 2.2f), 0.02f, 4.0f);
    }
}

// QuakeRay samples the sky from the cubemap its own sky pass rasterizes, and a sidecar cannot
// feed that pass, so the composed faces are also packed into a single atlas that the overlay
// draws as six inward facing quads. Face order and orientation match SkyFaceDirection, which
// keeps the box, the cloud stages and the drawn geometry consistent with each other.

void DestroySkyAtlas()
{
    if (skyAtlasMaterial != QR_NO_MATERIAL && qrayInstance != QR_NULL_HANDLE) {
        qrDestroyMaterial(qrayInstance, skyAtlasMaterial);
    }
    skyAtlasMaterial = QR_NO_MATERIAL;
    skyAtlasFaceSize = 0;
    skyAtlasAttempted = false;
}

// Retail Q3 bakes the sun into the lightmaps and paints no disc, so the sky maps come without
// one. The atlas is drawn by the overlay instead of the SDK sky pass, which means it has to
// carry the disc itself: a small saturated core plus a wide glow, placed exactly where the
// directional light points.
void AddSunDiscToAtlas(std::vector<uint8_t> &atlas, int atlasWidth, int faceSize)
{
    float lightDirection[3] = {0.35f, 0.45f, -0.82f};
    float color[3] = {1.0f, 0.95f, 0.85f};
    float intensity = 3.0f;
    if (!ResolveSun(lightDirection, color, intensity)) {
        return;
    }

    const float toRadians = 3.14159265358979f / 180.0f;
    const float coreCosine = std::cos(1.1f * toRadians);
    const float glowCosine = std::cos(7.0f * toRadians);
    const float strength = std::clamp(intensity, 0.0f, 4.0f);
    const float sunDirection[3] = {-lightDirection[0], -lightDirection[1], -lightDirection[2]};
    int touched = 0;

    for (int face = 0; face < 6; ++face) {
        const int cellX = (face % 3) * faceSize;
        const int cellY = (face / 3) * faceSize;
        for (int y = 0; y < faceSize; ++y) {
            const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize) * 2.0f - 1.0f;
            for (int x = 0; x < faceSize; ++x) {
                const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize) * 2.0f - 1.0f;
                float direction[3];
                SkyFaceDirection(face, s, t, direction);
                NormalizeVector(direction);
                const float cosine = direction[0] * sunDirection[0] +
                    direction[1] * sunDirection[1] + direction[2] * sunDirection[2];
                if (cosine <= glowCosine) {
                    continue;
                }
                const float glow = cosine >= coreCosine
                    ? 1.0f
                    : (cosine - glowCosine) / (coreCosine - glowCosine);
                const float amount = glow * glow * strength;
                uint8_t *pixel = &atlas[(static_cast<size_t>(cellY + y) * atlasWidth + cellX + x) * 4];
                for (int channel = 0; channel < 3; ++channel) {
                    const float value = pixel[channel] / 255.0f + color[channel] * amount;
                    pixel[channel] = static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
                }
                ++touched;
            }
        }
    }
    HostLog("AddSunDiscToAtlas: dir=(%.2f %.2f %.2f) strength=%.2f pixels=%d",
        sunDirection[0], sunDirection[1], sunDirection[2], strength, touched);
}

void BuildSkyAtlas(const std::vector<std::vector<uint8_t>> &faces)
{
    DestroySkyAtlas();
    if (faces.size() != 6 || faces[0].empty() || qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    const int faceSize = static_cast<int>(std::sqrt(static_cast<double>(faces[0].size()) / 4.0));
    if (faceSize <= 0) {
        return;
    }

    const int atlasWidth = faceSize * 3;
    const int atlasHeight = faceSize * 2;
    std::vector<uint8_t> atlas(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 255);
    for (int face = 0; face < 6; ++face) {
        const std::vector<uint8_t> &source = faces[face];
        if (source.size() < static_cast<size_t>(faceSize) * faceSize * 4) {
            continue;
        }
        const int cellX = (face % 3) * faceSize;
        const int cellY = (face / 3) * faceSize;
        for (int y = 0; y < faceSize; ++y) {
            uint8_t *destination = &atlas[(static_cast<size_t>(cellY + y) * atlasWidth + cellX) * 4];
            const uint8_t *sourceRow = &source[static_cast<size_t>(y) * faceSize * 4];
            std::memcpy(destination, sourceRow, static_cast<size_t>(faceSize) * 4);
            for (int x = 0; x < faceSize; ++x) {
                destination[x * 4 + 3] = 255;
            }
        }
    }

    AddSunDiscToAtlas(atlas, atlasWidth, faceSize);

    skyAtlasMaterial = CreateMaterialFromPixels("_qray/sky/atlas", atlasWidth, atlasHeight, atlas,
        QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);    if (skyAtlasMaterial == QR_NO_MATERIAL) {
        HostLog("BuildSkyAtlas: material creation failed (%dx%d)", atlasWidth, atlasHeight);
        return;
    }
    skyAtlasFaceSize = faceSize;
    HostLog("BuildSkyAtlas: %dx%d atlas, face=%d", atlasWidth, atlasHeight, faceSize);
}

void BuildSkyEnvironment()
{
    DestroySkyCubemap();

    if (qrayInstance == QR_NULL_HANDLE || mapSkyShaderName.empty()) {
        return;
    }

    const auto skyIt = skyShaders.find(NormalizeShaderName(mapSkyShaderName.c_str()));
    if (skyIt == skyShaders.end()) {
        HostLog("BuildSkyEnvironment: no sky shader for '%s'", mapSkyShaderName.c_str());
        return;
    }

    const SkyShaderInfo &sky = skyIt->second;
    std::vector<std::vector<uint8_t>> faces;
    bool built = false;
    if (!sky.outerBox.empty()) {
        built = CreateSkyCubemapFromBox(sky, faces);
        if (!built) {
            HostLog("BuildSkyEnvironment: box '%s' unavailable, trying cloud stages", sky.outerBox.c_str());
        }
    }
    if (!built) {
        built = CreateSkyCubemapFromClouds(sky, faces);
    }
    if (!built) {
        HostLog("BuildSkyEnvironment: '%s' has no usable sky textures", mapSkyShaderName.c_str());
        return;
    }

    const uint32_t sideSize = static_cast<uint32_t>(std::sqrt(static_cast<double>(faces[0].size()) / 4.0));
    QrCubemapCreateInfo createInfo{};
    for (int face = 0; face < 6; ++face) {
        createInfo.pData[face] = faces[face].data();
    }
    createInfo.sideSize = sideSize;
    createInfo.useMipmaps = QR_TRUE;
    createInfo.filter = QR_SAMPLER_FILTER_LINEAR;

    QrCubemap created = QR_EMPTY_CUBEMAP;
    const QrResult result = qrCreateCubemap(qrayInstance, &createInfo, &created);
    if (result != QR_SUCCESS || created == QR_EMPTY_CUBEMAP) {
        HostLog("BuildSkyEnvironment: qrCreateCubemap failed result=%d", static_cast<int>(result));
        return;
    }

    skyCubemap = created;
    ComputeSkyAverageColor(faces);
    BuildSkyAtlas(faces);
    HostLog("BuildSkyEnvironment: '%s' box='%s' stages=%zu size=%u avg=(%.3f %.3f %.3f)",
        mapSkyShaderName.c_str(), sky.outerBox.c_str(), sky.stages.size(), sideSize,
        skyAverageColor[0], skyAverageColor[1], skyAverageColor[2]);
}

void BuildMarkAcceleration(const std::vector<GeometryBatch> &batches)
{
    markTriangles.clear();
    markGrid.clear();

    size_t triangleCount = 0;
    for (const GeometryBatch &batch : batches) {
        triangleCount += batch.indices.size() / 3;
    }
    markTriangles.reserve(triangleCount);

    for (const GeometryBatch &batch : batches) {
        for (size_t index = 0; index + 2 < batch.indices.size(); index += 3) {
            MarkTriangle triangle;
            triangle.sourceTriangleIndex = static_cast<uint32_t>(index / 3);
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t vertexIndex = batch.indices[index + static_cast<size_t>(corner)];
                if (vertexIndex >= batch.vertices.size()) {
                    valid = false;
                    break;
                }
                const QrVertex &vertex = batch.vertices[vertexIndex];
                for (int axis = 0; axis < 3; ++axis) {
                    triangle.points[corner][axis] = vertex.position[axis];
                    triangle.vertexNormals[corner][axis] = vertex.normal[axis];
                }
                for (int axis = 0; axis < 2; ++axis) {
                    triangle.texCoords[corner][axis] = vertex.texCoord[axis];
                }
                if (corner == 0 && vertexIndex < batch.sourceSurfaceIndices.size()) {
                    triangle.sourceSurfaceIndex = batch.sourceSurfaceIndices[vertexIndex];
                }
            }
            if (!valid) {
                continue;
            }

            const float ax = triangle.points[1][0] - triangle.points[0][0];
            const float ay = triangle.points[1][1] - triangle.points[0][1];
            const float az = triangle.points[1][2] - triangle.points[0][2];
            const float bx = triangle.points[2][0] - triangle.points[0][0];
            const float by = triangle.points[2][1] - triangle.points[0][1];
            const float bz = triangle.points[2][2] - triangle.points[0][2];
            float normal[3] = {ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length < 1e-6f) {
                continue;
            }
            triangle.normal[0] = normal[0] / length;
            triangle.normal[1] = normal[1] / length;
            triangle.normal[2] = normal[2] / length;

            int minCell[3] = {};
            int maxCell[3] = {};
            bool inRange = true;
            for (int axis = 0; axis < 3 && inRange; ++axis) {
                float low = triangle.points[0][axis];
                float high = low;
                for (int corner = 1; corner < 3; ++corner) {
                    low = std::min(low, triangle.points[corner][axis]);
                    high = std::max(high, triangle.points[corner][axis]);
                }
                // Malformed BSP coordinates (NaN, huge values) would make the float to int
                // casts undefined and the cell loop below effectively unbounded.
                constexpr float kMaxWorldCoord = 262144.0f;
                if (!std::isfinite(low) || !std::isfinite(high) || low < -kMaxWorldCoord || high > kMaxWorldCoord) {
                    inRange = false;
                    break;
                }
                minCell[axis] = static_cast<int>(std::floor((low - 1.0f) / kMarkCellSize));
                maxCell[axis] = static_cast<int>(std::floor((high + 1.0f) / kMarkCellSize));
            }
            if (!inRange) {
                continue;
            }
            const int64_t cellSpan = static_cast<int64_t>(maxCell[0] - minCell[0] + 1) *
                (maxCell[1] - minCell[1] + 1) * (maxCell[2] - minCell[2] + 1);
            if (cellSpan > 65536) {
                continue;
            }

            const uint32_t triangleIndex = static_cast<uint32_t>(markTriangles.size());
            markTriangles.push_back(triangle);
            for (int x = minCell[0]; x <= maxCell[0]; ++x) {
                for (int y = minCell[1]; y <= maxCell[1]; ++y) {
                    for (int z = minCell[2]; z <= maxCell[2]; ++z) {
                        markGrid[MarkCellKey(x, y, z)].push_back(triangleIndex);
                    }
                }
            }
        }
    }

    HostLog("BuildMarkAcceleration: triangles=%zu cells=%zu", markTriangles.size(), markGrid.size());
}

bool LoadBspGeometryInner(const char *name)
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return false;
    }

    HostLog("LoadBspGeometry: %s", name);
    const int normalMode = qrayNormalMode != nullptr ? qrayNormalMode->integer : 0;
    HostLog("LoadBspGeometry: normalMode=%d cullBackFaces=%d", normalMode,
        qrayCullBackFaces != nullptr ? qrayCullBackFaces->integer : 1);
    LoadShaderScripts();
    DestroySkyCubemap();
    mapSkyShaderName.clear();
    worldSurfaceInfos.clear();
    worldShaderNames.clear();
    worldOverlayBatches.clear();
    for (int i = 0; i < 3; ++i) {
        worldMin[i] = 1.0e30f;
        worldMax[i] = -1.0e30f;
    }
    // Decals must never be projected against the previous map if this load fails.
    markTriangles.clear();
    markGrid.clear();
    void *fileData = nullptr;
    int fileLength = ri.FS_ReadFile(name, &fileData);
    HostLog("LoadBspGeometry: FS_ReadFile len=%d data=%p", fileLength, fileData);
    if (fileLength <= 0 || fileData == nullptr) {
        ri.Printf(PRINT_WARNING, "rendererqray: cannot read %s\n", name);
        return false;
    }
    // Released on every exit path, including exceptions (bad_alloc) on the Linux path.
    struct FileGuard {
        void *data;
        ~FileGuard() { if (data != nullptr) { ri.FS_FreeFile(data); } }
    } fileGuard{fileData};
    // An exception between qrBeginStaticGeometries and qrSubmitStaticGeometries must not
    // leave the SDK's static-geometry window open.
    struct StaticGeometryGuard {
        bool open = false;
        ~StaticGeometryGuard() { if (open && qrayInstance != QR_NULL_HANDLE) { qrSubmitStaticGeometries(qrayInstance); } }
    } staticGuard;

    bool loaded = false;
    const auto *base = static_cast<const uint8_t *>(fileData);
    const auto *header = reinterpret_cast<const dheader_t *>(base);
    if (fileLength >= static_cast<int>(sizeof(dheader_t)) &&
        std::memcmp(&header->ident, "IBSP", 4) == 0 &&
        header->version == BSP_VERSION) {
        const lump_t *lumps = header->lumps;
        const auto lumpValid = [&](int index, size_t elementSize) {
            if (index < 0 || index >= HEADER_LUMPS) {
                return false;
            }
            const lump_t &lump = lumps[index];
            const uint64_t end = static_cast<uint64_t>(lump.fileofs) + lump.filelen;
            return end <= static_cast<uint64_t>(fileLength) &&
                   (elementSize == 0 || (lump.filelen % elementSize) == 0);
        };

        if (lumpValid(LUMP_SHADERS, sizeof(dshader_t)) &&
            lumpValid(LUMP_SURFACES, sizeof(dsurface_t)) &&
            lumpValid(LUMP_DRAWVERTS, sizeof(drawVert_t)) &&
            lumpValid(LUMP_DRAWINDEXES, sizeof(int)) &&
            lumpValid(LUMP_MODELS, sizeof(dmodel_t)) &&
            lumpValid(LUMP_ENTITIES, 1)) {
            const auto *shaders = reinterpret_cast<const dshader_t *>(base + lumps[LUMP_SHADERS].fileofs);
            const int shaderCount = static_cast<int>(lumps[LUMP_SHADERS].filelen / sizeof(dshader_t));
            const auto *surfaces = reinterpret_cast<const dsurface_t *>(base + lumps[LUMP_SURFACES].fileofs);
            const int surfaceCount = static_cast<int>(lumps[LUMP_SURFACES].filelen / sizeof(dsurface_t));
            const auto *verts = reinterpret_cast<const drawVert_t *>(base + lumps[LUMP_DRAWVERTS].fileofs);
            const int vertexCount = static_cast<int>(lumps[LUMP_DRAWVERTS].filelen / sizeof(drawVert_t));
            const auto *indices = reinterpret_cast<const int *>(base + lumps[LUMP_DRAWINDEXES].fileofs);
            const int indexCount = static_cast<int>(lumps[LUMP_DRAWINDEXES].filelen / sizeof(int));
            const auto *models = reinterpret_cast<const dmodel_t *>(base + lumps[LUMP_MODELS].fileofs);
            const int modelCount = static_cast<int>(lumps[LUMP_MODELS].filelen / sizeof(dmodel_t));

            LoadMapLights(reinterpret_cast<const char *>(base + lumps[LUMP_ENTITIES].fileofs),
                lumps[LUMP_ENTITIES].filelen);

            ResetWorldClusters();
            if (lumpValid(LUMP_PLANES, sizeof(dplane_t)) && lumpValid(LUMP_NODES, sizeof(dnode_t)) &&
                lumpValid(LUMP_LEAFS, sizeof(dleaf_t))) {
                LoadBspClusterTree(
                    reinterpret_cast<const dplane_t *>(base + lumps[LUMP_PLANES].fileofs),
                    static_cast<int>(lumps[LUMP_PLANES].filelen / sizeof(dplane_t)),
                    reinterpret_cast<const dnode_t *>(base + lumps[LUMP_NODES].fileofs),
                    static_cast<int>(lumps[LUMP_NODES].filelen / sizeof(dnode_t)),
                    reinterpret_cast<const dleaf_t *>(base + lumps[LUMP_LEAFS].fileofs),
                    static_cast<int>(lumps[LUMP_LEAFS].filelen / sizeof(dleaf_t)));
            } else {
                HostLog("LoadBspGeometry: cluster lumps missing, map lights fall back to cluster 0");
            }

            shaderMaterials.assign(static_cast<size_t>(shaderCount), QR_NO_MATERIAL);
            shaderMaterialAttempted.assign(static_cast<size_t>(shaderCount), 0);
            worldSurfaceInfos.assign(static_cast<size_t>(surfaceCount), WorldSurfaceInfo{});
            worldShaderNames.resize(static_cast<size_t>(shaderCount));
            for (int shaderIndex = 0; shaderIndex < shaderCount; ++shaderIndex) {
                worldShaderNames[static_cast<size_t>(shaderIndex)] = shaders[shaderIndex].shader;
            }

            int firstSurface = 0;
            int numSurfaces = surfaceCount;
            if (modelCount > 0) {
                firstSurface = models[0].firstSurface;
                numSurfaces = models[0].numSurfaces;
            }

            if (firstSurface < 0 || numSurfaces < 0 || firstSurface + numSurfaces > surfaceCount) {
                firstSurface = 0;
                numSurfaces = surfaceCount;
            }

            const QrResult beginResult = qrBeginStaticGeometries(qrayInstance);
            HostLog("LoadBspGeometry: qrBeginStaticGeometries result=%d", static_cast<int>(beginResult));
            if (beginResult != QR_SUCCESS) {
                ri.Printf(PRINT_WARNING, "rendererqray: qrBeginStaticGeometries failed: %d\n", static_cast<int>(beginResult));
            } else {
                staticGuard.open = true;
                int uploaded = 0;
                int planarWindingKept = 0;
                std::vector<GeometryBatch> batches(static_cast<size_t>(shaderCount));
                for (int surfaceIndex = firstSurface; surfaceIndex < firstSurface + numSurfaces; ++surfaceIndex) {
                    const dsurface_t &surface = surfaces[surfaceIndex];
                    if (surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_TRIANGLE_SOUP &&
                        surface.surfaceType != MST_PATCH) {
                        continue;
                    }
                    if (surface.surfaceType == MST_PATCH) {
                        // Q3 patches have odd dimensions (3, 5, 7, ...) up to MAX_PATCH_SIZE;
                        // the 64-bit product cannot wrap for hostile values.
                        constexpr int kMaxPatchSize = 65;
                        if (surface.patchWidth < 3 || surface.patchHeight < 3 ||
                            surface.patchWidth > kMaxPatchSize || surface.patchHeight > kMaxPatchSize ||
                            (surface.patchWidth & 1) == 0 || (surface.patchHeight & 1) == 0 ||
                            static_cast<int64_t>(surface.patchWidth) * surface.patchHeight > surface.numVerts) {
                            continue;
                        }
                    } else if (surface.numVerts < 3 || surface.numIndexes < 3) {
                        continue;
                    }
                    if (surface.firstVert < 0 || surface.numVerts > vertexCount - surface.firstVert) {
                        continue;
                    }
                    if (surface.surfaceType != MST_PATCH &&
                        (surface.firstIndex < 0 || surface.numIndexes > indexCount - surface.firstIndex)) {
                        continue;
                    }
                    if (surface.shaderNum < 0 || surface.shaderNum >= shaderCount) {
                        continue;
                    }

                    const dshader_t &shader = shaders[surface.shaderNum];
                    if ((shader.surfaceFlags & SURF_SKY) != 0) {
                        // The sky itself comes from skyparms, not from this geometry.
                        if (mapSkyShaderName.empty()) {
                            mapSkyShaderName = shader.shader;
                        }
                        MarkSkyCluster(ClusterIndexForPoint(verts[surface.firstVert].xyz));
                        continue;
                    }
                    if ((shader.surfaceFlags & SURF_NODRAW) != 0) {
                        continue;
                    }

                    WorldSurfaceInfo &surfaceInfo = worldSurfaceInfos[static_cast<size_t>(surfaceIndex)];
                    surfaceInfo.valid = true;
                    surfaceInfo.shaderNum = surface.shaderNum;
                    surfaceInfo.surfaceType = surface.surfaceType;
                    surfaceInfo.surfaceFlags = shader.surfaceFlags;
                    surfaceInfo.contentFlags = shader.contentFlags;
                    surfaceInfo.firstVert = surface.firstVert;
                    surfaceInfo.numVerts = surface.numVerts;
                    surfaceInfo.firstIndex = surface.firstIndex;
                    surfaceInfo.numIndexes = surface.numIndexes;
                    surfaceInfo.lightmapNum = surface.lightmapNum;
                    surfaceInfo.lightmapWidth = surface.lightmapWidth;
                    surfaceInfo.lightmapHeight = surface.lightmapHeight;
                    surfaceInfo.patchWidth = surface.patchWidth;
                    surfaceInfo.patchHeight = surface.patchHeight;

                    const QrMaterial material = CreateMaterialForShader(surface.shaderNum, shader.shader);

                    if (surface.surfaceType == MST_PATCH) {
                        constexpr int level = 8;
                        const int columns = (surface.patchWidth - 1) * level + 1;
                        const int rows = (surface.patchHeight - 1) * level + 1;
                        std::vector<QrVertex> patchVerts(static_cast<size_t>(columns) * rows);
                        for (int y = 0; y < rows; ++y) {
                            const float v = static_cast<float>(y) / static_cast<float>(rows - 1);
                            for (int x = 0; x < columns; ++x) {
                                const float u = static_cast<float>(x) / static_cast<float>(columns - 1);
                                float position[3] = {};
                                float texCoord[2] = {};
                                EvaluatePatch(verts + surface.firstVert, surface.patchWidth, surface.patchHeight,
                                    u, v, position, texCoord);
                                QrVertex &dst = patchVerts[static_cast<size_t>(y) * columns + x];
                                dst = {};
                                dst.position[0] = position[0];
                                dst.position[1] = position[1];
                                dst.position[2] = position[2];
                                dst.normal[0] = 0.0f;
                                dst.normal[1] = 0.0f;
                                dst.normal[2] = 1.0f;
                                dst.texCoord[0] = texCoord[0];
                                dst.texCoord[1] = texCoord[1];
                                dst.packedColor = 0xFFFFFFFFu;
                                dst.cluster = ClusterIndexForPoint(dst.position);
                                AccumulateClusterBounds(dst.cluster, dst.position);
                                for (int axis = 0; axis < 3; ++axis) {
                                    worldMin[axis] = std::min(worldMin[axis], dst.position[axis]);
                                    worldMax[axis] = std::max(worldMax[axis], dst.position[axis]);
                                }
                            }
                        }

                        std::vector<uint32_t> patchIndices;
                        patchIndices.reserve(static_cast<size_t>((rows - 1) * (columns - 1) * 6));
                        for (int y = 0; y < rows - 1; ++y) {
                            for (int x = 0; x < columns - 1; ++x) {
                                const uint32_t i0 = static_cast<uint32_t>(y * columns + x);
                                const uint32_t i1 = i0 + 1;
                                const uint32_t i2 = i0 + static_cast<uint32_t>(columns);
                                const uint32_t i3 = i2 + 1;
                                // Q3 tessellates grids as (i0,i2,i1)/(i1,i2,i3); the ray tracer needs
                                // the same handedness as the swapped planar and triangle soup surfaces.
                                patchIndices.push_back(i0);
                                patchIndices.push_back(i1);
                                patchIndices.push_back(i2);
                                patchIndices.push_back(i1);
                                patchIndices.push_back(i3);
                                patchIndices.push_back(i2);
                            }
                        }

                        if (patchIndices.size() >= 3) {
                            const QrVertex &p0 = patchVerts[patchIndices[0]];
                            const QrVertex &p1 = patchVerts[patchIndices[1]];
                            const QrVertex &p2 = patchVerts[patchIndices[2]];
                            const float ax = p1.position[0] - p0.position[0];
                            const float ay = p1.position[1] - p0.position[1];
                            const float az = p1.position[2] - p0.position[2];
                            const float bx = p2.position[0] - p0.position[0];
                            const float by = p2.position[1] - p0.position[1];
                            const float bz = p2.position[2] - p0.position[2];
                            float patchNormal[3] = {
                                ay * bz - az * by,
                                az * bx - ax * bz,
                                ax * by - ay * bx,
                            };
                            NormalizeVector(patchNormal);
                            for (int axis = 0; axis < 3; ++axis) {
                                surfaceInfo.generatedNormal[axis] = patchNormal[axis];
                                surfaceInfo.originalWindingNormal[axis] = patchNormal[axis];
                                surfaceInfo.uploadedWindingNormal[axis] = patchNormal[axis];
                            }
                        }
                        float authoredNormal[3] = {0.0f, 0.0f, 0.0f};
                        for (int vertexIndex = 0; vertexIndex < surface.numVerts; ++vertexIndex) {
                            const drawVert_t &source = verts[surface.firstVert + vertexIndex];
                            for (int axis = 0; axis < 3; ++axis) {
                                authoredNormal[axis] += source.normal[axis];
                            }
                        }
                        NormalizeVector(authoredNormal);
                        std::memcpy(surfaceInfo.authoredNormal, authoredNormal, sizeof(authoredNormal));

                        GeometryBatch &batch = batches[static_cast<size_t>(surface.shaderNum)];
                        batch.material = material;
                        const uint32_t baseVertex = static_cast<uint32_t>(batch.vertices.size());
                        batch.vertices.insert(batch.vertices.end(), patchVerts.begin(), patchVerts.end());
                        batch.sourceSurfaceIndices.insert(batch.sourceSurfaceIndices.end(), patchVerts.size(),
                            surfaceIndex);
                        batch.indices.reserve(batch.indices.size() + patchIndices.size());
                        for (uint32_t index : patchIndices) {
                            batch.indices.push_back(baseVertex + index);
                        }
                        ++uploaded;
                        continue;
                    }

                    float faceNormal[3] = {0.0f, 0.0f, 1.0f};
                    if (surface.surfaceType == MST_PLANAR) {
                        faceNormal[0] = surface.lightmapVecs[2][0];
                        faceNormal[1] = surface.lightmapVecs[2][1];
                        faceNormal[2] = surface.lightmapVecs[2][2];
                    } else {
                        const int i0 = indices[surface.firstIndex + 0];
                        const int i1 = indices[surface.firstIndex + 1];
                        const int i2 = indices[surface.firstIndex + 2];
                        if (i0 >= 0 && i0 < surface.numVerts && i1 >= 0 && i1 < surface.numVerts && i2 >= 0 && i2 < surface.numVerts) {
                            const drawVert_t &v0 = verts[surface.firstVert + i0];
                            const drawVert_t &v1 = verts[surface.firstVert + i1];
                            const drawVert_t &v2 = verts[surface.firstVert + i2];
                            const float ax = v1.xyz[0] - v0.xyz[0];
                            const float ay = v1.xyz[1] - v0.xyz[1];
                            const float az = v1.xyz[2] - v0.xyz[2];
                            const float bx = v2.xyz[0] - v0.xyz[0];
                            const float by = v2.xyz[1] - v0.xyz[1];
                            const float bz = v2.xyz[2] - v0.xyz[2];
                            faceNormal[0] = ay * bz - az * by;
                            faceNormal[1] = az * bx - ax * bz;
                            faceNormal[2] = ax * by - ay * bx;
                        }
                    }

                    const float normalLength = std::sqrt(faceNormal[0] * faceNormal[0] + faceNormal[1] * faceNormal[1] + faceNormal[2] * faceNormal[2]);
                    if (normalLength > 1e-6f) {
                        faceNormal[0] /= normalLength;
                        faceNormal[1] /= normalLength;
                        faceNormal[2] /= normalLength;
                    } else {
                        faceNormal[0] = 0.0f;
                        faceNormal[1] = 0.0f;
                        faceNormal[2] = 1.0f;
                    }

                    std::memcpy(surfaceInfo.generatedNormal, faceNormal, sizeof(faceNormal));
                    if (surface.surfaceType == MST_PLANAR) {
                        std::memcpy(surfaceInfo.planeNormal, faceNormal, sizeof(faceNormal));
                        surfaceInfo.planeDist = faceNormal[0] * verts[surface.firstVert].xyz[0] +
                            faceNormal[1] * verts[surface.firstVert].xyz[1] +
                            faceNormal[2] * verts[surface.firstVert].xyz[2];
                    }
                    float authoredNormal[3] = {0.0f, 0.0f, 0.0f};
                    for (int vertexIndex = 0; vertexIndex < surface.numVerts; ++vertexIndex) {
                        const drawVert_t &source = verts[surface.firstVert + vertexIndex];
                        for (int axis = 0; axis < 3; ++axis) {
                            authoredNormal[axis] += source.normal[axis];
                        }
                    }
                    NormalizeVector(authoredNormal);
                    std::memcpy(surfaceInfo.authoredNormal, authoredNormal, sizeof(authoredNormal));

                    std::vector<QrVertex> outVerts(static_cast<size_t>(surface.numVerts));
                    for (int i = 0; i < surface.numVerts; ++i) {
                        const drawVert_t &src = verts[surface.firstVert + i];
                        QrVertex &dst = outVerts[static_cast<size_t>(i)];
                        dst = {};
                        dst.position[0] = src.xyz[0];
                        dst.position[1] = src.xyz[1];
                        dst.position[2] = src.xyz[2];
                        dst.normal[0] = faceNormal[0];
                        dst.normal[1] = faceNormal[1];
                        dst.normal[2] = faceNormal[2];
                        dst.texCoord[0] = src.st[0];
                        dst.texCoord[1] = src.st[1];
                        dst.packedColor = 0xFFFFFFFFu;
                        dst.cluster = ClusterIndexForPoint(dst.position);
                        AccumulateClusterBounds(dst.cluster, dst.position);
                        for (int axis = 0; axis < 3; ++axis) {
                            worldMin[axis] = std::min(worldMin[axis], dst.position[axis]);
                            worldMax[axis] = std::max(worldMax[axis], dst.position[axis]);
                        }
                    }

                    std::vector<uint32_t> outIndices(static_cast<size_t>(surface.numIndexes));
                    for (int i = 0; i < surface.numIndexes; ++i) {
                        const int index = indices[surface.firstIndex + i];
                        outIndices[static_cast<size_t>(i)] = (index >= 0 && index < surface.numVerts) ? static_cast<uint32_t>(index) : 0u;
                    }
                    if (outIndices.size() >= 3) {
                        const drawVert_t &p0 = verts[surface.firstVert + outIndices[0]];
                        const drawVert_t &p1 = verts[surface.firstVert + outIndices[1]];
                        const drawVert_t &p2 = verts[surface.firstVert + outIndices[2]];
                        const float ax = p1.xyz[0] - p0.xyz[0];
                        const float ay = p1.xyz[1] - p0.xyz[1];
                        const float az = p1.xyz[2] - p0.xyz[2];
                        const float bx = p2.xyz[0] - p0.xyz[0];
                        const float by = p2.xyz[1] - p0.xyz[1];
                        const float bz = p2.xyz[2] - p0.xyz[2];
                        float originalWindingNormal[3] = {
                            ay * bz - az * by,
                            az * bx - ax * bz,
                            ax * by - ay * bx,
                        };
                        NormalizeVector(originalWindingNormal);
                        std::memcpy(surfaceInfo.originalWindingNormal, originalWindingNormal,
                            sizeof(originalWindingNormal));
                        std::memcpy(surfaceInfo.uploadedWindingNormal, originalWindingNormal,
                            sizeof(originalWindingNormal));
                    }
                    // Q3 treats clockwise as front facing, which is the opposite handedness of
                    // QuakeRay's ray traced shadows, so every triangle is flipped once here.
                    bool flipWinding = normalMode < 2 || normalMode >= 5;
                    bool keptSurfaceWinding = false;
                    if (flipWinding && normalMode < 5 && surface.surfaceType == MST_PLANAR && surface.numIndexes >= 3) {
                        const int w0 = indices[surface.firstIndex + 0];
                        const int w1 = indices[surface.firstIndex + 1];
                        const int w2 = indices[surface.firstIndex + 2];
                        if (w0 >= 0 && w1 >= 0 && w2 >= 0 && w0 < surface.numVerts &&
                            w1 < surface.numVerts && w2 < surface.numVerts) {
                            const drawVert_t &p0 = verts[surface.firstVert + w0];
                            const drawVert_t &p1 = verts[surface.firstVert + w1];
                            const drawVert_t &p2 = verts[surface.firstVert + w2];
                            const float e1[3] = {p1.xyz[0] - p0.xyz[0], p1.xyz[1] - p0.xyz[1], p1.xyz[2] - p0.xyz[2]};
                            const float e2[3] = {p2.xyz[0] - p0.xyz[0], p2.xyz[1] - p0.xyz[1], p2.xyz[2] - p0.xyz[2]};
                            const float windingNormal[3] = {
                                e1[1] * e2[2] - e1[2] * e2[1],
                                e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0],
                            };
                            const float alignment = windingNormal[0] * faceNormal[0] +
                                windingNormal[1] * faceNormal[1] + windingNormal[2] * faceNormal[2];
                            // A few compiled planar surfaces already wind towards their own plane
                            // normal; flipping those inverts their lighting, so they are left alone.
                            if (alignment > 0.0f) {
                                flipWinding = false;
                                keptSurfaceWinding = true;
                                ++planarWindingKept;
                            }
                        }
                    }
                    if (flipWinding) {
                        for (size_t i = 0; i + 2 < outIndices.size(); i += 3) {
                            std::swap(outIndices[i + 1], outIndices[i + 2]);
                        }
                        for (int axis = 0; axis < 3; ++axis) {
                            surfaceInfo.uploadedWindingNormal[axis] =
                                -surfaceInfo.uploadedWindingNormal[axis];
                        }
                        // Planar normals come from the BSP plane and stay valid when the winding
                        // is reversed; curve and triangle-soup normals are derived from that very
                        // winding, so they have to follow the flip or the surface turns inside out.
                        if (surface.surfaceType != MST_PLANAR) {
                            for (QrVertex &vertex : outVerts) {
                                vertex.normal[0] = -vertex.normal[0];
                                vertex.normal[1] = -vertex.normal[1];
                                vertex.normal[2] = -vertex.normal[2];
                            }
                        }
                    }

                    GeometryBatch &batch = batches[static_cast<size_t>(surface.shaderNum)];
                    batch.material = material;
                    const bool surfaceAlphaTested = qrayAlphaTest != nullptr && qrayAlphaTest->integer != 0
                        && ShaderUsesAlphaTest(shader.shader);
                    surfaceInfo.alphaTested = surfaceAlphaTested;
                    surfaceInfo.windingFlipped = flipWinding;
                    surfaceInfo.planarWindingKept = keptSurfaceWinding;
                    if (surfaceAlphaTested && !batch.alphaTested) {
                        HostLog("LoadBspGeometry: alpha tested shader \"%s\"", shader.shader);
                    }
                    batch.alphaTested = surfaceAlphaTested;
                    const uint32_t baseVertex = static_cast<uint32_t>(batch.vertices.size());
                    batch.vertices.insert(batch.vertices.end(), outVerts.begin(), outVerts.end());
                    batch.sourceSurfaceIndices.insert(batch.sourceSurfaceIndices.end(), outVerts.size(),
                        surfaceIndex);
                    batch.indices.reserve(batch.indices.size() + outIndices.size());
                    for (uint32_t index : outIndices) {
                        batch.indices.push_back(baseVertex + index);
                    }
                    ++uploaded;
                }

                HostLog("LoadBspGeometry: upload loop done, uploaded=%d planarWindingKept=%d",
                    uploaded, planarWindingKept);
                int uploadedBatches = 0;
                int alphaTestedBatches = 0;
                int overlayBatches = 0;
                uint32_t normalFlags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
                if (normalMode == 1 || normalMode == 3) {
                    normalFlags = QR_GEOMETRY_UPLOAD_GENERATE_INVERTED_NORMALS_BIT;
                } else if (normalMode == 4 || normalMode == 6) {
                    normalFlags = QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT;
                }
                for (size_t batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
                    const GeometryBatch &batch = batches[batchIndex];
                    if (batch.vertices.empty() || batch.indices.empty()) {
                        continue;
                    }

                    const std::string &batchShaderName =
                        worldShaderNames[static_cast<size_t>(batchIndex)];
                    const auto shaderDefinitionIt =
                        shaderDefinitions.find(NormalizeShaderName(batchShaderName));
                    if (shaderDefinitionIt != shaderDefinitions.end() &&
                        shaderDefinitionIt->second.usesOverlay) {
                        WorldOverlayBatch overlay;
                        overlay.shaderName = batchShaderName;
                        overlay.vertices = batch.vertices;
                        overlay.indices = batch.indices;
                        worldOverlayBatches.push_back(std::move(overlay));
                        ++overlayBatches;
                        HostLog("LoadBspGeometry: overlay shader='%s' stages=%zu tracedBase=%d",
                            batchShaderName.c_str(), shaderDefinitionIt->second.stages.size(),
                            shaderDefinitionIt->second.tracedBase ? 1 : 0);
                        if (!shaderDefinitionIt->second.tracedBase) {
                            continue;
                        }
                    }

                    const std::vector<QrVertex> meterVertices = QuakeVerticesToMeters(batch.vertices);
                    QrGeometryUploadInfo upload{};
                    upload.uniqueID = static_cast<uint64_t>(batchIndex) + 1;
                    upload.flags = normalFlags;
                    upload.geomType = QR_GEOMETRY_TYPE_STATIC;
                    upload.passThroughType = batch.alphaTested
                        ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED
                        : QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
                    upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
                    upload.vertexCount = static_cast<uint32_t>(meterVertices.size());
                    upload.pVertices = meterVertices.data();
                    upload.indexCount = static_cast<uint32_t>(batch.indices.size());
                    upload.pIndices = batch.indices.data();
                    upload.layerColors[0].data[0] = 1.0f;
                    upload.layerColors[0].data[1] = 1.0f;
                    upload.layerColors[0].data[2] = 1.0f;
                    upload.layerColors[0].data[3] = 1.0f;
                    upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
                    upload.texCoordScale[0] = 1.0f;
                    upload.texCoordScale[1] = 1.0f;
                    upload.defaultRoughness = 0.8f;
                    upload.defaultMetallicity = 0.0f;
                    upload.defaultEmission = 0.0f;
                    upload.geomMaterial.layerMaterials[0] = batch.material;
                    upload.transform = IdentityTransform();

                    const QrResult uploadResult = qrUploadGeometry(qrayInstance, &upload);
                    if (uploadResult == QR_SUCCESS) {
                        ++uploadedBatches;
                        if (batch.alphaTested) {
                            ++alphaTestedBatches;
                        }
                    } else {
                        HostLog("LoadBspGeometry: batch %zu upload failed result=%d", batchIndex, static_cast<int>(uploadResult));
                    }
                }
                staticGuard.open = false;
                const QrResult submitResult = qrSubmitStaticGeometries(qrayInstance);
                HostLog("LoadBspGeometry: qrSubmitStaticGeometries result=%d alphaTestedBatches=%d "
                    "overlayBatches=%d overlayStages=%zu",
                    static_cast<int>(submitResult), alphaTestedBatches, overlayBatches,
                    [&]() {
                        size_t count = 0;
                        for (const WorldOverlayBatch &overlay : worldOverlayBatches) {
                            const auto it = shaderDefinitions.find(
                                NormalizeShaderName(overlay.shaderName));
                            if (it != shaderDefinitions.end()) {
                                count += it->second.stages.size();
                            }
                        }
                        return count;
                    }());
                HostLog("LoadBspGeometry: bounds min=(%f %f %f) max=(%f %f %f)", worldMin[0], worldMin[1], worldMin[2], worldMax[0], worldMax[1], worldMax[2]);
                if (submitResult != QR_SUCCESS) {
                    ri.Printf(PRINT_WARNING, "rendererqray: qrSubmitStaticGeometries failed: %d\n", static_cast<int>(submitResult));
                }
                ri.Printf(PRINT_ALL, "rendererqray: uploaded %d world surfaces from %s\n", uploaded, name);

                const uint8_t *visibility = nullptr;
                uint32_t visibilitySize = 0;
                if (lumpValid(LUMP_VISIBILITY, 1) && lumps[LUMP_VISIBILITY].filelen > 0) {
                    visibility = base + lumps[LUMP_VISIBILITY].fileofs;
                    visibilitySize = static_cast<uint32_t>(lumps[LUMP_VISIBILITY].filelen);
                }
                FinalizeWorldClusters(worldMin, worldMax, visibility, visibilitySize);
                ResolveMapLightClusters();
                UploadWorldClusters();

                loaded = uploaded > 0;
                BuildMarkAcceleration(batches);
                BuildSkyEnvironment();
            }
        } else {
            HostLog("LoadBspGeometry: malformed BSP lumps in %s", name);
            ri.Printf(PRINT_WARNING, "rendererqray: malformed BSP lumps in %s\n", name);
        }
    } else {
        HostLog("LoadBspGeometry: %s is not a Quake 3 BSP", name);
        ri.Printf(PRINT_WARNING, "rendererqray: %s is not a Quake 3 BSP\n", name);
    }

    return loaded;
}

bool LoadBspGeometry(const char *name)
{
#ifdef _WIN32
    __try {
        return LoadBspGeometryInner(name);
    } __except (HostExceptionFilter(GetExceptionInformation())) {
        return false;
    }
#else
    try {
        return LoadBspGeometryInner(name);
    } catch (const std::exception &exception) {
        HostLog("LoadBspGeometry: exception: %s", exception.what());
        ri.Printf(PRINT_WARNING, "rendererqray: failed to load %s: %s\n", name, exception.what());
        return false;
    }
#endif
}

void ResetReadback()
{
    readbackBuffer.clear();
    readbackReadyPixels.clear();
    readbackWidth = 0;
    readbackHeight = 0;
    readbackRowPitch = 0;
    readbackBufferSize = 0;
    readbackRequest = 0;
    readbackPending = false;
    readbackValid = false;
}

void UpdateReadback(bool allowRequest)
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    if (readbackWidth == 0) {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        uint64_t bufferSize = 0;
        const QrResult result = qrGetFrameReadbackRequirements(
            qrayInstance, static_cast<QrFrameReadbackFormat>(kReadbackFormat), QR_TRUE,
            &width, &height, &rowPitch, &bufferSize);
        if (result == QR_SUCCESS && width > 0 && height > 0 && bufferSize > 0) {
            readbackWidth = width;
            readbackHeight = height;
            readbackRowPitch = rowPitch;
            readbackBufferSize = bufferSize;
            readbackBuffer.assign(static_cast<size_t>(bufferSize), 0);
            ri.Printf(PRINT_ALL, "rendererqray: readback %ux%u pitch %u size %llu\n",
                width, height, rowPitch, static_cast<unsigned long long>(bufferSize));
        }
        return;
    }

    if (allowRequest && !readbackPending && !readbackBuffer.empty()) {
        QrFrameReadbackInfo info{};
        info.width = readbackWidth;
        info.height = readbackHeight;
        info.rowPitch = readbackRowPitch;
        info.bufferSize = readbackBufferSize;
        info.format = static_cast<QrFrameReadbackFormat>(kReadbackFormat);
        info.includeUi = QR_TRUE;
        info.pDst = readbackBuffer.data();
        info.frameId = QR_FRAME_ID_LATEST;

        const QrResult result = qrRequestFrameReadback(qrayInstance, &info, &readbackRequest);
        if (result == QR_SUCCESS) {
            readbackPending = true;
        } else if (result == QR_READBACK_BUSY) {
            readbackPending = true;
        } else if (result == QR_FRAME_NOT_READY) {
            return;
        } else if (result == QR_READBACK_FAILED || result == QR_READBACK_INVALID) {
            ResetReadback();
        }
    }

    if (readbackPending) {
        QrBool32 done = QR_FALSE;
        const QrResult result = qrGetFrameReadbackStatus(qrayInstance, readbackRequest, &done);
        if (result == QR_SUCCESS && done == QR_TRUE) {
            readbackReadyPixels.swap(readbackBuffer);
            readbackBuffer.resize(static_cast<size_t>(readbackBufferSize));
            readbackValid = true;
            readbackPending = false;
        } else if (result == QR_READBACK_FAILED || result == QR_READBACK_INVALID) {
            ResetReadback();
        }
    }
}

void CopyReadbackToCapture()
{
    if (!capturePending) {
        return;
    }
    // TakeVideoFrame hands over client-owned buffers that are only valid for this
    // call sequence (CL_CloseAVI frees them), so a frame is never deferred.
    capturePending = false;
    if (captureBuffer == nullptr || captureWidth <= 0 || captureHeight <= 0) {
        return;
    }

    // Produce exactly what RB_TakeVideoFrameCmd produces: captureWidth x captureHeight,
    // bottom-up RGB rows (glReadPixels order). Pixels the readback does not cover, or the
    // whole frame while no readback is ready yet, are black so the AVI keeps its timing.
    const int width = captureWidth;
    const int height = captureHeight;
    const size_t linePitch = static_cast<size_t>(width) * 3;
    const bool haveReadback = readbackValid && readbackWidth > 0 && readbackHeight > 0 &&
        readbackReadyPixels.size() >= static_cast<size_t>(readbackRowPitch) * readbackHeight;
    const int copyWidth = haveReadback ? std::min<int>(width, static_cast<int>(readbackWidth)) : 0;
    const int copyHeight = haveReadback ? std::min<int>(height, static_cast<int>(readbackHeight)) : 0;

    std::memset(captureBuffer, 0, linePitch * static_cast<size_t>(height));
    for (int y = 0; y < copyHeight; ++y) {
        const uint8_t *srcRow = readbackReadyPixels.data() + static_cast<size_t>(y) * readbackRowPitch;
        byte *dstRow = captureBuffer + static_cast<size_t>(height - 1 - y) * linePitch;
        for (int x = 0; x < copyWidth; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
    }

    if (captureEncodeBuffer == nullptr) {
        return;
    }
    if (captureMotionJpeg) {
        const size_t encodedSize = ri.CL_SaveJPGToBuffer(captureEncodeBuffer, linePitch * static_cast<size_t>(height), 90,
            width, height, captureBuffer, 0);
        if (encodedSize > 0) {
            ri.CL_WriteAVIVideoFrame(captureEncodeBuffer, static_cast<int>(encodedSize));
        }
        return;
    }

    // Uncompressed AVI: BGR with rows padded to AVI_LINE_PADDING (4 bytes).
    const size_t aviPitch = (linePitch + 3u) & ~static_cast<size_t>(3u);
    for (int y = 0; y < height; ++y) {
        const byte *srcRow = captureBuffer + static_cast<size_t>(y) * linePitch;
        byte *dstRow = captureEncodeBuffer + static_cast<size_t>(y) * aviPitch;
        for (int x = 0; x < width; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 3 + 2];
            dstRow[x * 3 + 1] = srcRow[x * 3 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 3 + 0];
        }
        std::memset(dstRow + linePitch, 0, aviPitch - linePitch);
    }
    ri.CL_WriteAVIVideoFrame(captureEncodeBuffer, static_cast<int>(aviPitch * static_cast<size_t>(height)));
}

void StartFrame()
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    QrStartFrameInfo info{};
    const bool uncapped = qrayUncapped != nullptr && qrayUncapped->integer != 0;
    info.presentMode = uncapped ? QR_PRESENT_MODE_IMMEDIATE : QR_PRESENT_MODE_VSYNC;
    info.maxFrameLatency = uncapped ? 0u : 1u;
    info.requestShaderReload = QR_FALSE;
    info.wantFullscreenExclusive = QR_FALSE;
    info.hasFocus = QR_TRUE;

    QrResult result = qrStartFrame(qrayInstance, &info);
    if (result == QR_FEATURE_UNAVAILABLE && info.presentMode != QR_PRESENT_MODE_VSYNC) {
        // SDK 0.2+ no longer substitutes a present mode the surface lacks (IMMEDIATE is
        // missing on several Linux/X11 drivers); fall back to FIFO instead of a black screen.
        static bool fallbackReported = false;
        if (!fallbackReported) {
            ri.Printf(PRINT_WARNING, "rendererqray: present mode %d unavailable, using vsync\n",
                static_cast<int>(info.presentMode));
            fallbackReported = true;
        }
        info.presentMode = QR_PRESENT_MODE_VSYNC;
        info.maxFrameLatency = 1u;
        result = qrStartFrame(qrayInstance, &info);
    }
    static QrResult lastStartFailure = QR_SUCCESS;
    if (result != QR_SUCCESS) {
        // Minimised or suspended swapchains fail every frame; report each new code once.
        if (result != lastStartFailure) {
            ri.Printf(PRINT_WARNING, "rendererqray: qrStartFrame failed: %d\n", static_cast<int>(result));
            lastStartFailure = result;
        }
        return;
    }
    lastStartFailure = QR_SUCCESS;

    frameStarted = true;
    sceneRendered = false;
    sceneValid = false;
    frameHasWorldScene = false;
    dynamicLights.clear();

    if (!worldLoaded && !pendingWorldName.empty()) {
        LoadBspGeometry(pendingWorldName.c_str());
        worldLoaded = true;
    }

    UpdateReadback(readbackWantedFrames > 0);
    if (readbackWantedFrames > 0) {
        --readbackWantedFrames;
    }
}

uint64_t NextDynamicGeometryId()
{
    return dynamicGeometryId++;
}

void UploadDynamicGeometry(const std::vector<QrVertex> &vertices, const std::vector<uint32_t> &indices,
    QrMaterial material, QrGeometryPrimaryVisibilityType visibility,
    QrGeometryPassThroughType passThrough, QrGeometryMaterialBlendType blend,
    float emission, QrGeometryUploadFlags extraFlags = 0, float layerAlpha = 1.0f)
{
    if (qrayInstance == QR_NULL_HANDLE || vertices.empty() || indices.empty()) {
        return;
    }
    if (material == QR_NO_MATERIAL) {
        material = defaultMaterial;
    }

    const std::vector<QrVertex> meterVertices = QuakeVerticesToMeters(vertices);
    QrGeometryUploadInfo upload{};
    upload.uniqueID = NextDynamicGeometryId();
    upload.flags = QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT | extraFlags;
    upload.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    upload.passThroughType = passThrough;
    upload.visibilityType = visibility;
    upload.vertexCount = static_cast<uint32_t>(meterVertices.size());
    upload.pVertices = meterVertices.data();
    upload.indexCount = static_cast<uint32_t>(indices.size());
    upload.pIndices = indices.data();
    upload.layerColors[0].data[0] = 1.0f;
    upload.layerColors[0].data[1] = 1.0f;
    upload.layerColors[0].data[2] = 1.0f;
    upload.layerColors[0].data[3] = std::clamp(layerAlpha, 0.0f, 1.0f);
    upload.layerBlendingTypes[0] = blend;
    upload.texCoordScale[0] = 1.0f;
    upload.texCoordScale[1] = 1.0f;
    upload.defaultRoughness = 0.8f;
    upload.defaultMetallicity = 0.0f;
    upload.defaultEmission = emission;
    upload.geomMaterial.layerMaterials[0] = material;
    upload.transform = IdentityTransform();

    const QrResult result = qrUploadGeometry(qrayInstance, &upload);
    if (result != QR_SUCCESS) {
        HostLog("UploadDynamicGeometry: result=%d verts=%u indices=%u",
            static_cast<int>(result), upload.vertexCount, upload.indexCount);
    }
}

// Semi-transparent effects cannot be composited by the traced pass - QuakeRay's primary path only
// knows alpha cut-out - so sprites, beams and impact marks go through the rasterized overlay. The
// upload passes no view projection, which makes the overlay use the frame's own camera matrices,
// and keeps the GL blend equation of the shader so smoke, additive glows and darkening decals come
// out the way the original renderer draws them.
void UploadRasterizedWorldGeometry(const std::vector<QrVertex> &vertices,
    const std::vector<uint32_t> &indices, QrMaterial material, QrBlendFactor blendSource,
    QrBlendFactor blendDestination, uint32_t pipelineStateOverride = 0xFFFFFFFFu,
    const float texCoordOffset[2] = nullptr, const float color[4] = nullptr)
{
    if (qrayInstance == QR_NULL_HANDLE || vertices.empty() || !frameStarted) {
        return;
    }
    if (material == QR_NO_MATERIAL) {
        material = defaultMaterial;
    }

    const std::vector<QrVertex> meterVertices = QuakeVerticesToMeters(vertices);
    QrRasterizedGeometryUploadInfo info{};
    info.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT;
    info.vertexCount = static_cast<uint32_t>(meterVertices.size());
    info.pVertices = meterVertices.data();
    info.indexCount = static_cast<uint32_t>(indices.size());
    info.pIndices = indices.empty() ? nullptr : indices.data();
    info.transform = IdentityTransform();
    info.color.data[0] = color != nullptr ? color[0] : 1.0f;
    info.color.data[1] = color != nullptr ? color[1] : 1.0f;
    info.color.data[2] = color != nullptr ? color[2] : 1.0f;
    info.color.data[3] = color != nullptr ? color[3] : 1.0f;
    info.texCoordScale[0] = 1.0f;
    info.texCoordScale[1] = 1.0f;
    info.texCoordOffset[0] = texCoordOffset != nullptr ? texCoordOffset[0] : 0.0f;
    info.texCoordOffset[1] = texCoordOffset != nullptr ? texCoordOffset[1] : 0.0f;
    info.material = material;
    info.pipelineState = pipelineStateOverride != 0xFFFFFFFFu
        ? pipelineStateOverride
        : (QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE |
           QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST);
    info.blendFuncSrc = blendSource;
    info.blendFuncDst = blendDestination;

    const QrResult result = qrUploadRasterizedGeometry(qrayInstance, &info, nullptr, nullptr);
    if (result != QR_SUCCESS) {
        HostLog("UploadRasterizedWorldGeometry: result=%d verts=%u indices=%u blend=%d/%d",
            static_cast<int>(result), info.vertexCount, info.indexCount,
            static_cast<int>(blendSource), static_cast<int>(blendDestination));
    }
}

std::string ActiveStageTexture(const ShaderStage &stage, float timeSeconds)
{
    if (stage.animationMaps.empty() || stage.animationFps <= 0.0f) {
        return stage.map;
    }
    const size_t frame = static_cast<size_t>(
        std::floor(std::max(timeSeconds, 0.0f) * stage.animationFps)) %
        stage.animationMaps.size();
    return stage.animationMaps[frame];
}

void UploadWorldOverlayGeometry()
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted || worldOverlayBatches.empty()) {
        return;
    }

    const float timeSeconds = static_cast<float>(sceneDef.time) * 0.001f;
    for (const WorldOverlayBatch &batch : worldOverlayBatches) {
        const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(batch.shaderName));
        if (definitionIt == shaderDefinitions.end()) {
            continue;
        }

        const ShaderDefinition &definition = definitionIt->second;
        const size_t firstOverlayStage = definition.tracedBase &&
            definition.baseStageIndex < definition.stages.size()
            ? definition.baseStageIndex + 1u
            : 0u;
        for (size_t stageIndex = firstOverlayStage;
             stageIndex < definition.stages.size(); ++stageIndex) {
            const ShaderStage &stage = definition.stages[stageIndex];
            const std::string textureName = ActiveStageTexture(stage, timeSeconds);
            if (textureName.empty() || _stricmp(textureName.c_str(), "$lightmap") == 0) {
                continue;
            }

            const QrMaterial material = GetMaterialForTextureName(textureName.c_str());
            if (material == QR_NO_MATERIAL) {
                continue;
            }

            QrBlendFactor source = stage.blendSrc;
            QrBlendFactor destination = stage.blendDst;
            uint32_t state = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST;
            if (stage.blended || stage.blend != QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE) {
                state |= QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
            } else {
                state |= QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE;
                source = QR_BLEND_FACTOR_ONE;
                destination = QR_BLEND_FACTOR_ZERO;
            }
            if (stage.alphaTested) {
                state |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
            }

            const float rgb = EvaluateShaderWave(stage.rgbWave, stage.rgbWaveBase,
                stage.rgbWaveAmplitude, stage.rgbWavePhase, stage.rgbWaveFrequency, timeSeconds);
            const float alpha = EvaluateShaderWave(stage.alphaWave, stage.alphaWaveBase,
                stage.alphaWaveAmplitude, stage.alphaWavePhase, stage.alphaWaveFrequency,
                timeSeconds);
            const float color[4] = {rgb, rgb, rgb, alpha};
            const float offset[2] = {
                stage.scroll[0] * timeSeconds,
                stage.scroll[1] * timeSeconds,
            };
            UploadRasterizedWorldGeometry(batch.vertices, batch.indices, material,
                source, destination, state, offset, color);
        }
    }

    static size_t loggedBatchCount = 0;
    if (loggedBatchCount != worldOverlayBatches.size()) {
        loggedBatchCount = worldOverlayBatches.size();
        size_t stageCount = 0;
        for (const WorldOverlayBatch &batch : worldOverlayBatches) {
            const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(batch.shaderName));
            if (definitionIt != shaderDefinitions.end()) {
                stageCount += definitionIt->second.stages.size();
            }
        }
        HostLog("UploadWorldOverlayGeometry: batches=%zu stages=%zu", worldOverlayBatches.size(),
            stageCount);
        for (const WorldOverlayBatch &batch : worldOverlayBatches) {
            const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(batch.shaderName));
            if (definitionIt == shaderDefinitions.end()) {
                continue;
            }
            for (const ShaderStage &stage : definitionIt->second.stages) {
                const std::string textureName = ActiveStageTexture(stage, timeSeconds);
                HostLog("OverlayStage: shader='%s' map='%s' blend=%d src=%d dst=%d "
                    "animated=%zu dynamic=%d",
                    batch.shaderName.c_str(), textureName.c_str(),
                    static_cast<int>(stage.blend), static_cast<int>(stage.blendSrc),
                    static_cast<int>(stage.blendDst), stage.animationMaps.size(),
                    stage.dynamic ? 1 : 0);
            }
        }
    }
}

// The sky is composited by the overlay pass, which runs on top of the traced frame, so the box
// has to pass the depth test of that frame: the quads sit at the far distance of the map and
// only background pixels survive. Both windings are emitted because the overlay does not cull.
void UploadSkyGeometry()
{
    if (qrayPhysicalSky != nullptr && qrayPhysicalSky->integer != 0) {
        return;
    }
    if (qraySkybox == nullptr || qraySkybox->integer == 0 || qrayInstance == QR_NULL_HANDLE ||
        skyAtlasMaterial == QR_NO_MATERIAL || skyAtlasFaceSize <= 0 || !frameStarted) {
        return;
    }

    const float radius = std::max(256.0f, CvarFloat(qraySkyboxRadius, 2000.0f));
    const float atlasWidth = static_cast<float>(skyAtlasFaceSize * 3);
    const float atlasHeight = static_cast<float>(skyAtlasFaceSize * 2);
    const float cellWidth = static_cast<float>(skyAtlasFaceSize) / atlasWidth;
    const float cellHeight = static_cast<float>(skyAtlasFaceSize) / atlasHeight;
    const float cornerST[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

    std::vector<QrVertex> vertices;
    vertices.reserve(24);
    std::vector<uint32_t> indices;
    indices.reserve(72);

    for (int face = 0; face < 6; ++face) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const float cellX = static_cast<float>(face % 3);
        const float cellY = static_cast<float>(face / 3);
        for (int corner = 0; corner < 4; ++corner) {
            float direction[3];
            SkyFaceDirection(face, cornerST[corner][0], cornerST[corner][1], direction);
            const float length = std::sqrt(direction[0] * direction[0] +
                direction[1] * direction[1] + direction[2] * direction[2]);
            const float scale = length > 0.0f ? 1.0f / length : 0.0f;
            direction[0] *= scale;
            direction[1] *= scale;
            direction[2] *= scale;

            QrVertex vertex{};
            vertex.position[0] = sceneDef.vieworg[0] + direction[0] * radius;
            vertex.position[1] = sceneDef.vieworg[1] + direction[1] * radius;
            vertex.position[2] = sceneDef.vieworg[2] + direction[2] * radius;
            vertex.normal[0] = -direction[0];
            vertex.normal[1] = -direction[1];
            vertex.normal[2] = -direction[2];
            vertex.texCoord[0] = (cellX + (cornerST[corner][0] * 0.5f + 0.5f)) * cellWidth;
            vertex.texCoord[1] = (cellY + (cornerST[corner][1] * 0.5f + 0.5f)) * cellHeight;
            vertex.packedColor = 0xFFFFFFFFu;
            vertices.push_back(vertex);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 1);
        indices.push_back(base + 0);
        indices.push_back(base + 3);
        indices.push_back(base + 2);
    }

    UploadRasterizedWorldGeometry(vertices, indices, skyAtlasMaterial, QR_BLEND_FACTOR_ONE,
        QR_BLEND_FACTOR_ZERO, QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST);
}

QrMaterial ResolveModelMaterial(const refEntity_t &entity, const ModelAsset &asset, const ModelSurface &surface)
{
    (void)asset;
    if (entity.customShader != 0) {
        return GetMaterialForHandle(entity.customShader);
    }
    if (entity.customSkin != 0) {
        const auto skinIt = skinAssets.find(entity.customSkin);
        if (skinIt != skinAssets.end()) {
            const auto shaderIt = skinIt->second.shaderBySurface.find(surface.shaderName);
            if (shaderIt != skinIt->second.shaderBySurface.end()) {
                return GetMaterialForTextureName(shaderIt->second.c_str());
            }
        }
    }
    return GetMaterialForTextureName(surface.shaderName.c_str());
}

const char *ResolveModelShaderName(const refEntity_t &entity, const ModelSurface &surface)
{
    if (entity.customSkin != 0) {
        const auto skinIt = skinAssets.find(entity.customSkin);
        if (skinIt != skinAssets.end()) {
            const auto shaderIt = skinIt->second.shaderBySurface.find(surface.shaderName);
            if (shaderIt != skinIt->second.shaderBySurface.end()) {
                return shaderIt->second.c_str();
            }
        }
    }
    return surface.shaderName.c_str();
}

// tcGen environment: GL_SPHERE_MAP of the reflection vector, evaluated per vertex the way the
// fixed function pipeline Q3 was authored for did it. Used for the additive glass shells, the
// medkit and armour spheres, which have no usable UVs of their own.
void ComputeModelEnvironmentTexCoord(const float position[3], const float normal[3], float out[2])
{
    float view[3] = {
        position[0] - sceneDef.vieworg[0],
        position[1] - sceneDef.vieworg[1],
        position[2] - sceneDef.vieworg[2]
    };
    const float viewLength = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (viewLength <= 1.0e-4f) {
        out[0] = 0.5f;
        out[1] = 0.5f;
        return;
    }
    view[0] /= viewLength;
    view[1] /= viewLength;
    view[2] /= viewLength;

    const float facing = view[0] * normal[0] + view[1] * normal[1] + view[2] * normal[2];
    const float reflected[3] = {
        view[0] - 2.0f * facing * normal[0],
        view[1] - 2.0f * facing * normal[1],
        view[2] - 2.0f * facing * normal[2]
    };

    // Eye space, +Z pointing back at the viewer.
    const float eyeX = reflected[0] * sceneDef.viewaxis[1][0] + reflected[1] * sceneDef.viewaxis[1][1] +
        reflected[2] * sceneDef.viewaxis[1][2];
    const float eyeY = reflected[0] * sceneDef.viewaxis[2][0] + reflected[1] * sceneDef.viewaxis[2][1] +
        reflected[2] * sceneDef.viewaxis[2][2];
    const float eyeZ = -(reflected[0] * sceneDef.viewaxis[0][0] + reflected[1] * sceneDef.viewaxis[0][1] +
        reflected[2] * sceneDef.viewaxis[0][2]);

    const float denominator = 2.0f * std::sqrt(eyeX * eyeX + eyeY * eyeY + (eyeZ + 1.0f) * (eyeZ + 1.0f));
    if (denominator <= 1.0e-4f) {
        out[0] = 0.5f;
        out[1] = 0.5f;
        return;
    }
    out[0] = 0.5f + eyeX / denominator;
    out[1] = 0.5f - eyeY / denominator;
}

void UploadModelEntity(const refEntity_t &entity)
{
    const auto modelIt = modelAssets.find(entity.hModel);
    if (modelIt == modelAssets.end() || !modelIt->second.valid) {
        return;
    }
    const ModelAsset &asset = modelIt->second;

    const uint32_t entityCluster = ClusterIndexForPoint(entity.origin);

    const QrGeometryPrimaryVisibilityType visibility =
        (entity.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) != 0
        ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON
        : QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    const int frame = std::clamp(entity.frame, 0, asset.numFrames - 1);
    const int oldFrame = std::clamp(entity.oldframe, 0, asset.numFrames - 1);
    const float backLerp = frame == oldFrame ? 0.0f : std::clamp(entity.backlerp, 0.0f, 1.0f);
    const float frontLerp = 1.0f - backLerp;

    for (const ModelSurface &surface : asset.surfaces) {
        if (surface.numVerts <= 0 || surface.indices.empty()) {
            continue;
        }
        // tcGen environment surfaces have no usable UVs, so the texcoords are always rebuilt from
        // the eye reflection vector. The authored blend factors decide how the stage is drawn.
        // one/zero replaces the surface, so it is the pickup body itself and has to be traced like
        // any other material: the map light dims it exactly the way the retail lightmaps do.
        // one/one is the additive glass shell, which runs unlit on the blending overlay.
        bool environmentMapped = false;
        QrBlendFactor environmentSource = QR_BLEND_FACTOR_ONE;
        QrBlendFactor environmentDestination = QR_BLEND_FACTOR_ZERO;
        QrMaterial environmentMaterial = QR_NO_MATERIAL;
        std::string environmentDefinitionMap;
        if (entity.customShader == 0) {
            const char *shaderName = ResolveModelShaderName(entity, surface);
            LoadShaderScripts();
            const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
            if (definitionIt != shaderDefinitions.end() && definitionIt->second.environmentMap) {
                environmentMapped = true;
                environmentSource = definitionIt->second.blendSrc;
                environmentDestination = definitionIt->second.blendDst;
                environmentMaterial = GetMaterialForTextureName(definitionIt->second.map.c_str());
                environmentDefinitionMap = definitionIt->second.map;
                if (hostLogEnabled) {
                    static std::unordered_set<std::string> loggedEnvironmentShaders;
                    if (loggedEnvironmentShaders.insert(definitionIt->first).second) {
                        HostLog("UploadModelEntity: env stage %s map=%s src=%d dst=%d",
                            definitionIt->first.c_str(), definitionIt->second.map.c_str(),
                            static_cast<int>(environmentSource),
                            static_cast<int>(environmentDestination));
                    }
                }
            }
        }
        const int surfaceFrame = std::min(frame, static_cast<int>(surface.positions.size() /
            (static_cast<size_t>(surface.numVerts) * 3)) - 1);
        const int surfaceOldFrame = std::min(oldFrame, static_cast<int>(surface.positions.size() /
            (static_cast<size_t>(surface.numVerts) * 3)) - 1);
        if (surfaceFrame < 0 || surfaceOldFrame < 0) {
            continue;
        }

        std::vector<QrVertex> vertices(static_cast<size_t>(surface.numVerts));
        const size_t newBase = static_cast<size_t>(surfaceFrame) * surface.numVerts * 3;
        const size_t oldBase = static_cast<size_t>(surfaceOldFrame) * surface.numVerts * 3;
        for (int vertexIndex = 0; vertexIndex < surface.numVerts; ++vertexIndex) {
            const float localX = surface.positions[oldBase + static_cast<size_t>(vertexIndex) * 3 + 0] * backLerp +
                surface.positions[newBase + static_cast<size_t>(vertexIndex) * 3 + 0] * frontLerp;
            const float localY = surface.positions[oldBase + static_cast<size_t>(vertexIndex) * 3 + 1] * backLerp +
                surface.positions[newBase + static_cast<size_t>(vertexIndex) * 3 + 1] * frontLerp;
            const float localZ = surface.positions[oldBase + static_cast<size_t>(vertexIndex) * 3 + 2] * backLerp +
                surface.positions[newBase + static_cast<size_t>(vertexIndex) * 3 + 2] * frontLerp;

            float localNormal[3] = {
                surface.normals[oldBase + static_cast<size_t>(vertexIndex) * 3 + 0] * backLerp +
                    surface.normals[newBase + static_cast<size_t>(vertexIndex) * 3 + 0] * frontLerp,
                surface.normals[oldBase + static_cast<size_t>(vertexIndex) * 3 + 1] * backLerp +
                    surface.normals[newBase + static_cast<size_t>(vertexIndex) * 3 + 1] * frontLerp,
                surface.normals[oldBase + static_cast<size_t>(vertexIndex) * 3 + 2] * backLerp +
                    surface.normals[newBase + static_cast<size_t>(vertexIndex) * 3 + 2] * frontLerp
            };

            QrVertex &destination = vertices[static_cast<size_t>(vertexIndex)];
            destination = {};
            destination.position[0] = entity.origin[0] + entity.axis[0][0] * localX +
                entity.axis[1][0] * localY + entity.axis[2][0] * localZ;
            destination.position[1] = entity.origin[1] + entity.axis[0][1] * localX +
                entity.axis[1][1] * localY + entity.axis[2][1] * localZ;
            destination.position[2] = entity.origin[2] + entity.axis[0][2] * localX +
                entity.axis[1][2] * localY + entity.axis[2][2] * localZ;
            destination.normal[0] = entity.axis[0][0] * localNormal[0] +
                entity.axis[1][0] * localNormal[1] + entity.axis[2][0] * localNormal[2];
            destination.normal[1] = entity.axis[0][1] * localNormal[0] +
                entity.axis[1][1] * localNormal[1] + entity.axis[2][1] * localNormal[2];
            destination.normal[2] = entity.axis[0][2] * localNormal[0] +
                entity.axis[1][2] * localNormal[1] + entity.axis[2][2] * localNormal[2];
            NormalizeVector(destination.normal);
            if (environmentMapped) {
                ComputeModelEnvironmentTexCoord(destination.position, destination.normal,
                    destination.texCoord);
            } else {
                destination.texCoord[0] = surface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 0];
                destination.texCoord[1] = surface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 1];
            }
            destination.packedColor = 0xFFFFFFFFu;
            destination.cluster = entityCluster;
        }

        const QrMaterial material = environmentMaterial != QR_NO_MATERIAL
            ? environmentMaterial
            : ResolveModelMaterial(entity, asset, surface);
        std::vector<uint32_t> indices = surface.indices;
        if (qrayModelWinding != nullptr && qrayModelWinding->integer != 0) {
            for (size_t index = 0; index + 2 < indices.size(); index += 3) {
                std::swap(indices[index + 1], indices[index + 2]);
            }
        }
        if (environmentMapped) {
            const bool additiveEnvironment = environmentSource != QR_BLEND_FACTOR_ONE ||
                environmentDestination != QR_BLEND_FACTOR_ZERO;
            if (additiveEnvironment) {
                if (qrayEnvShellTrace != nullptr && qrayEnvShellTrace->integer == 2) {
                    // Debug: draw the body alone to inspect the traced pass without the shell.
                } else if (qrayEnvShellTrace != nullptr && qrayEnvShellTrace->integer != 0) {
                    UploadDynamicGeometry(vertices, indices, material, visibility,
                        QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT,
                        QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD, 0.0f);
                } else {
                    const float shellScale = std::clamp(CvarFloat(qrayEnvShellScale, 0.1f), 0.0f, 1.0f);
                    const QrMaterial shellMaterial = GetScaledEnvMaterialForTextureName(
                        environmentDefinitionMap.empty() ? nullptr : environmentDefinitionMap.c_str(),
                        shellScale);
                    UploadRasterizedWorldGeometry(vertices, indices,
                        shellMaterial == QR_NO_MATERIAL ? material : shellMaterial,
                        environmentSource, environmentDestination);
                }
            } else if (qrayEnvMapRaster != nullptr && qrayEnvMapRaster->integer != 0) {
                const float bodyScale = std::clamp(CvarFloat(qrayEnvMapScale, 1.15f), 0.0f, 8.0f);
                const float bodySaturation =
                    std::clamp(CvarFloat(qrayEnvMapSaturation, 1.6f), 0.0f, 4.0f);
                const QrMaterial bodyMaterial = environmentDefinitionMap.empty()
                    ? material
                    : GetScaledEnvMaterialForTextureName(environmentDefinitionMap.c_str(), bodyScale,
                          bodySaturation);
                UploadRasterizedWorldGeometry(vertices, indices,
                    bodyMaterial == QR_NO_MATERIAL ? material : bodyMaterial,
                    environmentSource, environmentDestination);
            } else {
                UploadDynamicGeometry(vertices, indices, material, visibility,
                    QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE,
                    0.0f);
            }
            continue;
        }
        UploadDynamicGeometry(vertices, indices, material, visibility,
            QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE, 0.0f);
    }
}

void UploadSpriteEntity(const refEntity_t &entity)
{
    const float radius = entity.radius;
    if (radius <= 0.0f) {
        return;
    }

    const float left[3] = {sceneDef.viewaxis[1][0], sceneDef.viewaxis[1][1], sceneDef.viewaxis[1][2]};
    const float up[3] = {sceneDef.viewaxis[2][0], sceneDef.viewaxis[2][1], sceneDef.viewaxis[2][2]};
    float rotatedLeft[3] = {left[0], left[1], left[2]};
    float rotatedUp[3] = {up[0], up[1], up[2]};
    if (entity.rotation != 0.0f) {
        const float angle = kPi * entity.rotation / 180.0f;
        const float sine = std::sin(angle);
        const float cosine = std::cos(angle);
        for (int axis = 0; axis < 3; ++axis) {
            rotatedLeft[axis] = (left[axis] * cosine - up[axis] * sine) * radius;
            rotatedUp[axis] = (up[axis] * cosine + left[axis] * sine) * radius;
        }
    } else {
        for (int axis = 0; axis < 3; ++axis) {
            rotatedLeft[axis] *= radius;
            rotatedUp[axis] *= radius;
        }
    }

    const float cornerOffsets[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    const float textureCoords[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    const uint32_t spriteCluster = ClusterIndexForPoint(entity.origin);
    std::vector<QrVertex> vertices(4);
    for (int corner = 0; corner < 4; ++corner) {
        QrVertex &vertex = vertices[static_cast<size_t>(corner)];
        vertex = {};
        for (int axis = 0; axis < 3; ++axis) {
            vertex.position[axis] = entity.origin[axis] +
                rotatedLeft[axis] * cornerOffsets[corner][0] + rotatedUp[axis] * cornerOffsets[corner][1];
            vertex.normal[axis] = -sceneDef.viewaxis[0][axis];
        }
        vertex.texCoord[0] = textureCoords[corner][0];
        vertex.texCoord[1] = textureCoords[corner][1];
        vertex.packedColor = PackColor(entity.shader);
        vertex.cluster = spriteCluster;
    }

    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const QrGeometryPrimaryVisibilityType visibility =
        (entity.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) != 0
        ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON
        : QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    const float layerAlpha = entity.shader.rgba[3] != 0
        ? static_cast<float>(entity.shader.rgba[3]) / 255.0f
        : 1.0f;
    const QrGeometryMaterialBlendType blend = EffectBlendForHandle(entity.customShader);
    const QrMaterial material = GetMaterialForHandle(entity.customShader);
    QrBlendFactor blendSource = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDestination = QR_BLEND_FACTOR_ONE;
    const bool blended = EffectBlendFactorsForHandle(entity.customShader, blendSource, blendDestination);
    if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
        HostLog("UploadSprite: shader=%d name='%s' radius=%f alpha=%f blend=%d material=%d default=%d "
            "src=%d dst=%d",
            static_cast<int>(entity.customShader), ShaderNameForHandle(entity.customShader),
            radius, layerAlpha, static_cast<int>(blend), static_cast<int>(material),
            material == defaultMaterial ? 1 : 0, static_cast<int>(blendSource),
            static_cast<int>(blendDestination));
    }
    if (blended) {
        UploadRasterizedWorldGeometry(vertices, indices, material, blendSource, blendDestination);
        return;
    }
    UploadDynamicGeometry(vertices, indices, material, visibility,
        QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED, blend, 1.0f,
        QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT, layerAlpha);
}

void UploadBeamEntity(const refEntity_t &entity)
{
    const float direction[3] = {
        entity.oldorigin[0] - entity.origin[0],
        entity.oldorigin[1] - entity.origin[1],
        entity.oldorigin[2] - entity.origin[2]
    };
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length < 1e-4f) {
        return;
    }

    const float normalizedDirection[3] = {direction[0] / length, direction[1] / length, direction[2] / length};
    const float viewDirection[3] = {
        sceneDef.vieworg[0] - entity.origin[0],
        sceneDef.vieworg[1] - entity.origin[1],
        sceneDef.vieworg[2] - entity.origin[2]
    };
    float side[3] = {
        normalizedDirection[1] * viewDirection[2] - normalizedDirection[2] * viewDirection[1],
        normalizedDirection[2] * viewDirection[0] - normalizedDirection[0] * viewDirection[2],
        normalizedDirection[0] * viewDirection[1] - normalizedDirection[1] * viewDirection[0]
    };
    NormalizeVector(side);
    if (side[0] == 0.0f && side[1] == 0.0f && side[2] == 0.0f) {
        side[0] = sceneDef.viewaxis[1][0];
        side[1] = sceneDef.viewaxis[1][1];
        side[2] = sceneDef.viewaxis[1][2];
    }
    const float width = entity.reType == RT_RAIL_CORE ? 6.0f : 4.0f;
    for (int axis = 0; axis < 3; ++axis) {
        side[axis] *= width;
    }

    std::vector<QrVertex> vertices(4);
    const uint32_t beamCluster = ClusterIndexForPoint(entity.origin);
    const float *positions[4] = {
        entity.origin, entity.oldorigin, entity.oldorigin, entity.origin
    };
    const float signs[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    const float textureU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    const float textureV[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    for (int corner = 0; corner < 4; ++corner) {
        QrVertex &vertex = vertices[static_cast<size_t>(corner)];
        vertex = {};
        for (int axis = 0; axis < 3; ++axis) {
            vertex.position[axis] = positions[corner][axis] + side[axis] * signs[corner];
            vertex.normal[axis] = -sceneDef.viewaxis[0][axis];
        }
        vertex.texCoord[0] = textureU[corner];
        vertex.texCoord[1] = textureV[corner];
        vertex.packedColor = PackColor(entity.shader);
        vertex.cluster = beamCluster;
    }

    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const float layerAlpha = entity.shader.rgba[3] != 0
        ? static_cast<float>(entity.shader.rgba[3]) / 255.0f
        : 1.0f;
    const QrMaterial material = GetMaterialForHandle(entity.customShader);
    QrBlendFactor blendSource = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDestination = QR_BLEND_FACTOR_ONE;
    if (EffectBlendFactorsForHandle(entity.customShader, blendSource, blendDestination)) {
        UploadRasterizedWorldGeometry(vertices, indices, material, blendSource, blendDestination);
        return;
    }
    UploadDynamicGeometry(vertices, indices, material,
        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0, QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
        EffectBlendForHandle(entity.customShader), 1.0f,
        QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT, layerAlpha);
}

void UploadPolyGeometry(const PendingPoly &poly)
{
    if (poly.verts.size() < 3) {
        return;
    }

    // Bullet marks, scorches and plasma decals are generated coplanar with the surface they
    // belong to, which makes every ray hit them twice. Push them out along the face normal the
    // polygons were projected with.
    float faceNormal[3] = {0.0f, 0.0f, 0.0f};
    {
        const polyVert_t &v0 = poly.verts[0];
        const polyVert_t &v1 = poly.verts[1];
        const polyVert_t &v2 = poly.verts[2];
        const float ax = v1.xyz[0] - v0.xyz[0];
        const float ay = v1.xyz[1] - v0.xyz[1];
        const float az = v1.xyz[2] - v0.xyz[2];
        const float bx = v2.xyz[0] - v0.xyz[0];
        const float by = v2.xyz[1] - v0.xyz[1];
        const float bz = v2.xyz[2] - v0.xyz[2];
        faceNormal[0] = ay * bz - az * by;
        faceNormal[1] = az * bx - ax * bz;
        faceNormal[2] = ax * by - ay * bx;
        NormalizeVector(faceNormal);
        const float facing = faceNormal[0] * sceneDef.viewaxis[0][0] +
            faceNormal[1] * sceneDef.viewaxis[0][1] + faceNormal[2] * sceneDef.viewaxis[0][2];
        if (facing > 0.0f) {
            faceNormal[0] = -faceNormal[0];
            faceNormal[1] = -faceNormal[1];
            faceNormal[2] = -faceNormal[2];
        }
        const float offset = qrayDecalOffset != nullptr ? qrayDecalOffset->value : 0.25f;
        for (int axis = 0; axis < 3; ++axis) {
            faceNormal[axis] *= offset;
        }
    }

    std::vector<QrVertex> vertices(poly.verts.size());
    const uint32_t polyCluster = ClusterIndexForPoint(poly.verts[0].xyz);
    for (size_t vertexIndex = 0; vertexIndex < poly.verts.size(); ++vertexIndex) {
        const polyVert_t &source = poly.verts[vertexIndex];
        QrVertex &destination = vertices[vertexIndex];
        destination = {};
        destination.position[0] = source.xyz[0] + faceNormal[0];
        destination.position[1] = source.xyz[1] + faceNormal[1];
        destination.position[2] = source.xyz[2] + faceNormal[2];
        destination.normal[0] = -sceneDef.viewaxis[0][0];
        destination.normal[1] = -sceneDef.viewaxis[0][1];
        destination.normal[2] = -sceneDef.viewaxis[0][2];
        destination.texCoord[0] = source.st[0];
        destination.texCoord[1] = source.st[1];
        destination.packedColor = PackColor(source.modulate);
        destination.cluster = polyCluster;
    }

    std::vector<uint32_t> indices;
    indices.reserve((poly.verts.size() - 2) * 3);
    for (size_t vertexIndex = 0; vertexIndex + 2 < poly.verts.size(); ++vertexIndex) {
        indices.push_back(0);
        indices.push_back(static_cast<uint32_t>(vertexIndex + 1));
        indices.push_back(static_cast<uint32_t>(vertexIndex + 2));
    }
    const float layerAlpha = poly.verts[0].modulate.rgba[3] != 0
        ? static_cast<float>(poly.verts[0].modulate.rgba[3]) / 255.0f
        : 1.0f;
    const QrGeometryMaterialBlendType blend = EffectBlendForHandle(poly.shader);
    QrBlendFactor blendSource = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDestination = QR_BLEND_FACTOR_ONE;
    const bool blended = EffectBlendFactorsForHandle(poly.shader, blendSource, blendDestination);
    // GL_ZERO / GL_ONE_MINUS_SRC_COLOR darkens by the source colour, and Q3 keeps the vertex
    // colour of a shadow blob opaque black while the brightness comes from the image. Feeding
    // that vertex colour into the overlay darkens the surface to full black, so darken decals
    // take the image instead.
    if (blend == QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE) {
        for (QrVertex &vertex : vertices) {
            vertex.packedColor = 0xFFFFFFFFu;
        }
    }
    // A darken decal is only dark once its real blend equation runs, so the blend-equation path
    // takes the untouched image instead of the luminance-to-alpha conversion the traced path needs.
    const QrMaterial material = blended ? GetRawMaterialForHandle(poly.shader)
                                        : GetMaterialForHandle(poly.shader);
    if (qrayDebugEntities != nullptr && qrayDebugEntities->integer != 0) {
        float minX = poly.verts[0].xyz[0], maxX = minX;
        float minY = poly.verts[0].xyz[1], maxY = minY;
        float minZ = poly.verts[0].xyz[2], maxZ = minZ;
        for (const polyVert_t &vertex : poly.verts) {
            minX = std::min(minX, vertex.xyz[0]);
            maxX = std::max(maxX, vertex.xyz[0]);
            minY = std::min(minY, vertex.xyz[1]);
            maxY = std::max(maxY, vertex.xyz[1]);
            minZ = std::min(minZ, vertex.xyz[2]);
            maxZ = std::max(maxZ, vertex.xyz[2]);
        }
        HostLog("UploadPoly: shader=%d name='%s' verts=%u alpha=%f blend=%d src=%d dst=%d "
            "material=%d default=%d size=(%.2f %.2f %.2f) modulate=(%u %u %u %u)",
            static_cast<int>(poly.shader), ShaderNameForHandle(poly.shader),
            static_cast<uint32_t>(poly.verts.size()), layerAlpha, static_cast<int>(blend),
            static_cast<int>(blendSource), static_cast<int>(blendDestination),
            static_cast<int>(material), material == defaultMaterial ? 1 : 0,
            maxX - minX, maxY - minY, maxZ - minZ,
            poly.verts[0].modulate.rgba[0], poly.verts[0].modulate.rgba[1],
            poly.verts[0].modulate.rgba[2], poly.verts[0].modulate.rgba[3]);
        static int polyLogBudget = 0;
        const bool impactMark = std::strstr(ShaderNameForHandle(poly.shader), "mrk") != nullptr;
        if (polyLogBudget < 12 && impactMark) {
            ++polyLogBudget;
            float centroid[3] = {0.0f, 0.0f, 0.0f};
            for (const QrVertex &vertex : vertices) {
                for (int axis = 0; axis < 3; ++axis) {
                    centroid[axis] += vertex.position[axis] / static_cast<float>(vertices.size());
                }
            }
            const float dx = centroid[0] - sceneDef.vieworg[0];
            const float dy = centroid[1] - sceneDef.vieworg[1];
            const float dz = centroid[2] - sceneDef.vieworg[2];
            const float forward = dx * sceneDef.viewaxis[0][0] + dy * sceneDef.viewaxis[0][1] +
                dz * sceneDef.viewaxis[0][2];
            HostLog("PolyPlacement: dist=%.1f forward=%.1f vieworg=(%.1f %.1f %.1f) "
                "centroid=(%.1f %.1f %.1f) fwdAxis=(%.2f %.2f %.2f)", std::sqrt(dx * dx + dy * dy + dz * dz), forward,
                sceneDef.vieworg[0], sceneDef.vieworg[1], sceneDef.vieworg[2],
                centroid[0], centroid[1], centroid[2],
                sceneDef.viewaxis[0][0], sceneDef.viewaxis[0][1], sceneDef.viewaxis[0][2]);
        }
    }
    if (qrayPolyDebug != nullptr && qrayPolyDebug->integer == 7) {
        const float scale = 8.0f;
        float centroid[3] = {0.0f, 0.0f, 0.0f};
        for (const QrVertex &vertex : vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                centroid[axis] += vertex.position[axis] / static_cast<float>(vertices.size());
            }
        }
        std::vector<QrVertex> magnified = vertices;
        for (QrVertex &vertex : magnified) {
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = centroid[axis] + (vertex.position[axis] - centroid[axis]) * scale;
            }
        }
        UploadRasterizedWorldGeometry(magnified, indices, material, blendSource, blendDestination);
        return;
    }
    if (qrayPolyDebug != nullptr && qrayPolyDebug->integer == 8) {
        const float scale = 8.0f;
        float centroid[3] = {0.0f, 0.0f, 0.0f};
        for (const QrVertex &vertex : vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                centroid[axis] += vertex.position[axis] / static_cast<float>(vertices.size());
            }
        }
        std::vector<QrVertex> magnified = vertices;
        for (QrVertex &vertex : magnified) {
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = centroid[axis] + (vertex.position[axis] - centroid[axis]) * scale;
            }
        }
        UploadRasterizedWorldGeometry(magnified, indices, material, blendSource, blendDestination,
            static_cast<uint32_t>(QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE));
        return;
    }
    if (qrayPolyDebug != nullptr && qrayPolyDebug->integer >= 4) {
        const float forwardDistance = 96.0f;
        const float scale = 3.0f;
        float centroid[3] = {0.0f, 0.0f, 0.0f};
        for (const QrVertex &vertex : vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                centroid[axis] += vertex.position[axis] / static_cast<float>(vertices.size());
            }
        }
        std::vector<QrVertex> transplanted = vertices;
        for (QrVertex &vertex : transplanted) {
            const float local[3] = {
                (vertex.position[0] - centroid[0]) * scale,
                (vertex.position[1] - centroid[1]) * scale,
                (vertex.position[2] - centroid[2]) * scale};
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] =
                    sceneDef.vieworg[axis] + sceneDef.viewaxis[0][axis] * forwardDistance +
                    sceneDef.viewaxis[1][axis] * local[0] +
                    sceneDef.viewaxis[2][axis] * local[1] +
                    sceneDef.viewaxis[0][axis] * local[2];
            }
        }
        UploadRasterizedWorldGeometry(transplanted, indices, material,
            blendSource, blendDestination);
        return;
    }
    if (qrayPolyDebug != nullptr && qrayPolyDebug->integer > 0) {
        const uint32_t debugState = qrayPolyDebug->integer == 1
            ? 0u
            : static_cast<uint32_t>(QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE);
        const QrMaterial debugMaterial = qrayPolyDebug->integer == 1 ? defaultMaterial : material;
        const QrBlendFactor debugSource = qrayPolyDebug->integer == 1
            ? QR_BLEND_FACTOR_ONE : blendSource;
        const QrBlendFactor debugDestination = qrayPolyDebug->integer == 1
            ? QR_BLEND_FACTOR_ONE : blendDestination;
        UploadRasterizedWorldGeometry(vertices, indices, debugMaterial,
            debugSource, debugDestination, debugState);
        return;
    }
    if (blended) {
        UploadRasterizedWorldGeometry(vertices, indices, material, blendSource, blendDestination);
        return;
    }
    UploadDynamicGeometry(vertices, indices, material,
        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0, QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
        blend, 1.0f, 0, layerAlpha);
}

void UploadTestQuad();

void UploadTestQuad()
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted) {
        return;
    }

    const int variant = qrayTestQuad != nullptr ? qrayTestQuad->integer : 0;
    const float forwardDistance = 96.0f;
    const float halfSize = 48.0f;
    float center[3];
    for (int axis = 0; axis < 3; ++axis) {
        center[axis] = sceneDef.vieworg[axis] + sceneDef.viewaxis[0][axis] * forwardDistance;
    }

    const float corners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    std::vector<QrVertex> vertices(4);
    for (int i = 0; i < 4; ++i) {
        QrVertex &vertex = vertices[static_cast<size_t>(i)];
        vertex = {};
        for (int axis = 0; axis < 3; ++axis) {
            vertex.position[axis] = center[axis] +
                sceneDef.viewaxis[1][axis] * (corners[i][0] * halfSize) +
                sceneDef.viewaxis[2][axis] * (corners[i][1] * halfSize);
            vertex.normal[axis] = -sceneDef.viewaxis[0][axis];
        }
        vertex.texCoord[0] = (corners[i][0] + 1.0f) * 0.5f;
        vertex.texCoord[1] = (corners[i][1] + 1.0f) * 0.5f;
        vertex.packedColor = 0xFFFFFFFFu;
    }
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

    QrMaterial material = defaultMaterial;
    uint32_t pipelineState = 0;
    QrBlendFactor blendSource = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDestination = QR_BLEND_FACTOR_ONE;
    float drawColorAlpha = 1.0f;

    switch (variant) {
    case 2:
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE |
            QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST;
        blendSource = QR_BLEND_FACTOR_SRC_ALPHA;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case 3:
    case 4: {
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE |
            QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST;
        blendSource = QR_BLEND_FACTOR_ZERO;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        if (variant == 3) {
            const QrMaterial markMaterial = GetRawMaterialForHandle(50097);
            if (markMaterial != QR_NO_MATERIAL) {
                material = markMaterial;
            }
        }
        break;
    }
    case 5: {
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
        blendSource = QR_BLEND_FACTOR_SRC_ALPHA;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const QrMaterial puffMaterial = GetMaterialForTextureName("gfx/misc/smokepuff3.tga");
        if (puffMaterial != QR_NO_MATERIAL) {
            material = puffMaterial;
        }
        for (QrVertex &vertex : vertices) {
            vertex.packedColor = 0x52FFFFFFu;
        }
        break;
    }
    case 6: {
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
        blendSource = QR_BLEND_FACTOR_SRC_ALPHA;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const QrMaterial puffMaterial = GetMaterialForTextureName("gfx/misc/smokepuff3.tga");
        if (puffMaterial != QR_NO_MATERIAL) {
            material = puffMaterial;
        }
        break;
    }
    case 9: {
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
        blendSource = QR_BLEND_FACTOR_SRC_ALPHA;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const QrMaterial puffMaterial = GetMaterialForTextureName("gfx/misc/smokepuff3.tga");
        if (puffMaterial != QR_NO_MATERIAL) {
            material = puffMaterial;
        }
        for (QrVertex &vertex : vertices) {
            vertex.packedColor = 0x00FFFFFFu;
        }
        break;
    }
    case 10: {
        pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
        blendSource = QR_BLEND_FACTOR_SRC_ALPHA;
        blendDestination = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const QrMaterial puffMaterial = GetMaterialForTextureName("gfx/misc/smokepuff3.tga");
        if (puffMaterial != QR_NO_MATERIAL) {
            material = puffMaterial;
        }
        drawColorAlpha = 0.25f;
        break;
    }
    default:
        break;
    }

    const std::vector<QrVertex> meterVertices = QuakeVerticesToMeters(vertices);
    QrRasterizedGeometryUploadInfo info{};
    info.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT;
    info.vertexCount = static_cast<uint32_t>(meterVertices.size());
    info.pVertices = meterVertices.data();
    info.indexCount = static_cast<uint32_t>(indices.size());
    info.pIndices = indices.data();
    info.transform = IdentityTransform();
    info.color.data[0] = 1.0f;
    info.color.data[1] = 1.0f;
    info.color.data[2] = 1.0f;
    info.color.data[3] = drawColorAlpha;
    info.texCoordScale[0] = 1.0f;
    info.texCoordScale[1] = 1.0f;
    info.material = material;
    info.pipelineState = pipelineState;
    info.blendFuncSrc = blendSource;
    info.blendFuncDst = blendDestination;

    const QrResult result = qrUploadRasterizedGeometry(qrayInstance, &info, nullptr, nullptr);
    static int loggedVariant = -1;
    if (loggedVariant != variant) {
        loggedVariant = variant;
        HostLog("UploadTestQuad: variant=%d result=%d material=%d pipelineState=%u blend=%d/%d",
            variant, static_cast<int>(result), static_cast<int>(material),
            pipelineState, static_cast<int>(blendSource), static_cast<int>(blendDestination));
    }
}

void UploadPendingSceneGeometry()
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted) {
        pendingEntities.clear();
        pendingPolys.clear();
        return;
    }

    {
        static uint64_t uploadFrameCounter = 0;
        int histogram[RT_MAX_REF_ENTITY_TYPE] = {};
        for (const refEntity_t &entity : pendingEntities) {
            if (entity.reType >= 0 && entity.reType < RT_MAX_REF_ENTITY_TYPE) {
                histogram[entity.reType]++;
            }
        }
        if (++uploadFrameCounter % 60 == 0) {
            HostLog("PendingEntities: total=%zu model=%d poly=%d sprite=%d beam=%d railCore=%d "
                "railRings=%d lightning=%d portal=%d",
                pendingEntities.size(), histogram[RT_MODEL], histogram[RT_POLY],
                histogram[RT_SPRITE], histogram[RT_BEAM], histogram[RT_RAIL_CORE],
                histogram[RT_RAIL_RINGS], histogram[RT_LIGHTNING], histogram[RT_PORTALSURFACE]);
        }
    }

    UploadWorldOverlayGeometry();

    const int skipMask = qraySkipEntities != nullptr ? qraySkipEntities->integer : 0;
    for (const refEntity_t &entity : pendingEntities) {
        if ((entity.renderfx & RF_THIRD_PERSON) != 0) {
            continue;
        }
        switch (entity.reType) {
        case RT_MODEL:
            if ((skipMask & 1) == 0) {
                UploadModelEntity(entity);
            }
            break;
        case RT_SPRITE:
            if ((skipMask & 2) == 0) {
                UploadSpriteEntity(entity);
            }
            break;
        case RT_BEAM:
        case RT_RAIL_CORE:
        case RT_LIGHTNING:
            if ((skipMask & 4) == 0) {
                UploadBeamEntity(entity);
            }
            break;
        case RT_POLY:
        case RT_RAIL_RINGS:
        case RT_PORTALSURFACE:
            break;
        default:
            break;
        }
    }
    if ((skipMask & 8) == 0) {
        for (const PendingPoly &poly : pendingPolys) {
            UploadPolyGeometry(poly);
        }
    }
    if (qrayTestQuad != nullptr && qrayTestQuad->integer != 0) {
        UploadTestQuad();
    }
    UploadSkyGeometry();

    pendingEntities.clear();
    pendingPolys.clear();
}

void DrawFrame()
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted) {
        return;
    }

    const bool renderUiOnly = !frameHasWorldScene;
    if (!sceneRendered) {
        std::memset(&sceneDef, 0, sizeof(sceneDef));
        sceneDef.viewaxis[0][0] = 1.0f;
        sceneDef.viewaxis[1][1] = 1.0f;
        sceneDef.viewaxis[2][2] = 1.0f;
        sceneDef.fov_y = 90.0f;
        sceneDef.time = ri.Milliseconds();
        sceneDef.width = glConfig.vidWidth;
        sceneDef.height = glConfig.vidHeight;
    }

    QrDrawFrameRenderResolutionParams resolution{};
    resolution.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_LINEAR;
    resolution.sharpenTechnique = QR_RENDER_SHARPEN_TECHNIQUE_NONE;
    resolution.resolutionMode = QR_RENDER_RESOLUTION_MODE_CUSTOM;
    resolution.customRenderSize.width = static_cast<uint32_t>(std::max(1, glConfig.vidWidth));
    resolution.customRenderSize.height = static_cast<uint32_t>(std::max(1, glConfig.vidHeight));

    const float giLevel = std::clamp(CvarFloat(qrayGiLevel, 1.0f), 0.0f, 2.0f);
    QrDrawFrameIlluminationParams illumination{};
    illumination.maxBounceShadows = 0;
    illumination.enableSecondBounceForIndirect = giLevel >= 1.5f ? QR_TRUE : QR_FALSE;
    illumination.cellWorldSize = std::max(CvarFloat(qrayCellWorldSize, 2.0f), 2.5e-5f);
    illumination.directDiffuseSensitivityToChange = 0.4f;
    illumination.indirectDiffuseSensitivityToChange = 0.06f;
    illumination.specularSensitivityToChange = 0.03f;
    illumination.polygonalLightSpotlightFactor = 2.0f;
    illumination.q2DepthGradMode = 1;
    illumination.q2LightStatsMode = 1;
    illumination.reflRefrEarlyOut = 1;
    illumination.neeLightSamples = CvarFloat(qrayNeeSamples, 2.0f) < 1.5f ? 1u : 2u;
    illumination.restirEnabled = GlobalLightSamplingEnabled() ? 1u : 0u;
    illumination.restirCandidates = static_cast<uint32_t>(std::clamp(
        qrayRestirCandidates != nullptr ? qrayRestirCandidates->integer : 8, 1, 64));
    const bool indirectHalfRes = qrayIndirectHalfRes == nullptr || qrayIndirectHalfRes->integer != 0;
    illumination.giBounceRays = giLevel > 0.0f && indirectHalfRes ? 0.5f : giLevel;
    illumination.denoiserEnabled = qrayDenoiser == nullptr || qrayDenoiser->integer != 0
        ? QR_TRUE : QR_FALSE;
    illumination.fixedAlbedo = 0.0f;
    illumination.sunBounceRange = std::max(CvarFloat(qraySunBounceRange, 50.8f), 0.0f);
    illumination.sunBounceScale = 1.0f;
    illumination.lightUniqueIdIgnoreFirstPersonViewerShadows = nullptr;

    QrDrawFrameSkyParams sky{};
    const bool proceduralSky = qrayPhysicalSky == nullptr || qrayPhysicalSky->integer != 0;
    const bool physicalSun = qrayPhysicalSkySun == nullptr || qrayPhysicalSkySun->integer != 0;
    if (proceduralSky) {
        sky.skyType = QR_SKY_TYPE_PROCEDURAL;

        const bool automaticSkyColor = qraySkyColor == nullptr ||
            qraySkyColor->string == nullptr || qraySkyColor->string[0] == '\0' ||
            _stricmp(qraySkyColor->string, "auto") == 0;
        if (automaticSkyColor) {
            const float maxComponent = std::max(
                skyAverageColor[0], std::max(skyAverageColor[1], skyAverageColor[2]));
            if (maxComponent < 0.03f) {
                sky.skyColorDefault.data[0] = 0.0f;
                sky.skyColorDefault.data[1] = 0.397f;
                sky.skyColorDefault.data[2] = 1.0f;
            } else {
                const float scale = 0.45f / maxComponent;
                sky.skyColorDefault.data[0] = std::clamp(skyAverageColor[0] * scale, 0.0f, 1.0f);
                sky.skyColorDefault.data[1] = std::clamp(skyAverageColor[1] * scale, 0.0f, 1.0f);
                sky.skyColorDefault.data[2] = std::clamp(skyAverageColor[2] * scale, 0.0f, 1.0f);
            }
        } else {
            const float fallbackSkyColor[3] = {
                skyAverageColor[0], skyAverageColor[1], skyAverageColor[2],
            };
            CvarColor(qraySkyColor, fallbackSkyColor, sky.skyColorDefault);
        }
        sky.skyColorMultiplier = kQuakeRaySkyRadianceScale;
        sky.skyColorSaturation = std::clamp(CvarFloat(qrayCloudOpacity, 1.0f), 0.0f, 1.0f);
        sky.skyAmbientLod = std::clamp(CvarFloat(qraySkyAmbientLod, 4.0f), 0.0f, 10.0f);
        const float requestedSkyLightMultiplier = CvarFloat(qraySkyLightMultiplier, 1.0f);
        sky.skyLightMultiplier = std::clamp(requestedSkyLightMultiplier,
            0.0f, kQuakeRaySkyLightMultiplierMax);
        static bool reportedSkyLightClamp = false;
        if (!reportedSkyLightClamp && requestedSkyLightMultiplier > kQuakeRaySkyLightMultiplierMax) {
            reportedSkyLightClamp = true;
            HostLog("DrawFrame: qray_skyLightMultiplier %.2f clamped to %.2f",
                requestedSkyLightMultiplier, kQuakeRaySkyLightMultiplierMax);
        }
        sky.skyNee = QR_TRUE;

        float sunColor[3] = {1.0f, 0.95f, 0.85f};
        float sunDirection[3] = {0.35f, 0.45f, -0.82f};
        float sunIntensity = 3.0f;
        ResolveSun(sunDirection, sunColor, sunIntensity);
        sky.sunDiscColor.data[0] = physicalSun ? sunColor[0] * sunIntensity : 0.0f;
        sky.sunDiscColor.data[1] = physicalSun ? sunColor[1] * sunIntensity : 0.0f;
        sky.sunDiscColor.data[2] = physicalSun ? sunColor[2] * sunIntensity : 0.0f;
        sky.sunDiscSize = std::max(CvarFloat(qraySunDiscSize, 1.0f), 0.0f);

        const float fallbackCloudColor[3] = {1.0f, 1.0f, 1.0f};
        QrFloat3D cloudColor{};
        CvarColor(qrayCloudColor, fallbackCloudColor, cloudColor);
        float *cloud = &sky.skyCubemapRotationTransform.matrix[0][0];
        cloud[0] = cloudColor.data[0];
        cloud[1] = cloudColor.data[1];
        cloud[2] = cloudColor.data[2];
        cloud[3] = std::clamp(CvarFloat(qrayCloudCoverage, 0.2f), 0.0f, 1.0f);
        cloud[4] = std::max(CvarFloat(qrayCloudDensity, 0.8f), 0.0f);
        cloud[5] = std::max(CvarFloat(qrayCloudSpeed, 0.3f), 0.0f);
        cloud[6] = qrayClouds == nullptr || qrayClouds->integer != 0 ? 1.0f : 0.0f;
        cloud[7] = std::max(CvarFloat(qrayCloudHeight, 3500.0f), 1.0f);
        cloud[8] = std::max(CvarFloat(qrayCloudThickness, 2250.0f), 1.0f);
        sky.skyCloudsQuality = static_cast<uint32_t>(std::clamp(
            qrayCloudQuality != nullptr ? qrayCloudQuality->integer : 2,
            0, static_cast<int>(QR_SKY_CLOUDS_MAX_QUALITY)));

        sky.godRaysEnabled = physicalSun &&
            (qrayGodRays == nullptr || qrayGodRays->integer != 0) ? QR_TRUE : QR_FALSE;
        sky.godRaysIntensity = std::max(CvarFloat(qrayGodRaysIntensity, 1.0f), 0.0f);
        sky.godRaysQuality = static_cast<uint32_t>(std::max(
            qrayGodRaysQuality != nullptr ? qrayGodRaysQuality->integer : 2, 0));
        sky.godRaysFromSkyTexture = QR_FALSE;
    } else {
        if (skyCubemap != QR_EMPTY_CUBEMAP) {
            sky.skyType = QR_SKY_TYPE_CUBEMAP;
            sky.skyCubemap = skyCubemap;
        } else {
            sky.skyType = QR_SKY_TYPE_COLOR;
        }
        sky.skyColorDefault.data[0] = skyAverageColor[0];
        sky.skyColorDefault.data[1] = skyAverageColor[1];
        sky.skyColorDefault.data[2] = skyAverageColor[2];
        sky.skyColorMultiplier = 1.0f;
        sky.skyColorSaturation = 1.0f;
        sky.skyAmbientLod = std::clamp(CvarFloat(qraySkyAmbientLod, 4.0f), 0.0f, 10.0f);
        sky.skyLightMultiplier = std::max(CvarFloat(qraySkyLightMultiplier, 1.0f), 0.0f);
        sky.skyNee = QR_TRUE;
        sky.godRaysEnabled = QR_FALSE;
    }
    QuakePositionToMeters(sceneDef.vieworg, sky.skyViewerPosition);

    refdef_t sdkSceneDef = sceneDef;
    QuakePositionToMeters(sceneDef.vieworg, sdkSceneDef.vieworg);
    QrDrawFrameInfo info{};
    BuildViewMatrix(sdkSceneDef, info.view);
    info.worldUpVector.data[0] = 0.0f;
    info.worldUpVector.data[1] = 0.0f;
    info.worldUpVector.data[2] = 1.0f;
    const float aspect = glConfig.vidHeight > 0
        ? static_cast<float>(glConfig.vidWidth) / static_cast<float>(glConfig.vidHeight)
        : 1.0f;
    float fovYRadians = sceneDef.fov_y * (kPi / 180.0f);
    if (sceneDef.fov_x > 0.0f && aspect > 0.0f) {
        fovYRadians = 2.0f * std::atan(std::tan(sceneDef.fov_x * (kPi / 360.0f)) / aspect);
    }
    info.fovYRadians = fovYRadians;
    info.cameraNear = QuakeUnitsToMeters(0.1f);
    info.cameraFar = QuakeUnitsToMeters(8192.0f);
    info.rayLength = QuakeUnitsToMeters(10000.0f);
    info.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT |
        QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT |
        QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
    info.disableRayTracedGeometry = QR_FALSE;
    info.disableRasterization = QR_FALSE;
    info.currentTime = static_cast<double>(sceneDef.time) * 0.001;
    info.disableEyeAdaptation = qrayEyeAdaptation->integer ? QR_FALSE : QR_TRUE;
    info.forceAntiFirefly = QR_TRUE;
    info.pRenderResolutionParams = &resolution;
    info.pIlluminationParams = &illumination;
    info.pSkyParams = &sky;

    QrDrawFrameTonemappingParams tonemapping{};
    tonemapping.minLogLuminance = -3.9f;
    tonemapping.maxLogLuminance = -2.8f;
    tonemapping.luminanceWhitePoint = std::max(qrayWhitePoint->value, 1.0f);
    tonemapping.exposureBias = qrayExposureBias->value;
    tonemapping.tonemapPower = std::clamp(qrayTonemapPower->value, 0.0f, 1.0f);
    tonemapping.highlightDesaturation = 0.0f;
    tonemapping.chromaPreservation = 0.8f;
    tonemapping.exposureSpeedUp = 3.0f;
    tonemapping.exposureSpeedDown = 1.0f;
    tonemapping.exposureLowPercentile = 70.0f;
    tonemapping.exposureHighPercentile = 90.0f;
    tonemapping.minAdaptedLuminance = 0.02f;
    tonemapping.maxAdaptedLuminance = 1.0f;
    tonemapping.tonemapType =
        static_cast<uint32_t>(std::clamp(qrayTonemapType->integer, 0, 4));
    info.pTonemappingParams = &tonemapping;

    info.renderUiOnly = renderUiOnly ? QR_TRUE : QR_FALSE;
    info.enableCpuProfiling = QR_FALSE;

    UploadSceneLights();

    const QrResult result = qrDrawFrame(qrayInstance, &info);
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrDrawFrame failed: %d\n", static_cast<int>(result));
    }

    UpdateReadback(false);
    CopyReadbackToCapture();

    frameStarted = false;
    sceneValid = false;
    sceneRendered = false;
    frameHasWorldScene = false;
}

void ShutdownRenderer(refShutdownCode_t code)
{
    if (code == REF_KEEP_CONTEXT) {
        if (frameStarted && qrayInstance != QR_NULL_HANDLE) {
            DrawFrame();
        }
        frameStarted = false;
        sceneValid = false;
        return;
    }

    if (frameStarted && qrayInstance != QR_NULL_HANDLE) {
        DrawFrame();
    }

    // The SDK may still write into readbackBuffer until the instance is gone, so the
    // host buffers are released only after qrDestroyInstance.
    if (qrayInstance != QR_NULL_HANDLE) {
        DestroySkyAtlas();
        DestroySkyCubemap();
        qrDestroyInstance(qrayInstance);
        qrayInstance = QR_NULL_HANDLE;
    }
    ResetReadback();
    capturePending = false;
    captureBuffer = nullptr;

    if (code == REF_UNLOAD_DLL && commandsRegistered) {
        ri.Cmd_RemoveCommand("screenshot");
        ri.Cmd_RemoveCommand("screenshotJPEG");
        ri.Cmd_RemoveCommand("qray_readback");
        ri.Cmd_RemoveCommand("qray_probe");
        commandsRegistered = false;
    }

    ResetMaterialState();
    frameStarted = false;
    sceneValid = false;
    probeSceneValid = false;

    if (code != REF_KEEP_CONTEXT && code != REF_KEEP_WINDOW) {
        ri.VKimp_Shutdown(code == REF_UNLOAD_DLL ? qtrue : qfalse);
        windowInitialized = false;
    }
}

void BeginRegistration(glconfig_t *config)
{
    HostLog("BeginRegistration: entry config=%p", config);
    RegisterCvars();

    if (config != nullptr) {
        if (!windowInitialized) {
            // vid_restart with REF_KEEP_WINDOW keeps the native window alive while the
            // QuakeRay instance is recreated; initialising it again would leak it.
            HostLog("BeginRegistration: VKimp_Init");
            ri.VKimp_Init(config);
            windowInitialized = true;
            glConfig = *config;
            HostLog("BeginRegistration: VKimp_Init done %dx%d", glConfig.vidWidth, glConfig.vidHeight);
        } else {
            glConfig = *config;
        }
    }

    if (qrayInstance == QR_NULL_HANDLE) {
        if (!CreateQrayInstance()) {
            HostLog("BeginRegistration: CreateQrayInstance failed");
            ri.Printf(PRINT_WARNING, "rendererqray: QuakeRay initialization failed; check qray_shaderDir and qray_blueNoise, or use cl_renderer vulkan\n");
            return;
        }
        if (!ConfigureSpatialConvention()) {
            qrDestroyInstance(qrayInstance);
            qrayInstance = QR_NULL_HANDLE;
            HostLog("BeginRegistration: ConfigureSpatialConvention failed");
            return;
        }
        HostLog("BeginRegistration: CreateDefaultMaterial");
        CreateDefaultMaterial();
        RebuildUiMaterials();
    }

    if (config != nullptr) {
        QrAdapterInfo adapter{};
        if (qrayInstance != QR_NULL_HANDLE && qrGetAdapterInfo(qrayInstance, &adapter) == QR_SUCCESS) {
            CopyString(glConfig.renderer_string, "QuakeRay", sizeof(glConfig.renderer_string));
            CopyString(glConfig.vendor_string, adapter.name, sizeof(glConfig.vendor_string));
            CopyString(glConfig.version_string, QR_API_VERSION, sizeof(glConfig.version_string));
        }
        glConfig.colorBits = 32;
        glConfig.depthBits = 24;
        glConfig.stencilBits = 8;
        glConfig.maxTextureSize = 16384;
        glConfig.numTextureUnits = 8;
        glConfig.deviceSupportsGamma = qfalse;
        glConfig.driverType = GLDRV_ICD;
        glConfig.hardwareType = GLHW_GENERIC;
        glConfig.windowAspect = glConfig.vidHeight > 0
            ? static_cast<float>(glConfig.vidWidth) / static_cast<float>(glConfig.vidHeight)
            : 1.0f;
        *config = glConfig;
    }

    ri.CL_SetScaling(1.0f, glConfig.vidWidth, glConfig.vidHeight);

    if (!commandsRegistered) {
        ri.Cmd_AddCommand("screenshot", QrayScreenshot_f);
        ri.Cmd_AddCommand("screenshotJPEG", QrayScreenshot_f);
        ri.Cmd_AddCommand("qray_readback", QrayReadbackDump_f);
        ri.Cmd_AddCommand("qray_probe", QrayProbe_f);
        commandsRegistered = true;
    }
}

void EndRegistration()
{
    HostLog("EndRegistration");
}

// Console commands can be issued by game VMs (trap_SendConsoleCommand), so file names
// for the screenshot/readback commands must stay relative and inside the game root.
bool IsSafeOutputFileName(const char *name)
{
    if (name == nullptr || name[0] == '\0' || name[0] == '/' || name[0] == '\\') {
        return false;
    }
    if (std::strstr(name, "..") != nullptr || std::strchr(name, ':') != nullptr) {
        return false;
    }
    for (const char *c = name; *c != '\0'; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20) {
            return false;
        }
    }
    return true;
}

void QrayScreenshot_f()
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    const char *name = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv(1) : "";
    if (name[0] == '\0' || _stricmp(name, "silent") == 0 || _stricmp(name, "levelshot") == 0) {
        name = "qray-screenshot.png";
    }
    if (!IsSafeOutputFileName(name)) {
        ri.Printf(PRINT_WARNING, "rendererqray: refusing unsafe screenshot name '%s'\n", name);
        return;
    }

    const QrResult result = qrRequestScreenshot(qrayInstance, name);
    if (result == QR_SUCCESS) {
        ri.Printf(PRINT_ALL, "rendererqray: requested screenshot %s\n", name);
    } else {
        ri.Printf(PRINT_WARNING, "rendererqray: screenshot request failed: %d\n", static_cast<int>(result));
    }
}

void QrayReadbackDump_f()
{
    readbackWantedFrames = kReadbackKeepAliveFrames;
    if (!readbackValid || readbackWidth == 0 || readbackHeight == 0 ||
        readbackReadyPixels.size() < static_cast<size_t>(readbackWidth) * readbackHeight * 4) {
        ri.Printf(PRINT_WARNING, "rendererqray: no completed readback frame yet; readback started, run qray_readback again\n");
        return;
    }

    const uint32_t width = readbackWidth;
    const uint32_t height = readbackHeight;
    const uint32_t imageSize = width * height * 4;

    // BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), little endian, written
    // by hand so the dump does not depend on <windows.h> or struct packing.
    uint8_t header[54] = {};
    const auto put16 = [&header](size_t offset, uint32_t value) {
        header[offset + 0] = static_cast<uint8_t>(value & 0xffu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
    };
    const auto put32 = [&header](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) {
            header[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
        }
    };
    put16(0, 0x4D42u);                                     // bfType 'BM'
    put32(2, static_cast<uint32_t>(sizeof(header)) + imageSize); // bfSize
    put32(10, static_cast<uint32_t>(sizeof(header)));      // bfOffBits
    put32(14, 40u);                                         // biSize
    put32(18, width);                                       // biWidth
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(height))); // top-down
    put16(26, 1u);                                          // biPlanes
    put16(28, 32u);                                         // biBitCount
    put32(30, 0u);                                          // BI_RGB
    put32(34, imageSize);                                   // biSizeImage

    const char *name = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv(1) : "qray-readback.bmp";
    if (!IsSafeOutputFileName(name)) {
        ri.Printf(PRINT_WARNING, "rendererqray: refusing unsafe readback name '%s'\n", name);
        return;
    }
    std::ofstream stream(name, std::ios::binary);
    if (!stream.is_open()) {
        ri.Printf(PRINT_WARNING, "rendererqray: cannot write %s\n", name);
        return;
    }

    stream.write(reinterpret_cast<const char *>(header), sizeof(header));
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = readbackReadyPixels.data() + static_cast<size_t>(y) * readbackRowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t bgra[4] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0], row[x * 4 + 3]};
            stream.write(reinterpret_cast<const char *>(bgra), 4);
        }
    }

    ri.Printf(PRINT_ALL, "rendererqray: wrote %s (%ux%u)\n", name, width, height);
}

void QrayProbe_f()
{
    static int probeSequence = 0;
    const int sequence = ++probeSequence;
    if (!probeSceneValid || markTriangles.empty()) {
        ProbeLog("QRAY_PROBE seq=%d status=NO_SCENE map=%s triangles=%zu",
            sequence, pendingWorldName.c_str(), markTriangles.size());
        ri.Printf(PRINT_WARNING, "rendererqray: qray_probe has no loaded world scene\n");
        return;
    }

    const float *origin = probeSceneDef.vieworg;
    float direction[3] = {
        probeSceneDef.viewaxis[0][0],
        probeSceneDef.viewaxis[0][1],
        probeSceneDef.viewaxis[0][2],
    };
    NormalizeVector(direction);
    if (direction[0] == 0.0f && direction[1] == 0.0f && direction[2] == 0.0f) {
        ProbeLog("QRAY_PROBE seq=%d status=INVALID_VIEW map=%s", sequence, pendingWorldName.c_str());
        ri.Printf(PRINT_WARNING, "rendererqray: qray_probe has an invalid view direction\n");
        return;
    }

    struct ProbeHit
    {
        const MarkTriangle *triangle = nullptr;
        float distance = FLT_MAX;
        float u = 0.0f;
        float v = 0.0f;
        float facing = 0.0f;
        bool frontFacing = false;
    };

    ProbeHit nearestAny;
    ProbeHit nearestFront;
    constexpr float probeDistance = 32768.0f;
    float hitDistance = 0.0f;
    float hitU = 0.0f;
    float hitV = 0.0f;
    for (const MarkTriangle &triangle : markTriangles) {
        if (!RayIntersectsTriangle(triangle, origin, direction, probeDistance,
                hitDistance, &hitU, &hitV)) {
            continue;
        }

        const float facing = triangle.normal[0] * direction[0] +
            triangle.normal[1] * direction[1] +
            triangle.normal[2] * direction[2];
        if (hitDistance < nearestAny.distance) {
            nearestAny.triangle = &triangle;
            nearestAny.distance = hitDistance;
            nearestAny.u = hitU;
            nearestAny.v = hitV;
            nearestAny.facing = facing;
            nearestAny.frontFacing = facing < -0.001f;
        }
        if (facing < -0.001f && hitDistance < nearestFront.distance) {
            nearestFront.triangle = &triangle;
            nearestFront.distance = hitDistance;
            nearestFront.u = hitU;
            nearestFront.v = hitV;
            nearestFront.facing = facing;
            nearestFront.frontFacing = true;
        }
    }

    if (nearestAny.triangle == nullptr) {
        ProbeLog("QRAY_PROBE seq=%d status=NO_HIT map=%s origin=(%.3f %.3f %.3f) "
            "dir=(%.6f %.6f %.6f) tested=%zu",
            sequence, pendingWorldName.c_str(), origin[0], origin[1], origin[2],
            direction[0], direction[1], direction[2], markTriangles.size());
        ri.Printf(PRINT_ALL, "rendererqray: qray_probe #%d: no world triangle hit\n", sequence);
        return;
    }

    const ProbeHit &primary = nearestFront.triangle != nullptr ? nearestFront : nearestAny;
    const MarkTriangle &triangle = *primary.triangle;
    const float barycentric[3] = {1.0f - primary.u - primary.v, primary.u, primary.v};
    float hitPoint[3] = {};
    float hitUv[2] = {};
    float interpolatedNormal[3] = {};
    for (int axis = 0; axis < 3; ++axis) {
        hitPoint[axis] = origin[axis] + direction[axis] * primary.distance;
        for (int corner = 0; corner < 3; ++corner) {
            interpolatedNormal[axis] += triangle.vertexNormals[corner][axis] *
                barycentric[corner];
        }
    }
    for (int axis = 0; axis < 2; ++axis) {
        for (int corner = 0; corner < 3; ++corner) {
            hitUv[axis] += triangle.texCoords[corner][axis] * barycentric[corner];
        }
    }
    NormalizeVector(interpolatedNormal);

    const char *shaderName = "_unknown";
    const WorldSurfaceInfo *surfaceInfo = nullptr;
    if (triangle.sourceSurfaceIndex >= 0 &&
        static_cast<size_t>(triangle.sourceSurfaceIndex) < worldSurfaceInfos.size()) {
        surfaceInfo = &worldSurfaceInfos[static_cast<size_t>(triangle.sourceSurfaceIndex)];
        if (surfaceInfo->valid && surfaceInfo->shaderNum >= 0 &&
            static_cast<size_t>(surfaceInfo->shaderNum) < worldShaderNames.size()) {
            shaderName = worldShaderNames[static_cast<size_t>(surfaceInfo->shaderNum)].c_str();
        }
    }

    const ShaderDefinition *definition = nullptr;
    const auto definitionIt = shaderDefinitions.find(NormalizeShaderName(shaderName));
    if (definitionIt != shaderDefinitions.end()) {
        definition = &definitionIt->second;
    }
    const char *textureName = definition != nullptr && !definition->map.empty()
        ? definition->map.c_str()
        : shaderName;
    const int shaderNum = surfaceInfo != nullptr ? surfaceInfo->shaderNum : -1;
    const QrMaterial material = shaderNum >= 0 &&
        static_cast<size_t>(shaderNum) < shaderMaterials.size()
        ? shaderMaterials[static_cast<size_t>(shaderNum)]
        : QR_NO_MATERIAL;

    float generatedDotWinding = 0.0f;
    float authoredDotGenerated = 0.0f;
    float vertexNormalDotWinding = 0.0f;
    if (surfaceInfo != nullptr) {
        for (int axis = 0; axis < 3; ++axis) {
            generatedDotWinding += surfaceInfo->generatedNormal[axis] *
                surfaceInfo->originalWindingNormal[axis];
            authoredDotGenerated += surfaceInfo->authoredNormal[axis] *
                surfaceInfo->generatedNormal[axis];
            vertexNormalDotWinding += interpolatedNormal[axis] * triangle.normal[axis];
        }
    }

    const int cluster = ClusterIndexForPoint(hitPoint);
    const char *tag = primary.frontFacing ? "QRAY_PROBE" : "QRAY_PROBE_BACKFACE";
    ProbeLog("%s seq=%d status=HIT map=%s origin=(%.3f %.3f %.3f) "
        "dir=(%.6f %.6f %.6f) tested=%zu surface=%d batchTri=%u shaderNum=%d "
        "shader=%s texture=%s material=%d type=%s flags=0x%08x contents=0x%08x "
        "distance=%.3f point=(%.3f %.3f %.3f) uv=(%.6f %.6f) cluster=%d "
        "front=%d rayDotNormal=%.6f triNormal=(%.6f %.6f %.6f) "
        "vertexNormal=(%.6f %.6f %.6f) generatedNormal=(%.6f %.6f %.6f) "
        "authoredNormal=(%.6f %.6f %.6f) originalWinding=(%.6f %.6f %.6f) "
        "uploadedWinding=(%.6f %.6f %.6f) genDotWinding=%.6f "
        "authoredDotGenerated=%.6f vertexNormalDotWinding=%.6f windingFlipped=%d "
        "planarWindingKept=%d alphaTested=%d blend=%d darkenDecal=%d envMap=%d "
        "lightmap=%d lightmapSize=%dx%d patch=%dx%d verts=%d indexes=%d",
        tag, sequence, pendingWorldName.c_str(), origin[0], origin[1], origin[2],
        direction[0], direction[1], direction[2], markTriangles.size(),
        triangle.sourceSurfaceIndex, triangle.sourceTriangleIndex, shaderNum,
        shaderName, textureName, static_cast<int>(material),
        surfaceInfo != nullptr ? SurfaceTypeName(surfaceInfo->surfaceType) : "UNKNOWN",
        surfaceInfo != nullptr ? surfaceInfo->surfaceFlags : 0,
        surfaceInfo != nullptr ? surfaceInfo->contentFlags : 0,
        primary.distance, hitPoint[0], hitPoint[1], hitPoint[2], hitUv[0], hitUv[1],
        cluster, primary.frontFacing ? 1 : 0, primary.facing,
        triangle.normal[0], triangle.normal[1], triangle.normal[2],
        interpolatedNormal[0], interpolatedNormal[1], interpolatedNormal[2],
        surfaceInfo != nullptr ? surfaceInfo->generatedNormal[0] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->generatedNormal[1] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->generatedNormal[2] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->authoredNormal[0] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->authoredNormal[1] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->authoredNormal[2] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->originalWindingNormal[0] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->originalWindingNormal[1] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->originalWindingNormal[2] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->uploadedWindingNormal[0] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->uploadedWindingNormal[1] : 0.0f,
        surfaceInfo != nullptr ? surfaceInfo->uploadedWindingNormal[2] : 0.0f,
        generatedDotWinding, authoredDotGenerated, vertexNormalDotWinding,
        surfaceInfo != nullptr && surfaceInfo->windingFlipped ? 1 : 0,
        surfaceInfo != nullptr && surfaceInfo->planarWindingKept ? 1 : 0,
        surfaceInfo != nullptr && surfaceInfo->alphaTested ? 1 : 0,
        definition != nullptr ? static_cast<int>(definition->blend) : -1,
        definition != nullptr && definition->darkenDecal ? 1 : 0,
        definition != nullptr && definition->environmentMap ? 1 : 0,
        surfaceInfo != nullptr ? surfaceInfo->lightmapNum : -1,
        surfaceInfo != nullptr ? surfaceInfo->lightmapWidth : 0,
        surfaceInfo != nullptr ? surfaceInfo->lightmapHeight : 0,
        surfaceInfo != nullptr ? surfaceInfo->patchWidth : 0,
        surfaceInfo != nullptr ? surfaceInfo->patchHeight : 0,
        surfaceInfo != nullptr ? surfaceInfo->numVerts : 0,
        surfaceInfo != nullptr ? surfaceInfo->numIndexes : 0);

    if (nearestAny.triangle != nullptr && !nearestAny.frontFacing &&
        (nearestFront.triangle == nullptr || nearestAny.distance < nearestFront.distance)) {
        ProbeLog("QRAY_PROBE_BACKFACE seq=%d map=%s surface=%d distance=%.3f "
            "frontSurface=%d frontDistance=%.3f recommendedFix=%s",
            sequence, pendingWorldName.c_str(), nearestAny.triangle->sourceSurfaceIndex,
            nearestAny.distance,
            nearestFront.triangle != nullptr ? nearestFront.triangle->sourceSurfaceIndex : -1,
            nearestFront.triangle != nullptr ? nearestFront.distance : -1.0f,
            "inspect winding, generated normal and authored drawVert normals");
    }

    ri.Printf(PRINT_ALL,
        "rendererqray: qray_probe #%d: %s surface=%d dist=%.1f front=%d texture=%s\n",
        sequence, SurfaceTypeName(surfaceInfo != nullptr ? surfaceInfo->surfaceType : MST_BAD),
        triangle.sourceSurfaceIndex, primary.distance, primary.frontFacing ? 1 : 0,
        textureName);
}

void LoadWorld(const char *name)
{
    HostLog("LoadWorld: name=%s instance=%p", name != nullptr ? name : "(null)", qrayInstance);
    if (name == nullptr || name[0] == '\0' || qrayInstance == QR_NULL_HANDLE) {
        return;
    }
    pendingWorldName = name;
    worldLoaded = false;
    probeSceneValid = false;
}

void ClearScene()
{
    sceneValid = false;
    pendingEntities.clear();
    pendingPolys.clear();
}

void AddRefEntityToScene(const refEntity_t *entity, qboolean intShaderTime)
{
    (void)intShaderTime;
    if (entity == nullptr) {
        return;
    }
    if (pendingEntities.size() >= 16384) {
        return;
    }
    pendingEntities.push_back(*entity);
}

void AddPolyToScene(qhandle_t shader, int numVerts, const polyVert_t *verts, int num)
{
    if (numVerts < 3 || verts == nullptr || num <= 0) {
        return;
    }
    for (int polyIndex = 0; polyIndex < num && pendingPolys.size() < 16384; ++polyIndex) {
        PendingPoly poly;
        poly.shader = shader;
        const polyVert_t *source = verts + static_cast<size_t>(polyIndex) * numVerts;
        poly.verts.assign(source, source + numVerts);
        pendingPolys.push_back(std::move(poly));
    }
}

int LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir)
{
    (void)point;
    ambientLight[0] = ambientLight[1] = ambientLight[2] = 0.0f;
    directedLight[0] = directedLight[1] = directedLight[2] = 0.0f;
    lightDir[0] = 0.0f;
    lightDir[1] = 0.0f;
    lightDir[2] = 1.0f;
    return 0;
}

void AddDynamicLight(const float origin[3], float radius, float r, float g, float b,
    bool additive, bool linear)
{
    if (origin == nullptr || radius <= 0.0f || dynamicLights.size() >= MAX_DLIGHTS) {
        return;
    }

    DynamicLight light{};
    light.origin[0] = origin[0];
    light.origin[1] = origin[1];
    light.origin[2] = origin[2];
    light.color[0] = std::max(r, 0.0f);
    light.color[1] = std::max(g, 0.0f);
    light.color[2] = std::max(b, 0.0f);
    light.radius = radius;
    light.additive = additive;
    light.linear = linear;
    dynamicLights.push_back(light);
}

void AddLightToScene(const vec3_t org, float intensity, float r, float g, float b)
{
    AddDynamicLight(org, intensity, r, g, b, false, false);
}

void AddAdditiveLightToScene(const vec3_t org, float intensity, float r, float g, float b)
{
    AddDynamicLight(org, intensity, r, g, b, true, false);
}

void AddLinearLightToScene(const vec3_t start, const vec3_t end, float intensity, float r, float g, float b)
{
    if (start == nullptr || end == nullptr || intensity <= 0.0f) {
        return;
    }

    const float delta[3] = {
        end[0] - start[0], end[1] - start[1], end[2] - start[2],
    };
    const float lengthSquared = delta[0] * delta[0] + delta[1] * delta[1] +
        delta[2] * delta[2];
    if (lengthSquared <= 1.0e-6f) {
        AddDynamicLight(start, intensity, r, g, b, false, false);
        return;
    }

    const float midpoint[3] = {
        (start[0] + end[0]) * 0.5f,
        (start[1] + end[1]) * 0.5f,
        (start[2] + end[2]) * 0.5f,
    };
    const float radius = std::max(intensity, std::sqrt(lengthSquared) * 0.5f);
    AddDynamicLight(midpoint, radius, r, g, b, false, true);
}

void RenderScene(const refdef_t *fd)
{
    if (fd == nullptr) {
        return;
    }
    const bool worldScene = (fd->rdflags & RDF_NOWORLDMODEL) == 0;
    if (worldScene || !frameHasWorldScene) {
        const bool firstWorldScene = worldScene && !frameHasWorldScene;
        sceneDef = *fd;
        sceneValid = true;
        sceneRendered = true;
        if (worldScene) {
            frameHasWorldScene = true;
            probeSceneDef = *fd;
            probeSceneValid = true;
        }
        if (firstWorldScene) {
            UploadPendingSceneGeometry();
        }
    }
}

void SetColor(const float *rgba)
{
    if (rgba != nullptr) {
        currentColor[0] = rgba[0];
        currentColor[1] = rgba[1];
        currentColor[2] = rgba[2];
        currentColor[3] = rgba[3];
    } else {
        currentColor[0] = 1.0f;
        currentColor[1] = 1.0f;
        currentColor[2] = 1.0f;
        currentColor[3] = 1.0f;
    }
}

void DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t shader)
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted || w <= 0.0f || h <= 0.0f) {
        return;
    }

    const float x0 = x;
    const float y0 = y;
    const float x1 = x + w;
    const float y1 = y + h;

    QrVertex vertices[6] = {};
    const float positions[4][2] = {
        {x0, y0},
        {x1, y0},
        {x1, y1},
        {x0, y1}
    };
    const float texCoords[4][2] = {
        {s1, t1},
        {s2, t1},
        {s2, t2},
        {s1, t2}
    };
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const int corner = order[i];
        vertices[i].position[0] = positions[corner][0];
        vertices[i].position[1] = positions[corner][1];
        vertices[i].position[2] = 0.0f;
        vertices[i].texCoord[0] = texCoords[corner][0];
        vertices[i].texCoord[1] = texCoords[corner][1];
        vertices[i].packedColor = 0xFFFFFFFFu;
    }

    QrMaterial material = defaultMaterial;
    const auto materialIt = uiMaterials.find(shader);
    if (materialIt != uiMaterials.end()) {
        material = materialIt->second;
    }

    float overlayMatrix[16] = {};
    QrViewport viewport{};
    BuildOverlayMatrix(overlayMatrix, viewport);

    QrRasterizedGeometryUploadInfo info{};
    info.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
    info.vertexCount = 6;
    info.pVertices = vertices;
    info.indexCount = 0;
    info.pIndices = nullptr;
    info.transform = IdentityTransform();
    info.color.data[0] = currentColor[0];
    info.color.data[1] = currentColor[1];
    info.color.data[2] = currentColor[2];
    info.color.data[3] = currentColor[3];
    info.texCoordScale[0] = 1.0f;
    info.texCoordScale[1] = 1.0f;
    info.material = material;
    info.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
    info.blendFuncSrc = QR_BLEND_FACTOR_SRC_ALPHA;
    info.blendFuncDst = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    const QrResult result = qrUploadRasterizedGeometry(qrayInstance, &info, overlayMatrix, &viewport);
    if (result != QR_SUCCESS) {
        HostLog("DrawStretchPic: upload failed result=%d", static_cast<int>(result));
    }
}

void DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, byte *data, int client, qboolean dirty)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)cols;
    (void)rows;
    (void)data;
    (void)client;
    (void)dirty;
}

void UploadCinematic(int w, int h, int cols, int rows, byte *data, int client, qboolean dirty)
{
    (void)w;
    (void)h;
    (void)cols;
    (void)rows;
    (void)data;
    (void)client;
    (void)dirty;
}

void BeginFrame(stereoFrame_t stereoFrame)
{
    (void)stereoFrame;
    StartFrame();
}

void EndFrame(int *frontEndMsec, int *backEndMsec)
{
    DrawFrame();
    if (frontEndMsec != nullptr) {
        *frontEndMsec = 0;
    }
    if (backEndMsec != nullptr) {
        *backEndMsec = 0;
    }
}

bool RayIntersectsTriangle(const MarkTriangle &triangle, const float origin[3], const float direction[3],
    float maxDistance, float &hitDistance, float *outU = nullptr, float *outV = nullptr)
{
    const float *v0 = triangle.points[0];
    const float *v1 = triangle.points[1];
    const float *v2 = triangle.points[2];
    const float edge1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
    const float edge2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
    const float pvec[3] = {
       direction[1] * edge2[2] - direction[2] * edge2[1],
       direction[2] * edge2[0] - direction[0] * edge2[2],
       direction[0] * edge2[1] - direction[1] * edge2[0],
    };
    const float determinant = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];
    if (std::fabs(determinant) < 1e-7f) {
       return false;
    }

    const float inverse = 1.0f / determinant;
    const float tvec[3] = {origin[0] - v0[0], origin[1] - v0[1], origin[2] - v0[2]};
    const float u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inverse;
    if (u < 0.0f || u > 1.0f) {
       return false;
    }

    const float qvec[3] = {
       tvec[1] * edge1[2] - tvec[2] * edge1[1],
       tvec[2] * edge1[0] - tvec[0] * edge1[2],
       tvec[0] * edge1[1] - tvec[1] * edge1[0],
    };
    const float v = (direction[0] * qvec[0] + direction[1] * qvec[1] + direction[2] * qvec[2]) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
       return false;
    }

    const float distance = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * inverse;
    if (distance <= 0.0f || distance > maxDistance) {
       return false;
    }
    hitDistance = distance;
    if (outU != nullptr) {
        *outU = u;
    }
    if (outV != nullptr) {
        *outV = v;
    }
    return true;
}

// Each corner of the impact polygon is cast along the projection direction and snapped onto the
// closest front facing world triangle, which reproduces the clipped fragments of the reference
// renderer for the flat surfaces marks are normally used on.
int MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
    vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer)
{
    if (numPoints < 3 || points == nullptr || maxPoints <= 0 || maxFragments <= 0 ||
       pointBuffer == nullptr || fragmentBuffer == nullptr || markTriangles.empty()) {
       return 0;
    }

    constexpr int maxMarkPoints = 64;
    if (numPoints > maxMarkPoints) {
       numPoints = maxMarkPoints;
    }

    float direction[3] = {projection[0], projection[1], projection[2]};
    const float projectionLength = std::sqrt(direction[0] * direction[0] +
       direction[1] * direction[1] + direction[2] * direction[2]);
    if (projectionLength < 1e-5f) {
       return 0;
    }
    for (float &component : direction) {
       component /= projectionLength;
    }

    float mins[3] = {points[0][0], points[0][1], points[0][2]};
    float maxs[3] = {points[0][0], points[0][1], points[0][2]};
    for (int pointIndex = 0; pointIndex < numPoints; ++pointIndex) {
       for (int axis = 0; axis < 3; ++axis) {
           mins[axis] = std::min(mins[axis], points[pointIndex][axis]);
           maxs[axis] = std::max(maxs[axis], points[pointIndex][axis]);
           const float projected = points[pointIndex][axis] + projection[axis];
           mins[axis] = std::min(mins[axis], projected);
           maxs[axis] = std::max(maxs[axis], projected);
           // The box has to include the leaf behind the impact surface as well.
           mins[axis] = std::min(mins[axis], points[pointIndex][axis] - direction[axis] * 20.0f);
           maxs[axis] = std::max(maxs[axis], points[pointIndex][axis] - direction[axis] * 20.0f);
       }
    }

    const int minCell[3] = {
       static_cast<int>(std::floor(mins[0] / kMarkCellSize)),
       static_cast<int>(std::floor(mins[1] / kMarkCellSize)),
       static_cast<int>(std::floor(mins[2] / kMarkCellSize)),
    };
    const int maxCell[3] = {
       static_cast<int>(std::floor(maxs[0] / kMarkCellSize)),
       static_cast<int>(std::floor(maxs[1] / kMarkCellSize)),
       static_cast<int>(std::floor(maxs[2] / kMarkCellSize)),
    };

    std::vector<uint32_t> candidates;
    for (int x = minCell[0]; x <= maxCell[0]; ++x) {
       for (int y = minCell[1]; y <= maxCell[1]; ++y) {
           for (int z = minCell[2]; z <= maxCell[2]; ++z) {
               const auto cell = markGrid.find(MarkCellKey(x, y, z));
               if (cell == markGrid.end()) {
                   continue;
               }
               candidates.insert(candidates.end(), cell->second.begin(), cell->second.end());
           }
       }
    }
    if (candidates.empty()) {
       return 0;
    }

    float hitPoints[maxMarkPoints][3];
    int hitCount = 0;
    for (int pointIndex = 0; pointIndex < numPoints && pointIndex < maxPoints; ++pointIndex) {
       float bestDistance = projectionLength + 1.0f;
       const MarkTriangle *bestTriangle = nullptr;
       for (uint32_t triangleIndex : candidates) {
           const MarkTriangle &triangle = markTriangles[triangleIndex];
           const float facing = triangle.normal[0] * direction[0] + triangle.normal[1] * direction[1] +
               triangle.normal[2] * direction[2];
           if (facing > -0.001f) {
               continue;
           }
           float distance = 0.0f;
           if (!RayIntersectsTriangle(triangle, points[pointIndex], direction, projectionLength, distance)) {
               continue;
           }
           if (distance < bestDistance) {
               bestDistance = distance;
               bestTriangle = &triangle;
           }
       }
       if (bestTriangle == nullptr) {
           continue;
       }
       for (int axis = 0; axis < 3; ++axis) {
           hitPoints[hitCount][axis] = points[pointIndex][axis] + direction[axis] * bestDistance;
       }
       ++hitCount;
    }

    if (hitCount < 3) {
       return 0;
    }

    markFragment_t &fragment = fragmentBuffer[0];
    fragment.firstPoint = 0;
    fragment.numPoints = hitCount;
    for (int pointIndex = 0; pointIndex < hitCount; ++pointIndex) {
       for (int axis = 0; axis < 3; ++axis) {
           pointBuffer[pointIndex * 3 + axis] = hitPoints[pointIndex][axis];
       }
    }
    return 1;
}

const md3Tag_t *FindModelTag(const ModelAsset &asset, int frame, const char *tagName)
{
    if (!asset.valid || asset.numTags <= 0 || tagName == nullptr) {
        return nullptr;
    }
    frame = std::clamp(frame, 0, asset.numFrames - 1);
    const md3Tag_t *tags = asset.tags.data() + static_cast<size_t>(frame) * static_cast<size_t>(asset.numTags);
    for (int tagIndex = 0; tagIndex < asset.numTags; ++tagIndex) {
        if (_stricmp(tags[tagIndex].name, tagName) == 0) {
            return &tags[tagIndex];
        }
    }
    return nullptr;
}

void NormalizeVector(float vector[3])
{
    const float length = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
    if (length > 1e-6f) {
        vector[0] /= length;
        vector[1] /= length;
        vector[2] /= length;
    }
}

int LerpTag(orientation_t *tag, qhandle_t model, int startFrame, int endFrame, float frac, const char *tagName)
{
    if (tag == nullptr) {
        return 0;
    }

    const auto modelIt = modelAssets.find(model);
    if (modelIt == modelAssets.end()) {
        std::memset(tag, 0, sizeof(*tag));
        return 0;
    }

    const ModelAsset &asset = modelIt->second;
    const md3Tag_t *start = FindModelTag(asset, startFrame, tagName);
    const md3Tag_t *end = FindModelTag(asset, endFrame, tagName);
    if (start == nullptr || end == nullptr) {
        std::memset(tag, 0, sizeof(*tag));
        return 0;
    }

    const float frontLerp = std::clamp(frac, 0.0f, 1.0f);
    const float backLerp = 1.0f - frontLerp;
    for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
        tag->origin[axisIndex] = start->origin[axisIndex] * backLerp + end->origin[axisIndex] * frontLerp;
        tag->axis[0][axisIndex] = start->axis[0][axisIndex] * backLerp + end->axis[0][axisIndex] * frontLerp;
        tag->axis[1][axisIndex] = start->axis[1][axisIndex] * backLerp + end->axis[1][axisIndex] * frontLerp;
        tag->axis[2][axisIndex] = start->axis[2][axisIndex] * backLerp + end->axis[2][axisIndex] * frontLerp;
    }
    NormalizeVector(tag->axis[0]);
    NormalizeVector(tag->axis[1]);
    NormalizeVector(tag->axis[2]);
    return 1;
}

void ModelBounds(qhandle_t model, vec3_t mins, vec3_t maxs)
{
    if (mins == nullptr || maxs == nullptr) {
        return;
    }
    const auto modelIt = modelAssets.find(model);
    if (modelIt == modelAssets.end() || !modelIt->second.valid) {
        mins[0] = mins[1] = mins[2] = 0.0f;
        maxs[0] = maxs[1] = maxs[2] = 0.0f;
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        mins[axis] = modelIt->second.bounds[0][axis];
        maxs[axis] = modelIt->second.bounds[1][axis];
    }
}

void RegisterFont(const char *fontName, int pointSize, fontInfo_t *font)
{
    (void)pointSize;
    if (font == nullptr) {
        return;
    }

    const qhandle_t glyphShader = RegisterUiShader("gfx/2d/bigchars");
    std::memset(font, 0, sizeof(*font));
    font->glyphScale = 1.0f;
    CopyString(font->name, fontName != nullptr ? fontName : "bigchars", sizeof(font->name));

    for (int glyphIndex = 0; glyphIndex < GLYPHS_PER_FONT; ++glyphIndex) {
        glyphInfo_t &glyph = font->glyphs[glyphIndex];
        const int column = glyphIndex % 16;
        const int row = glyphIndex / 16;
        glyph.height = 32.0f;
        glyph.top = 0.0f;
        glyph.bottom = 32.0f;
        glyph.pitch = 32.0f;
        glyph.imageWidth = 512;
        glyph.imageHeight = 512;
        glyph.s = static_cast<float>(column) / 16.0f;
        glyph.t = static_cast<float>(row) / 16.0f;
        glyph.s2 = static_cast<float>(column + 1) / 16.0f;
        glyph.t2 = static_cast<float>(row + 1) / 16.0f;
        glyph.glyph = glyphShader;
        CopyString(glyph.shaderName, "gfx/2d/bigchars", sizeof(glyph.shaderName));
    }
}

void RemapShader(const char *oldShader, const char *newShader, const char *offsetTime)
{
    (void)oldShader;
    (void)newShader;
    (void)offsetTime;
}

qboolean GetEntityToken(char *buffer, int size)
{
    (void)buffer;
    (void)size;
    return qfalse;
}

qboolean InPVS(const vec3_t p1, const vec3_t p2)
{
    (void)p1;
    (void)p2;
    return qfalse;
}

void TakeVideoFrame(int h, int w, byte *capture, byte *encode, qboolean motionJpeg)
{
    captureWidth = w;
    captureHeight = h;
    captureBuffer = capture;
    captureEncodeBuffer = encode;
    captureMotionJpeg = motionJpeg != qfalse;
    capturePending = true;
    readbackWantedFrames = kReadbackKeepAliveFrames;
    CopyReadbackToCapture();
    captureBuffer = nullptr;
    captureEncodeBuffer = nullptr;
}

void ThrottleBackend()
{
}

void FinishBloom()
{
}

void SetColorMappings()
{
}

qboolean CanMinimize()
{
    return qtrue;
}

const glconfig_t *GetConfig()
{
    return &glConfig;
}

void VertexLighting(qboolean allowed)
{
    (void)allowed;
}

void SyncRender()
{
}

void SetWorldVisData(const byte *vis)
{
    (void)vis;
}

bool LoadMd3Asset(const char *name, ModelAsset &asset)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    void *fileData = nullptr;
    int fileLength = ri.FS_ReadFile(name, &fileData);
    if ((fileLength <= 0 || fileData == nullptr) && std::strstr(name, ".md3") == nullptr) {
        char withExtension[MAX_QPATH] = {};
        std::snprintf(withExtension, sizeof(withExtension), "%s.md3", name);
        fileLength = ri.FS_ReadFile(withExtension, &fileData);
    }
    if (fileLength <= 0 || fileData == nullptr) {
        return false;
    }

    bool loaded = false;
    const auto *base = static_cast<const uint8_t *>(fileData);
    if (fileLength >= static_cast<int>(sizeof(md3Header_t))) {
        const auto *header = reinterpret_cast<const md3Header_t *>(base);
        if (header->ident == MD3_IDENT && header->version == MD3_VERSION &&
            header->numFrames > 0 && header->numFrames <= MD3_MAX_FRAMES &&
            header->numSurfaces >= 0 && header->numSurfaces <= MD3_MAX_SURFACES &&
            header->ofsFrames <= static_cast<uint32_t>(fileLength) &&
            header->ofsTags <= static_cast<uint32_t>(fileLength) &&
            header->ofsSurfaces <= static_cast<uint32_t>(fileLength) &&
            header->numTags >= 0 && header->numTags <= MD3_MAX_TAGS) {
            const uint64_t framesEnd = static_cast<uint64_t>(header->ofsFrames) +
                static_cast<uint64_t>(header->numFrames) * sizeof(md3Frame_t);
            const uint64_t tagsEnd = static_cast<uint64_t>(header->ofsTags) +
                static_cast<uint64_t>(header->numFrames) * static_cast<uint64_t>(header->numTags) * sizeof(md3Tag_t);
            if (framesEnd <= static_cast<uint64_t>(fileLength) && tagsEnd <= static_cast<uint64_t>(fileLength)) {
                asset = {};
                asset.valid = true;
                asset.numFrames = header->numFrames;
                asset.numTags = header->numTags;
                const auto *frames = reinterpret_cast<const md3Frame_t *>(base + header->ofsFrames);
                std::memcpy(asset.bounds, frames[0].bounds, sizeof(asset.bounds));
                asset.radius = frames[0].radius;
                asset.tags.resize(static_cast<size_t>(asset.numFrames) * static_cast<size_t>(asset.numTags));
                if (!asset.tags.empty()) {
                    std::memcpy(asset.tags.data(), base + header->ofsTags,
                        asset.tags.size() * sizeof(md3Tag_t));
                }

                const uint8_t *surfaceData = base + header->ofsSurfaces;
                const uint8_t *fileEnd = base + fileLength;
                for (int surfaceIndex = 0; surfaceIndex < header->numSurfaces; ++surfaceIndex) {
                    if (surfaceData + sizeof(md3Surface_t) > fileEnd) {
                        asset.valid = false;
                        break;
                    }
                    const auto *surface = reinterpret_cast<const md3Surface_t *>(surfaceData);
                    if (surface->ident != MD3_IDENT || surface->numVerts <= 0 ||
                        surface->numVerts > MD3_MAX_VERTS || surface->numTriangles < 0 ||
                        surface->numTriangles > MD3_MAX_TRIANGLES ||
                        surface->numFrames <= 0 || surface->numFrames > MD3_MAX_FRAMES) {
                        asset.valid = false;
                        break;
                    }
                    const uint64_t surfaceEnd = static_cast<uint64_t>(surface->ofsEnd);
                    if (surfaceEnd > static_cast<uint64_t>(fileEnd - surfaceData) ||
                        surface->ofsTriangles > surface->ofsEnd ||
                        surface->ofsShaders > surface->ofsEnd ||
                        surface->ofsSt > surface->ofsEnd ||
                        surface->ofsXyzNormals > surface->ofsEnd ||
                        surface->numShaders < 0 || surface->numShaders > MD3_MAX_SHADERS) {
                        asset.valid = false;
                        break;
                    }

                    ModelSurface modelSurface;
                    modelSurface.numVerts = surface->numVerts;
                    modelSurface.shaderName.assign(surface->name, strnlen(surface->name, MAX_QPATH));
                    const uint64_t shadersBytes = static_cast<uint64_t>(surface->numShaders) * sizeof(md3Shader_t);
                    if (surface->numShaders > 0 &&
                        static_cast<uint64_t>(surface->ofsShaders) + shadersBytes <= surface->ofsEnd) {
                        const auto *shader = reinterpret_cast<const md3Shader_t *>(surfaceData + surface->ofsShaders);
                        if (shader->name[0] != '\0') {
                            modelSurface.shaderName.assign(shader->name, strnlen(shader->name, MAX_QPATH));
                        }
                    }

                    const uint64_t triangleCount = static_cast<uint64_t>(surface->numTriangles);
                    const uint64_t triangleBytes = triangleCount * sizeof(md3Triangle_t);
                    if (static_cast<uint64_t>(surface->ofsTriangles) + triangleBytes > surface->ofsEnd ||
                        static_cast<uint64_t>(surface->ofsSt) + static_cast<uint64_t>(surface->numVerts) * sizeof(md3St_t) > surface->ofsEnd) {
                        asset.valid = false;
                        break;
                    }

                    const auto *triangles = reinterpret_cast<const md3Triangle_t *>(surfaceData + surface->ofsTriangles);
                    modelSurface.indices.reserve(static_cast<size_t>(triangleCount) * 3);
                    bool indicesValid = true;
                    for (uint64_t triangleIndex = 0; triangleIndex < triangleCount && indicesValid; ++triangleIndex) {
                        const md3Triangle_t &triangle = triangles[triangleIndex];
                        for (int corner = 0; corner < 3; ++corner) {
                            // The indexes go straight to qrUploadGeometry; an out-of-range
                            // value would make the SDK read past the vertex array.
                            const int vertex = triangle.indexes[corner];
                            if (vertex < 0 || vertex >= surface->numVerts) {
                                indicesValid = false;
                                break;
                            }
                            modelSurface.indices.push_back(static_cast<uint32_t>(vertex));
                        }
                    }
                    if (!indicesValid) {
                        HostLog("LoadMd3Asset: %s has an out-of-range triangle index", name);
                        asset.valid = false;
                        break;
                    }

                    const auto *texCoords = reinterpret_cast<const md3St_t *>(surfaceData + surface->ofsSt);
                    modelSurface.texCoords.resize(static_cast<size_t>(surface->numVerts) * 2);
                    for (int vertexIndex = 0; vertexIndex < surface->numVerts; ++vertexIndex) {
                        modelSurface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 0] = texCoords[vertexIndex].st[0];
                        modelSurface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 1] = texCoords[vertexIndex].st[1];
                    }

                    const uint64_t xyzBytes = static_cast<uint64_t>(surface->numVerts) *
                        static_cast<uint64_t>(surface->numFrames) * sizeof(short) * 4;
                    if (static_cast<uint64_t>(surface->ofsXyzNormals) + xyzBytes > surface->ofsEnd) {
                        asset.valid = false;
                        break;
                    }

                    const auto *xyzNormals = reinterpret_cast<const short *>(surfaceData + surface->ofsXyzNormals);
                    modelSurface.positions.resize(static_cast<size_t>(surface->numFrames) *
                        static_cast<size_t>(surface->numVerts) * 3);
                    modelSurface.normals.resize(modelSurface.positions.size());
                    for (int frameIndex = 0; frameIndex < surface->numFrames; ++frameIndex) {
                        for (int vertexIndex = 0; vertexIndex < surface->numVerts; ++vertexIndex) {
                            const short *source = xyzNormals +
                                (static_cast<size_t>(frameIndex) * surface->numVerts + vertexIndex) * 4;
                            const size_t destination = (static_cast<size_t>(frameIndex) * surface->numVerts + vertexIndex) * 3;
                            modelSurface.positions[destination + 0] = static_cast<float>(source[0]) * MD3_XYZ_SCALE;
                            modelSurface.positions[destination + 1] = static_cast<float>(source[1]) * MD3_XYZ_SCALE;
                            modelSurface.positions[destination + 2] = static_cast<float>(source[2]) * MD3_XYZ_SCALE;

                            const uint32_t lat = (static_cast<uint32_t>(source[3]) >> 8) & 0xffu;
                            const uint32_t lng = static_cast<uint32_t>(source[3]) & 0xffu;
                            const float latRadians = static_cast<float>(lat) * (2.0f * kPi / 256.0f);
                            const float lngRadians = static_cast<float>(lng) * (2.0f * kPi / 256.0f);
                            const float sinLat = std::sin(latRadians);
                            const float cosLat = std::cos(latRadians);
                            const float sinLng = std::sin(lngRadians);
                            modelSurface.normals[destination + 0] = cosLat * sinLng;
                            modelSurface.normals[destination + 1] = sinLat * sinLng;
                            modelSurface.normals[destination + 2] = std::cos(lngRadians);
                        }
                    }

                    asset.surfaces.push_back(std::move(modelSurface));
                    surfaceData += surface->ofsEnd;
                }

                loaded = asset.valid;
            }
        }
    }

    ri.FS_FreeFile(fileData);
    if (!loaded) {
        asset = {};
    }
    return loaded;
}

SkinAsset LoadSkinAsset(const char *name)
{
    SkinAsset skin;
    if (name == nullptr || name[0] == '\0') {
        return skin;
    }

    void *fileData = nullptr;
    const int fileLength = ri.FS_ReadFile(name, &fileData);
    if (fileLength <= 0 || fileData == nullptr) {
        return skin;
    }

    const char *cursor = static_cast<const char *>(fileData);
    const char *end = cursor + fileLength;
    while (cursor < end) {
        const char *lineEnd = cursor;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
            ++lineEnd;
        }
        std::string line(cursor, lineEnd - cursor);
        cursor = lineEnd;
        while (cursor < end && (*cursor == '\n' || *cursor == '\r')) {
            ++cursor;
        }

        const size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const size_t comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        size_t firstStart = 0;
        size_t firstEnd = comma;
        while (firstStart < firstEnd && std::isspace(static_cast<unsigned char>(line[firstStart]))) {
            ++firstStart;
        }
        while (firstEnd > firstStart && std::isspace(static_cast<unsigned char>(line[firstEnd - 1]))) {
            --firstEnd;
        }
        size_t secondStart = comma + 1;
        size_t secondEnd = line.size();
        while (secondStart < secondEnd && std::isspace(static_cast<unsigned char>(line[secondStart]))) {
            ++secondStart;
        }
        while (secondEnd > secondStart && std::isspace(static_cast<unsigned char>(line[secondEnd - 1]))) {
            --secondEnd;
        }
        if (firstStart < firstEnd && secondStart < secondEnd) {
            skin.shaderBySurface.emplace(line.substr(firstStart, firstEnd - firstStart),
                line.substr(secondStart, secondEnd - secondStart));
        }
    }

    ri.FS_FreeFile(fileData);
    return skin;
}

qhandle_t RegisterModel(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return 0;
    }

    const std::string key(name);
    const auto existing = modelHandles.find(key);
    if (existing != modelHandles.end()) {
        return existing->second;
    }

    const qhandle_t handle = nextModelHandle++;
    ModelAsset asset;
    if (!LoadMd3Asset(name, asset)) {
        HostLog("RegisterModel: failed to load %s", name);
        ri.Printf(PRINT_DEVELOPER, "rendererqray: failed to load model %s\n", name);
    }
    modelAssets.emplace(handle, std::move(asset));
    modelHandles.emplace(key, handle);
    return handle;
}

qhandle_t RegisterSkin(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return 0;
    }

    const std::string key(name);
    const auto existing = skinHandles.find(key);
    if (existing != skinHandles.end()) {
        return existing->second;
    }

    const qhandle_t handle = nextSkinHandle++;
    skinAssets.emplace(handle, LoadSkinAsset(name));
    skinHandles.emplace(key, handle);
    return handle;
}

qhandle_t RegisterShader(const char *name)
{
    return RegisterUiShader(name);
}

qhandle_t RegisterShaderNoMip(const char *name)
{
    return RegisterUiShader(name);
}

}

extern "C" Q_EXPORT refexport_t *QDECL GetRefAPI(int apiVersion, refimport_t *rimp)
{
    ri = *rimp;
    HostLog("GetRefAPI: apiVersion=%d expected=%d rimp=%p", apiVersion, REF_API_VERSION, rimp);

    std::memset(&re, 0, sizeof(re));

    if (apiVersion != REF_API_VERSION) {
        ri.Printf(PRINT_ALL, "Mismatched REF_API_VERSION: expected %i, got %i\n", REF_API_VERSION, apiVersion);
        return nullptr;
    }

    std::memset(&glConfig, 0, sizeof(glConfig));

    re.Shutdown = ShutdownRenderer;
    re.BeginRegistration = BeginRegistration;
    re.RegisterModel = RegisterModel;
    re.RegisterSkin = RegisterSkin;
    re.RegisterShader = RegisterShader;
    re.RegisterShaderNoMip = RegisterShaderNoMip;
    re.LoadWorld = LoadWorld;
    re.SetWorldVisData = SetWorldVisData;
    re.EndRegistration = EndRegistration;
    re.ClearScene = ClearScene;
    re.AddRefEntityToScene = AddRefEntityToScene;
    re.AddPolyToScene = AddPolyToScene;
    re.LightForPoint = LightForPoint;
    re.AddLightToScene = AddLightToScene;
    re.AddAdditiveLightToScene = AddAdditiveLightToScene;
    re.AddLinearLightToScene = AddLinearLightToScene;
    re.RenderScene = RenderScene;
    re.SetColor = SetColor;
    re.DrawStretchPic = DrawStretchPic;
    re.DrawStretchRaw = DrawStretchRaw;
    re.UploadCinematic = UploadCinematic;
    re.BeginFrame = BeginFrame;
    re.EndFrame = EndFrame;
    re.MarkFragments = MarkFragments;
    re.LerpTag = LerpTag;
    re.ModelBounds = ModelBounds;
    re.RegisterFont = RegisterFont;
    re.RemapShader = RemapShader;
    re.GetEntityToken = GetEntityToken;
    re.inPVS = InPVS;
    re.TakeVideoFrame = TakeVideoFrame;
    re.ThrottleBackend = ThrottleBackend;
    re.FinishBloom = FinishBloom;
    re.SetColorMappings = SetColorMappings;
    re.CanMinimize = CanMinimize;
    re.GetConfig = GetConfig;
    re.VertexLighting = VertexLighting;
    re.SyncRender = SyncRender;

    return &re;
}
