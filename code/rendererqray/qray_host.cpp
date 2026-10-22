#include <windows.h>

#include <algorithm>
#include <cctype>
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

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kReadbackFormat = QR_FRAME_READBACK_FORMAT_RGBA8_UNORM;

refexport_t re;
glconfig_t glConfig;

QrInstance qrayInstance = QR_NULL_HANDLE;

cvar_t *qrayShaderDir = nullptr;
cvar_t *qrayBlueNoise = nullptr;
cvar_t *qrayWaterNormal = nullptr;
cvar_t *qrayConfig = nullptr;
cvar_t *qraySunColor = nullptr;
cvar_t *qraySunIntensity = nullptr;
cvar_t *qrayLightScale = nullptr;
cvar_t *qrayLightRadiusScale = nullptr;
cvar_t *qrayViewMode = nullptr;
cvar_t *qrayModelWinding = nullptr;
cvar_t *qrayCullBackFaces = nullptr;

QrMaterial defaultMaterial = QR_NO_MATERIAL;
std::vector<QrMaterial> shaderMaterials;
std::vector<uint8_t> shaderMaterialAttempted;

struct MapLight
{
    float position[3];
    float color[3];
    float radius;
};

struct GeometryBatch
{
    QrMaterial material = QR_NO_MATERIAL;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
};

std::vector<MapLight> mapLights;

std::unordered_map<qhandle_t, QrMaterial> uiMaterials;
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

struct ShaderDefinition
{
    std::string map;
};

std::unordered_map<qhandle_t, ModelAsset> modelAssets;
std::unordered_map<std::string, qhandle_t> modelHandles;
std::unordered_map<std::string, QrMaterial> textureMaterials;
std::unordered_map<std::string, ShaderDefinition> shaderDefinitions;
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

std::vector<refEntity_t> pendingEntities;
std::vector<PendingPoly> pendingPolys;
uint64_t dynamicGeometryId = 0x4000000000000000ull;

void NormalizeVector(float vector[3]);

bool frameStarted = false;
bool sceneValid = false;
bool sceneRendered = false;
bool frameHasWorldScene = false;
refdef_t sceneDef;
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

void HostLog(const char *format, ...)
{
    if (!hostLogEnabled) {
        return;
    }

    char path[MAX_PATH] = {};
    DWORD length = GetTempPathA(MAX_PATH, path);
    if (length == 0 || length + 16 >= MAX_PATH) {
        return;
    }
    std::strcat(path, "qray_host.log");

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

    qraySunColor = ri.Cvar_Get("qray_sunColor", "1.0 0.95 0.85", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySunColor, "QuakeRay directional light color.");

    qraySunIntensity = ri.Cvar_Get("qray_sunIntensity", "3.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qraySunIntensity, "QuakeRay directional light intensity.");

    qrayLightScale = ri.Cvar_Get("qray_lightScale", "1600", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightScale, "Intensity scale applied to Quake 3 map light entities.");

    qrayLightRadiusScale = ri.Cvar_Get("qray_lightRadiusScale", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayLightRadiusScale, "Radius scale applied to Quake 3 map light entities.");

    qrayViewMode = ri.Cvar_Get("qray_viewMode", "7", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayViewMode, "Debug camera matrix layout selection for QuakeRay.");

    qrayModelWinding = ri.Cvar_Get("qray_modelWinding", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayModelWinding, "Reverse MD3 triangle winding for QuakeRay ray culling.");

    qrayCullBackFaces = ri.Cvar_Get("qray_cullBackFaces", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(qrayCullBackFaces, "Cull back-facing triangles in QuakeRay.");
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

bool LoadQ3Texture(const char *shaderName, int &width, int &height, std::vector<uint8_t> &pixels)
{
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
        "scripts/test.shader"
    };

    for (const char *scriptName : scriptNames) {
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
        std::string stageMap;
        std::string preferredMap;
        bool stageHasEnvironment = false;
        int depth = 0;
        for (size_t index = 0; index < tokens.size(); ++index) {
            const std::string &token = tokens[index];
            if (token == "{") {
                if (depth == 0) {
                    currentName = pendingName;
                    stageMap.clear();
                    preferredMap.clear();
                    depth = 1;
                } else {
                    if (depth == 1) {
                        stageMap.clear();
                        stageHasEnvironment = false;
                    }
                    ++depth;
                }
                continue;
            }
            if (token == "}") {
                if (depth == 2) {
                    if (!stageMap.empty()) {
                        if (!stageHasEnvironment) {
                            preferredMap = stageMap;
                        } else if (preferredMap.empty()) {
                            preferredMap = stageMap;
                        }
                    }
                    stageMap.clear();
                    stageHasEnvironment = false;
                    depth = 1;
                } else if (depth == 1) {
                    if (!currentName.empty() && !preferredMap.empty()) {
                        shaderDefinitions[NormalizeShaderName(currentName)] = {preferredMap};
                    }
                    currentName.clear();
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
                continue;
            }
            if (depth == 2) {
                if (token == "tcGen" && index + 1 < tokens.size() &&
                    _stricmp(tokens[index + 1].c_str(), "environment") == 0) {
                    stageHasEnvironment = true;
                } else if (token == "map" || token == "clampmap") {
                    if (index + 1 < tokens.size()) {
                        stageMap = tokens[index + 1];
                    }
                } else if (token == "animMap" && index + 2 < tokens.size()) {
                    stageMap = tokens[index + 2];
                }
            }
        }
    }

}

QrMaterial GetMaterialForTextureName(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return defaultMaterial;
    }

    LoadShaderScripts();
    const std::string key = NormalizeShaderName(name);
    const auto existing = textureMaterials.find(key);
    if (existing != textureMaterials.end()) {
        return existing->second;
    }

    const auto shaderIt = shaderDefinitions.find(key);
    const char *textureName = shaderIt != shaderDefinitions.end() ? shaderIt->second.map.c_str() : name;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> albedo;
    QrMaterial material = QR_NO_MATERIAL;
    if (LoadQ3Texture(textureName, width, height, albedo)) {
        material = CreateMaterialFromPixels(textureName, width, height, albedo,
            QR_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    if (material == QR_NO_MATERIAL) {
        material = defaultMaterial;
    }
    textureMaterials.emplace(key, material);
    return material;
}

QrMaterial GetMaterialForHandle(qhandle_t handle)
{
    const auto it = uiMaterials.find(handle);
    return it != uiMaterials.end() ? it->second : defaultMaterial;
}

uint32_t PackColor(const color4ub_t &color)
{
    return static_cast<uint32_t>(color.rgba[0]) |
        (static_cast<uint32_t>(color.rgba[1]) << 8) |
        (static_cast<uint32_t>(color.rgba[2]) << 16) |
        (static_cast<uint32_t>(color.rgba[3]) << 24);
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

    int width = 0;
    int height = 0;
    std::vector<uint8_t> albedo;
    if (!LoadQ3Texture(shaderName, width, height, albedo)) {
        return defaultMaterial;
    }

    QrMaterial material = CreateMaterialFromPixels(shaderName, width, height, albedo,
        QR_SAMPLER_ADDRESS_MODE_REPEAT);
    if (material == QR_NO_MATERIAL) {
        return defaultMaterial;
    }

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

    const QrMaterial material = GetMaterialForTextureName(name);
    if (material != QR_NO_MATERIAL) {
        uiMaterials.emplace(handle, material);
    }
    return handle;
}

bool CreateQrayInstance()
{
    HostLog("CreateQrayInstance: start");
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

    QrInstanceCreateInfo info{};
    info.pAppName = "QuakeRay";
    info.pAppGUID = "6f6f4e42-2f8b-4d5f-9c8b-4a7b1c2d3e4f";
    info.pWin32SurfaceInfo = &surfaceInfo;
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

void UploadSunLight()
{
    float r = 1.0f;
    float g = 0.95f;
    float b = 0.85f;
    if (qraySunColor != nullptr) {
        std::sscanf(qraySunColor->string, "%f %f %f", &r, &g, &b);
    }

    const float intensity = CvarFloat(qraySunIntensity, 3.0f);

    QrDirectionalLightUploadInfo light{};
    light.uniqueID = 1;
    light.color.data[0] = r * intensity;
    light.color.data[1] = g * intensity;
    light.color.data[2] = b * intensity;
    light.direction.data[0] = 0.35f;
    light.direction.data[1] = 0.45f;
    light.direction.data[2] = -0.82f;
    light.angularDiameterDegrees = 1.0f;

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

    const float intensityScale = CvarFloat(qrayLightScale, 1600.0f);
    const float radiusScale = CvarFloat(qrayLightRadiusScale, 1.0f);

    for (size_t i = 0; i < mapLights.size(); ++i) {
        const MapLight &source = mapLights[i];
        QrSphericalLightUploadInfo light{};
        light.uniqueID = 1000 + i;
        light.color.data[0] = source.color[0] * intensityScale;
        light.color.data[1] = source.color[1] * intensityScale;
        light.color.data[2] = source.color[2] * intensityScale;
        light.position.data[0] = source.position[0];
        light.position.data[1] = source.position[1];
        light.position.data[2] = source.position[2];
        light.radius = std::max(1.0f, source.radius * radiusScale);

        const QrResult result = qrUploadSphericalLight(qrayInstance, &light);
        if (result != QR_SUCCESS) {
            HostLog("UploadMapLights: light %zu failed result=%d", i, static_cast<int>(result));
            break;
        }
    }
}

void UploadSceneLights()
{
    UploadSunLight();
    UploadMapLights();
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

float Bernstein(int degree, int index, float t)
{
    float coefficient = 1.0f;
    for (int i = 1; i <= index; ++i) {
        coefficient *= static_cast<float>(degree - index + i) / static_cast<float>(i);
    }
    return coefficient * std::pow(t, static_cast<float>(index)) *
        std::pow(1.0f - t, static_cast<float>(degree - index));
}

void EvaluatePatch(const drawVert_t *control, int width, int height,
    float u, float v, float outPosition[3], float outTexCoord[2])
{
    outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
    outTexCoord[0] = outTexCoord[1] = 0.0f;

    const int degreeU = width - 1;
    const int degreeV = height - 1;
    for (int y = 0; y < height; ++y) {
        const float basisV = Bernstein(degreeV, y, v);
        for (int x = 0; x < width; ++x) {
            const float basisU = Bernstein(degreeU, x, u);
            const float weight = basisU * basisV;
            const drawVert_t &point = control[y * width + x];
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

bool LoadBspGeometryInner(const char *name)
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return false;
    }

    HostLog("LoadBspGeometry: %s", name);
    for (int i = 0; i < 3; ++i) {
        worldMin[i] = 1.0e30f;
        worldMax[i] = -1.0e30f;
    }
    void *fileData = nullptr;
    int fileLength = ri.FS_ReadFile(name, &fileData);
    HostLog("LoadBspGeometry: FS_ReadFile len=%d data=%p", fileLength, fileData);
    if (fileLength <= 0 || fileData == nullptr) {
        ri.Printf(PRINT_WARNING, "rendererqray: cannot read %s\n", name);
        return false;
    }

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

            shaderMaterials.assign(static_cast<size_t>(shaderCount), QR_NO_MATERIAL);
            shaderMaterialAttempted.assign(static_cast<size_t>(shaderCount), 0);

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
                int uploaded = 0;
                std::vector<GeometryBatch> batches(static_cast<size_t>(shaderCount));
                for (int surfaceIndex = firstSurface; surfaceIndex < firstSurface + numSurfaces; ++surfaceIndex) {
                    const dsurface_t &surface = surfaces[surfaceIndex];
                    if (surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_TRIANGLE_SOUP &&
                        surface.surfaceType != MST_PATCH) {
                        continue;
                    }
                    if (surface.surfaceType == MST_PATCH) {
                        if (surface.patchWidth < 2 || surface.patchHeight < 2 ||
                            surface.patchWidth * surface.patchHeight > surface.numVerts) {
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
                    if ((shader.surfaceFlags & (SURF_NODRAW | SURF_SKY)) != 0) {
                        continue;
                    }

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
                                patchIndices.push_back(i0);
                                patchIndices.push_back(i2);
                                patchIndices.push_back(i1);
                                patchIndices.push_back(i1);
                                patchIndices.push_back(i2);
                                patchIndices.push_back(i3);
                            }
                        }

                        GeometryBatch &batch = batches[static_cast<size_t>(surface.shaderNum)];
                        batch.material = material;
                        const uint32_t baseVertex = static_cast<uint32_t>(batch.vertices.size());
                        batch.vertices.insert(batch.vertices.end(), patchVerts.begin(), patchVerts.end());
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
                    for (size_t i = 0; i + 2 < outIndices.size(); i += 3) {
                        std::swap(outIndices[i + 1], outIndices[i + 2]);
                    }

                    GeometryBatch &batch = batches[static_cast<size_t>(surface.shaderNum)];
                    batch.material = material;
                    const uint32_t baseVertex = static_cast<uint32_t>(batch.vertices.size());
                    batch.vertices.insert(batch.vertices.end(), outVerts.begin(), outVerts.end());
                    batch.indices.reserve(batch.indices.size() + outIndices.size());
                    for (uint32_t index : outIndices) {
                        batch.indices.push_back(baseVertex + index);
                    }
                    ++uploaded;
                }

                HostLog("LoadBspGeometry: upload loop done, uploaded=%d", uploaded);
                int uploadedBatches = 0;
                for (size_t batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
                    const GeometryBatch &batch = batches[batchIndex];
                    if (batch.vertices.empty() || batch.indices.empty()) {
                        continue;
                    }

                    QrGeometryUploadInfo upload{};
                    upload.uniqueID = static_cast<uint64_t>(batchIndex) + 1;
                    upload.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
                    upload.geomType = QR_GEOMETRY_TYPE_STATIC;
                    upload.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
                    upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
                    upload.vertexCount = static_cast<uint32_t>(batch.vertices.size());
                    upload.pVertices = batch.vertices.data();
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
                    } else {
                        HostLog("LoadBspGeometry: batch %zu upload failed result=%d", batchIndex, static_cast<int>(uploadResult));
                    }
                }
                const QrResult submitResult = qrSubmitStaticGeometries(qrayInstance);
                HostLog("LoadBspGeometry: qrSubmitStaticGeometries result=%d", static_cast<int>(submitResult));
                HostLog("LoadBspGeometry: bounds min=(%f %f %f) max=(%f %f %f)", worldMin[0], worldMin[1], worldMin[2], worldMax[0], worldMax[1], worldMax[2]);
                if (submitResult != QR_SUCCESS) {
                    ri.Printf(PRINT_WARNING, "rendererqray: qrSubmitStaticGeometries failed: %d\n", static_cast<int>(submitResult));
                }
                ri.Printf(PRINT_ALL, "rendererqray: uploaded %d world surfaces from %s\n", uploaded, name);
                loaded = uploaded > 0;
            }
        } else {
            HostLog("LoadBspGeometry: malformed BSP lumps in %s", name);
            ri.Printf(PRINT_WARNING, "rendererqray: malformed BSP lumps in %s\n", name);
        }
    } else {
        HostLog("LoadBspGeometry: %s is not a Quake 3 BSP", name);
        ri.Printf(PRINT_WARNING, "rendererqray: %s is not a Quake 3 BSP\n", name);
    }

    ri.FS_FreeFile(fileData);
    return loaded;
}

bool LoadBspGeometry(const char *name)
{
    __try {
        return LoadBspGeometryInner(name);
    } __except (HostExceptionFilter(GetExceptionInformation())) {
        return false;
    }
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
            readbackReadyPixels.assign(readbackBuffer.begin(), readbackBuffer.end());
            readbackValid = true;
            readbackPending = false;
        } else if (result == QR_READBACK_FAILED || result == QR_READBACK_INVALID) {
            ResetReadback();
        }
    }
}

void CopyReadbackToCapture()
{
    if (!capturePending || !readbackValid || captureBuffer == nullptr || readbackWidth == 0 || readbackHeight == 0) {
        return;
    }

    const int width = std::min<int>(captureWidth, static_cast<int>(readbackWidth));
    const int height = std::min<int>(captureHeight, static_cast<int>(readbackHeight));
    byte *dst = captureBuffer;
    const size_t dstPitch = static_cast<size_t>(width) * 3;
    const size_t srcPitch = readbackRowPitch;

    for (int y = 0; y < height; ++y) {
        const uint8_t *srcRow = readbackReadyPixels.data() + static_cast<size_t>(y) * srcPitch;
        for (int x = 0; x < width; ++x) {
            dst[0] = srcRow[x * 4 + 0];
            dst[1] = srcRow[x * 4 + 1];
            dst[2] = srcRow[x * 4 + 2];
            dst += 3;
        }
    }

    if (captureMotionJpeg && captureEncodeBuffer != nullptr) {
        const size_t encodedSize = ri.CL_SaveJPGToBuffer(captureEncodeBuffer, dstPitch * static_cast<size_t>(height), 85,
            width, height, captureBuffer, 0);
        if (encodedSize > 0) {
            ri.CL_WriteAVIVideoFrame(captureEncodeBuffer, static_cast<int>(encodedSize));
        }
    } else {
        ri.CL_WriteAVIVideoFrame(captureBuffer, static_cast<int>(dstPitch * static_cast<size_t>(height)));
    }

    capturePending = false;
}

void StartFrame()
{
    if (qrayInstance == QR_NULL_HANDLE) {
        return;
    }

    QrStartFrameInfo info{};
    info.presentMode = QR_PRESENT_MODE_MAILBOX;
    info.maxFrameLatency = 1;
    info.requestShaderReload = QR_FALSE;
    info.wantFullscreenExclusive = QR_FALSE;
    info.hasFocus = QR_TRUE;

    const QrResult result = qrStartFrame(qrayInstance, &info);
    if (result != QR_SUCCESS) {
        ri.Printf(PRINT_WARNING, "rendererqray: qrStartFrame failed: %d\n", static_cast<int>(result));
        return;
    }

    frameStarted = true;
    sceneRendered = false;
    sceneValid = false;
    frameHasWorldScene = false;

    if (!worldLoaded && !pendingWorldName.empty()) {
        LoadBspGeometry(pendingWorldName.c_str());
        worldLoaded = true;
    }

    UpdateReadback(true);
}

uint64_t NextDynamicGeometryId()
{
    return dynamicGeometryId++;
}

void UploadDynamicGeometry(const std::vector<QrVertex> &vertices, const std::vector<uint32_t> &indices,
    QrMaterial material, QrGeometryPrimaryVisibilityType visibility,
    QrGeometryPassThroughType passThrough, QrGeometryMaterialBlendType blend,
    float emission)
{
    if (qrayInstance == QR_NULL_HANDLE || vertices.empty() || indices.empty()) {
        return;
    }
    if (material == QR_NO_MATERIAL) {
        material = defaultMaterial;
    }

    QrGeometryUploadInfo upload{};
    upload.uniqueID = NextDynamicGeometryId();
    upload.flags = QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT;
    upload.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    upload.passThroughType = passThrough;
    upload.visibilityType = visibility;
    upload.vertexCount = static_cast<uint32_t>(vertices.size());
    upload.pVertices = vertices.data();
    upload.indexCount = static_cast<uint32_t>(indices.size());
    upload.pIndices = indices.data();
    upload.layerColors[0].data[0] = 1.0f;
    upload.layerColors[0].data[1] = 1.0f;
    upload.layerColors[0].data[2] = 1.0f;
    upload.layerColors[0].data[3] = 1.0f;
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

void UploadModelEntity(const refEntity_t &entity)
{
    const auto modelIt = modelAssets.find(entity.hModel);
    if (modelIt == modelAssets.end() || !modelIt->second.valid) {
        return;
    }
    const ModelAsset &asset = modelIt->second;

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
            destination.texCoord[0] = surface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 0];
            destination.texCoord[1] = surface.texCoords[static_cast<size_t>(vertexIndex) * 2 + 1];
            destination.packedColor = 0xFFFFFFFFu;
        }

        const QrMaterial material = ResolveModelMaterial(entity, asset, surface);
        std::vector<uint32_t> indices = surface.indices;
        if (qrayModelWinding != nullptr && qrayModelWinding->integer != 0) {
            for (size_t index = 0; index + 2 < indices.size(); index += 3) {
                std::swap(indices[index + 1], indices[index + 2]);
            }
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
    }

    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    const QrGeometryPrimaryVisibilityType visibility =
        (entity.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) != 0
        ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON
        : QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    UploadDynamicGeometry(vertices, indices, GetMaterialForHandle(entity.customShader), visibility,
        QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED, QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA, 1.0f);
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
    }

    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    UploadDynamicGeometry(vertices, indices, GetMaterialForHandle(entity.customShader),
        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0, QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
        QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD, 1.0f);
}

void UploadPolyGeometry(const PendingPoly &poly)
{
    if (poly.verts.size() < 3) {
        return;
    }

    std::vector<QrVertex> vertices(poly.verts.size());
    for (size_t vertexIndex = 0; vertexIndex < poly.verts.size(); ++vertexIndex) {
        const polyVert_t &source = poly.verts[vertexIndex];
        QrVertex &destination = vertices[vertexIndex];
        destination = {};
        destination.position[0] = source.xyz[0];
        destination.position[1] = source.xyz[1];
        destination.position[2] = source.xyz[2];
        destination.normal[0] = -sceneDef.viewaxis[0][0];
        destination.normal[1] = -sceneDef.viewaxis[0][1];
        destination.normal[2] = -sceneDef.viewaxis[0][2];
        destination.texCoord[0] = source.st[0];
        destination.texCoord[1] = source.st[1];
        destination.packedColor = PackColor(source.modulate);
    }

    std::vector<uint32_t> indices;
    indices.reserve((poly.verts.size() - 2) * 6);
    for (size_t vertexIndex = 0; vertexIndex + 2 < poly.verts.size(); ++vertexIndex) {
        indices.push_back(0);
        indices.push_back(static_cast<uint32_t>(vertexIndex + 1));
        indices.push_back(static_cast<uint32_t>(vertexIndex + 2));
        indices.push_back(0);
        indices.push_back(static_cast<uint32_t>(vertexIndex + 2));
        indices.push_back(static_cast<uint32_t>(vertexIndex + 1));
    }
    UploadDynamicGeometry(vertices, indices, GetMaterialForHandle(poly.shader),
        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0, QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
        QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA, 1.0f);
}

void UploadPendingSceneGeometry()
{
    if (qrayInstance == QR_NULL_HANDLE || !frameStarted) {
        pendingEntities.clear();
        pendingPolys.clear();
        return;
    }

    for (const refEntity_t &entity : pendingEntities) {
        if ((entity.renderfx & RF_THIRD_PERSON) != 0) {
            continue;
        }
        switch (entity.reType) {
        case RT_MODEL:
            UploadModelEntity(entity);
            break;
        case RT_SPRITE:
            UploadSpriteEntity(entity);
            break;
        case RT_BEAM:
        case RT_RAIL_CORE:
        case RT_LIGHTNING:
            UploadBeamEntity(entity);
            break;
        case RT_POLY:
        case RT_RAIL_RINGS:
        case RT_PORTALSURFACE:
            break;
        default:
            break;
        }
    }
    for (const PendingPoly &poly : pendingPolys) {
        UploadPolyGeometry(poly);
    }

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

    QrDrawFrameSkyParams sky{};
    sky.skyType = QR_SKY_TYPE_COLOR;
    sky.skyColorDefault.data[0] = 0.20f;
    sky.skyColorDefault.data[1] = 0.25f;
    sky.skyColorDefault.data[2] = 0.35f;
    sky.skyColorMultiplier = 1.0f;
    sky.skyColorSaturation = 1.0f;
    sky.skyAmbientLod = 0.0f;
    sky.skyLightMultiplier = 1.0f;
    sky.skyNee = QR_TRUE;
    sky.skyViewerPosition.data[0] = sceneDef.vieworg[0];
    sky.skyViewerPosition.data[1] = sceneDef.vieworg[1];
    sky.skyViewerPosition.data[2] = sceneDef.vieworg[2];
    sky.godRaysEnabled = QR_FALSE;

    QrDrawFrameInfo info{};
    BuildViewMatrix(sceneDef, info.view);
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
    info.cameraNear = 0.1f;
    info.cameraFar = 8192.0f;
    info.rayLength = 10000.0f;
    info.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT |
        QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT |
        QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
    info.disableRayTracedGeometry = QR_FALSE;
    info.disableRasterization = QR_FALSE;
    info.currentTime = static_cast<double>(sceneDef.time) * 0.001;
    info.disableEyeAdaptation = QR_FALSE;
    info.forceAntiFirefly = QR_TRUE;
    info.pRenderResolutionParams = &resolution;
    info.pSkyParams = &sky;
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
    ResetReadback();
    capturePending = false;

    if (qrayInstance != QR_NULL_HANDLE) {
        qrDestroyInstance(qrayInstance);
        qrayInstance = QR_NULL_HANDLE;
    }

    if (code == REF_UNLOAD_DLL && commandsRegistered) {
        ri.Cmd_RemoveCommand("screenshot");
        ri.Cmd_RemoveCommand("screenshotJPEG");
        ri.Cmd_RemoveCommand("qray_readback");
        commandsRegistered = false;
    }

    defaultMaterial = QR_NO_MATERIAL;
    frameStarted = false;
    sceneValid = false;

    if (code != REF_KEEP_CONTEXT && code != REF_KEEP_WINDOW) {
        ri.VKimp_Shutdown(code == REF_UNLOAD_DLL ? qtrue : qfalse);
    }
}

void BeginRegistration(glconfig_t *config)
{
    HostLog("BeginRegistration: entry config=%p", config);
    RegisterCvars();

    if (config != nullptr) {
        if (qrayInstance == QR_NULL_HANDLE) {
            HostLog("BeginRegistration: VKimp_Init");
            ri.VKimp_Init(config);
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
        HostLog("BeginRegistration: CreateDefaultMaterial");
        CreateDefaultMaterial();
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
        commandsRegistered = true;
    }
}

void EndRegistration()
{
    HostLog("EndRegistration");
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

    const QrResult result = qrRequestScreenshot(qrayInstance, name);
    if (result == QR_SUCCESS) {
        ri.Printf(PRINT_ALL, "rendererqray: requested screenshot %s\n", name);
    } else {
        ri.Printf(PRINT_WARNING, "rendererqray: screenshot request failed: %d\n", static_cast<int>(result));
    }
}

void QrayReadbackDump_f()
{
    if (!readbackValid || readbackWidth == 0 || readbackHeight == 0 ||
        readbackReadyPixels.size() < static_cast<size_t>(readbackWidth) * readbackHeight * 4) {
        ri.Printf(PRINT_WARNING, "rendererqray: no completed readback frame available\n");
        return;
    }

    const uint32_t width = readbackWidth;
    const uint32_t height = readbackHeight;
    const uint32_t imageSize = width * height * 4;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = static_cast<LONG>(width);
    infoHeader.biHeight = -static_cast<LONG>(height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageSize;

    const char *name = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv(1) : "qray-readback.bmp";
    std::ofstream stream(name, std::ios::binary);
    if (!stream.is_open()) {
        ri.Printf(PRINT_WARNING, "rendererqray: cannot write %s\n", name);
        return;
    }

    stream.write(reinterpret_cast<const char *>(&fileHeader), sizeof(fileHeader));
    stream.write(reinterpret_cast<const char *>(&infoHeader), sizeof(infoHeader));
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = readbackReadyPixels.data() + static_cast<size_t>(y) * readbackRowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t bgra[4] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0], row[x * 4 + 3]};
            stream.write(reinterpret_cast<const char *>(bgra), 4);
        }
    }

    ri.Printf(PRINT_ALL, "rendererqray: wrote %s (%ux%u)\n", name, width, height);
}

void LoadWorld(const char *name)
{
    HostLog("LoadWorld: name=%s instance=%p", name != nullptr ? name : "(null)", qrayInstance);
    if (name == nullptr || name[0] == '\0' || qrayInstance == QR_NULL_HANDLE) {
        return;
    }
    pendingWorldName = name;
    worldLoaded = false;
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

void AddLightToScene(const vec3_t org, float intensity, float r, float g, float b)
{
    (void)org;
    (void)intensity;
    (void)r;
    (void)g;
    (void)b;
}

void AddAdditiveLightToScene(const vec3_t org, float intensity, float r, float g, float b)
{
    AddLightToScene(org, intensity, r, g, b);
}

void AddLinearLightToScene(const vec3_t start, const vec3_t end, float intensity, float r, float g, float b)
{
    (void)start;
    (void)end;
    (void)intensity;
    (void)r;
    (void)g;
    (void)b;
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
    if (qrayInstance == QR_NULL_HANDLE || w <= 0.0f || h <= 0.0f) {
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

int MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
    vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer)
{
    (void)numPoints;
    (void)points;
    (void)projection;
    (void)maxPoints;
    (void)pointBuffer;
    (void)maxFragments;
    (void)fragmentBuffer;
    return 0;
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
    CopyReadbackToCapture();
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
                    for (uint64_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
                        const md3Triangle_t &triangle = triangles[triangleIndex];
                        modelSurface.indices.push_back(triangle.indexes[0]);
                        modelSurface.indices.push_back(triangle.indexes[1]);
                        modelSurface.indices.push_back(triangle.indexes[2]);
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
