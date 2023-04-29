// Interfaces
#include "../../TheForge/Utilities/Interfaces/ILog.h"
#include "../../TheForge/Utilities/Interfaces/ITime.h"
#include "../../TheForge/Game/Interfaces/IScripting.h"
#include "../../TheForge/Application/Interfaces/IUI.h"
#include "../../TheForge/Application/Interfaces/IApp.h"
#include "../../TheForge/Application/Interfaces/IFont.h"
#include "../../TheForge/Application/Interfaces/IInput.h"
#include "../../TheForge/Application/Interfaces/IProfiler.h"
#include "../../TheForge/Utilities/Interfaces/IFileSystem.h"
#include "../../TheForge/Application/Interfaces/IScreenshot.h"
#include "../../TheForge/Application/Interfaces/ICameraController.h"

// Renderer
#include "../../TheForge/Graphics/Interfaces/IGraphics.h"
#include "../../TheForge/Resources/ResourceLoader/Interfaces/IResourceLoader.h"

// Math
#include "../../TheForge/Utilities/Math/MathTypes.h"
#include "../../TheForge/Utilities/Interfaces/IMemory.h"

// Miscs
#include <inttypes.h>

// Shader commons
#define NO_FSL_DEFINITIONS
#include "Shaders/Headers/Shared.h"

// Handy macros
#define COLOR_SLIDER(VARIABLE, UB)                                                                            \
do                                                                                                            \
{                                                                                                             \
    ColorSliderWidget colorUI;                                                                                \
    colorUI.pData = & ## UB ## . ## VARIABLE;                                                                 \
    UIWidget* VARIABLE = uiCreateComponentWidget(pGuiWindow, #VARIABLE, &colorUI, WIDGET_TYPE_COLOR_SLIDER);  \
    VARIABLE->pOnEdited = [](void*) { resetHistory(); };                                                      \
} while (0)                                                                                                   \
/**/


// Globals
uint32_t       gFrameIndex = 0;
const uint32_t gImageCount = 3;
bool           gIsPaused = false;
uint64_t       gTotalFrameCounter = 0;
bool           gTakeScreenshot = false;
bool           gCurrentPathtraceIndex = false;
uint32_t       gPathtraceRootConstantIndex = 0;


// Shaders and pipelines
Shader*   pTonemapShader = NULL;
Pipeline* pTonemapPipeline = NULL;

Shader*   pResolveShader = NULL;
Pipeline* pResolvePipeline = NULL;

Shader*   pPathtraceShader = NULL;
Pipeline* pPathtracePipeline = NULL;


// Pipeline resources
Sampler*       pSampler = NULL;
DescriptorSet* pTonemapDS_None = { NULL };
RootSignature* pTonemapRootSignature = NULL;

DescriptorSet* pResolveDS_None = { NULL };
DescriptorSet* pResolveDS_PerFrame = { NULL };
RootSignature* pResolveRootSignature = NULL;

DescriptorSet* pPathtraceDS_None = { NULL };
DescriptorSet* pPathtraceDS_PerFrame = { NULL };
RootSignature* pPathtraceRootSignature = NULL;


// CPU data
FrameUB       gFrameUB = {};
uint32_t      gActiveMaterialIndex = 0;
QuadObject    gQuads[MAX_QUADS_COUNT];
SphereObject  gSpheres[MAX_SPHERES_COUNT];
Material      gMaterials[MAX_MATERIAL_COUNT];
RootConstants gRootConstants = { int4(MAX_SAMPLE_PER_PIXEL, MAX_RAY_BOUNCE, 0, 0) };


// Constant buffers
Buffer* pFrameUB[gImageCount] = { NULL };


// RWBuffers
Buffer* pQuadsBuffer[gImageCount] = { NULL };
Buffer* pSpheresBuffer[gImageCount] = { NULL };
Buffer* pMaterialsBuffer[gImageCount] = { NULL };


// RWTextures
Texture* pPathtraceTextures[2] = { NULL };


// Rendering
Renderer*  pRenderer = NULL;
SwapChain* pSwapChain = NULL;
Queue*     pGraphicsQueue = NULL;
Cmd*       pCmds[gImageCount] = { NULL };
CmdPool*   pCmdPools[gImageCount] = { NULL };
Semaphore* pImageAcquiredSemaphore = NULL;
Fence*     pRenderCompleteFences[gImageCount] = { NULL };
Semaphore* pRenderCompleteSemaphores[gImageCount] = { NULL };


// Forge miscs
uint32_t           gFontID = 0;
FontDrawDesc       gFrameTimeDraw;
UIComponent*       pGuiWindow = NULL;
ICameraController* pCameraController = NULL;
ProfileToken       gGpuProfileToken = PROFILE_INVALID_TOKEN;


// Configs
unsigned char  gTextBuf[32];
uint32_t       gFrameNumber = 0;
float2         gSunLightRot = float2(0, 0);
float4         gCommonTextColor = float4(1);
bstring        gFrameNumberStr = bemptyfromarr(gTextBuf);

void resetHistory()
{
    gFrameNumber = 0;
}


class PathTracingApp : public IApp
{
public:
    const char* GetName() { return "Software Path Tracing"; }

    bool Init()
    {
        // Default FrameUB
        {
            gFrameUB.sunFocus = 1500;
            gFrameUB.sunIntensity = 200;
            gFrameUB.skyHorizonColor = float4(1);
            gFrameUB.skyMode = SkyModeEnum_ExpensiveAtmosphere;
            gFrameUB.groundColor = float4(0.35f, 0.3f, 0.35f, 1.f);
            gFrameUB.skyZenithColor = float4(0.079f, 0.36f, 0.73f, 1.f);
        }

        // File system
        {
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "GPUCfg");
            fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_SCREENSHOTS, "Screenshots");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES, "Shaders");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_BINARIES, "CompiledShaders");
        }

        // Rendering
        {
            // window and renderer setup
            RendererDesc settings;
            memset(&settings, 0, sizeof(settings));
            settings.mGLESSupported = false;
            settings.mD3D11Supported = false;
            initRenderer(GetName(), &settings, &pRenderer);

            // Check for init success
            if (!pRenderer)
                return false;

            QueueDesc queueDesc = {};
            queueDesc.mType = QUEUE_TYPE_GRAPHICS;
            queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
            addQueue(pRenderer, &queueDesc, &pGraphicsQueue);

            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                CmdPoolDesc cmdPoolDesc = {};
                cmdPoolDesc.pQueue = pGraphicsQueue;
                addCmdPool(pRenderer, &cmdPoolDesc, &pCmdPools[i]);
                CmdDesc cmdDesc = {};
                cmdDesc.pPool = pCmdPools[i];
                addCmd(pRenderer, &cmdDesc, &pCmds[i]);

                addFence(pRenderer, &pRenderCompleteFences[i]);
                addSemaphore(pRenderer, &pRenderCompleteSemaphores[i]);
            }
            addSemaphore(pRenderer, &pImageAcquiredSemaphore);

            initScreenshotInterface(pRenderer, pGraphicsQueue);

            initResourceLoaderInterface(pRenderer);
        }

        // Constant buffers
        {
            BufferLoadDesc ubDesc = {};
            ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
            ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
            ubDesc.pData = NULL;
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                ubDesc.mDesc.mSize = sizeof(FrameUB);
                ubDesc.ppBuffer = &pFrameUB[i];
                addResource(&ubDesc, NULL);
            }
        }

        // Structure buffers
        {
            // Spheres
            BufferLoadDesc bufferDesc = {};
            bufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
            bufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
            bufferDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_NONE;
            bufferDesc.mDesc.mFirstElement = 0;
            bufferDesc.mDesc.mElementCount = MAX_SPHERES_COUNT;
            bufferDesc.mDesc.mStructStride = sizeof(SphereObject);
            bufferDesc.mDesc.mSize = MAX_SPHERES_COUNT * bufferDesc.mDesc.mStructStride;
            bufferDesc.pData = NULL;

            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                bufferDesc.ppBuffer = &pSpheresBuffer[i];
                addResource(&bufferDesc, NULL);
            }

            // Quads
            bufferDesc.mDesc.mElementCount = MAX_QUADS_COUNT;
            bufferDesc.mDesc.mStructStride = sizeof(QuadObject);
            bufferDesc.mDesc.mSize = MAX_QUADS_COUNT * bufferDesc.mDesc.mStructStride;
            bufferDesc.pData = NULL;
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                bufferDesc.ppBuffer = &pQuadsBuffer[i];
                addResource(&bufferDesc, NULL);
            }

            // Materials
            bufferDesc.mDesc.mElementCount = MAX_MATERIAL_COUNT;
            bufferDesc.mDesc.mStructStride = sizeof(Material);
            bufferDesc.mDesc.mSize = MAX_MATERIAL_COUNT * bufferDesc.mDesc.mStructStride;
            bufferDesc.pData = NULL;
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                bufferDesc.ppBuffer = &pMaterialsBuffer[i];
                addResource(&bufferDesc, NULL);
            }
        }

        // Forge UI and Profiler
        {
            FontDesc font = {};
            font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
            fntDefineFonts(&font, 1, &gFontID);

            FontSystemDesc fontRenderDesc = {};
            fontRenderDesc.pRenderer = pRenderer;
            if (!initFontSystem(&fontRenderDesc))
                return false; // report?

            // Initialize Forge User Interface Rendering
            UserInterfaceDesc uiRenderDesc = {};
            uiRenderDesc.pRenderer = pRenderer;
            initUserInterface(&uiRenderDesc);

            // Initialize micro profiler and its UI.
            ProfilerDesc profiler = {};
            profiler.pRenderer = pRenderer;
            profiler.mWidthUI = mSettings.mWidth;
            profiler.mHeightUI = mSettings.mHeight;
            initProfiler(&profiler);

            // Gpu profiler can only be added after initProfile.
            gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "GraphicsQueue");
        }

        // App UI
        {
            UIComponentDesc guiDesc = {};
            guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.8f, mSettings.mHeight * 0.01f);
            uiCreateComponent(GetName(), &guiDesc, &pGuiWindow);

            CheckboxWidget takeScreenshotUI;
            takeScreenshotUI.pData = &gTakeScreenshot;
            uiCreateComponentWidget(pGuiWindow, "Capture All Frames", &takeScreenshotUI, WIDGET_TYPE_CHECKBOX);

            CheckboxWidget pauseUI;
            pauseUI.pData = &gIsPaused;
            uiCreateComponentWidget(pGuiWindow, "Pause", &pauseUI, WIDGET_TYPE_CHECKBOX);

            // Vertical separator
            VerticalSeparatorWidget separator;
            separator.mLineCount = 1;
            uiCreateComponentWidget(pGuiWindow, "", &separator, WIDGET_TYPE_VERTICAL_SEPARATOR);

            DynamicTextWidget frameNumberUI;
            frameNumberUI.pText = &gFrameNumberStr;
            frameNumberUI.pColor = &gCommonTextColor;
            uiCreateComponentWidget(pGuiWindow, "", &frameNumberUI, WIDGET_TYPE_DYNAMIC_TEXT);

            // Vertical separator
            uiCreateComponentWidget(pGuiWindow, "", &separator, WIDGET_TYPE_VERTICAL_SEPARATOR);

            SliderIntWidget samplePerPixelUI;
            samplePerPixelUI.pData = &gRootConstants.args.x;
            samplePerPixelUI.mMin = 1;
            samplePerPixelUI.mMax = MAX_SAMPLE_PER_PIXEL;
            uiCreateComponentWidget(pGuiWindow, "Sample Per Pixel", &samplePerPixelUI, WIDGET_TYPE_SLIDER_INT);

            SliderIntWidget maxBounceUI;
            maxBounceUI.pData = &gRootConstants.args.y;
            maxBounceUI.mMin = 2;
            maxBounceUI.mMax = MAX_RAY_BOUNCE;
            uiCreateComponentWidget(pGuiWindow, "Max Bounce", &maxBounceUI, WIDGET_TYPE_SLIDER_INT);

            // Sky
            {
                // Vertical separator
                uiCreateComponentWidget(pGuiWindow, "", &separator, WIDGET_TYPE_VERTICAL_SEPARATOR);

                static const char* enumNames[] = { "None", "Skybox (Very Simple)", "Skybox (Simple)", "Atmosphere (Expensive)" };
                DropdownWidget skyModeUI;
                skyModeUI.pData = &gFrameUB.skyMode;
                skyModeUI.pNames = enumNames;
                skyModeUI.mCount = ARRAYSIZE(enumNames);
                UIWidget* pWidget = uiCreateComponentWidget(pGuiWindow, "Sky Mode", &skyModeUI, WIDGET_TYPE_DROPDOWN);
                pWidget->pOnEdited = [](void*) { resetHistory(); };

                SliderFloat2Widget sunLightXUI;
                sunLightXUI.pData = &gSunLightRot;
                sunLightXUI.mMin = float2(- PI);
                sunLightXUI.mMax = float2(PI);
                sunLightXUI.mStep = float2(1e-8f);
                pWidget = uiCreateComponentWidget(pGuiWindow, "Sun Light", &sunLightXUI, WIDGET_TYPE_SLIDER_FLOAT2);
                pWidget->pOnEdited = [](void*) { resetHistory(); };

                SliderFloatWidget sunFocusUI;
                sunFocusUI.pData = &gFrameUB.sunFocus;
                sunFocusUI.mMin = 1e-6f;
                sunFocusUI.mMax = 5'000;
                pWidget = uiCreateComponentWidget(pGuiWindow, "Sun Focus", &sunFocusUI, WIDGET_TYPE_SLIDER_FLOAT);
                pWidget->pOnEdited = [](void*) { resetHistory(); };

                SliderFloatWidget sunIntensityUI;
                sunIntensityUI.pData = &gFrameUB.sunIntensity;
                sunIntensityUI.mMin = 1e-6f;
                sunIntensityUI.mMax = 5'000;
                pWidget = uiCreateComponentWidget(pGuiWindow, "Sun Intensity", &sunIntensityUI, WIDGET_TYPE_SLIDER_FLOAT);
                pWidget->pOnEdited = [](void*) { resetHistory(); };

                COLOR_SLIDER(groundColor, gFrameUB);
                COLOR_SLIDER(skyZenithColor, gFrameUB);
                COLOR_SLIDER(skyHorizonColor, gFrameUB);
            }
        }

        waitForAllResourceLoads();

        // Camera
        {
            vec3 lookAt{ vec3(0.f, 0.f, 0.f) };
            // vec3 camPos{ 0.f, 8.f, 30.f };
            vec3 camPos{ 0.0f, 0.0f, -20.0f };

            CameraMotionParameters cmp{ 100.f, 200.f, 300.f };

            pCameraController = initFpsCameraController(camPos, lookAt);
            pCameraController->setMotionParameters(cmp);
        }

        // Input system
        {
            InputSystemDesc inputDesc = {};
            inputDesc.pRenderer = pRenderer;
            inputDesc.pWindow = pWindow;
            if (!initInputSystem(&inputDesc))
                return false;
        }

        // App actions
        {
            InputActionDesc  actionDesc = { DefaultInputActions::DUMP_PROFILE_DATA, [](InputActionContext* ctx) {  dumpProfileData(((Renderer*)ctx->pUserData)->pName); return true; }, pRenderer };
            addInputAction(&actionDesc);
            actionDesc = { DefaultInputActions::TOGGLE_FULLSCREEN, [](InputActionContext* ctx) { toggleFullscreen(((IApp*)ctx->pUserData)->pWindow); return true; }, this };
            addInputAction(&actionDesc);
            actionDesc = { DefaultInputActions::EXIT, [](InputActionContext* ctx) { requestShutdown(); return true; } };
            addInputAction(&actionDesc);
            InputActionCallback onAnyInput = [](InputActionContext* ctx)
            {
                if (ctx->mActionId > UISystemInputActions::UI_ACTION_START_ID_)
                {
                    uiOnInput(ctx->mActionId, ctx->mBool, ctx->pPosition, &ctx->mFloat2);
                }

                return true;
            };

            typedef bool(*CameraInputHandler)(InputActionContext* ctx, uint32_t index);
            static CameraInputHandler onCameraInput = [](InputActionContext* ctx, uint32_t index)
            {
                if (*(ctx->pCaptured))
                {
                    float2 delta = uiIsFocused() ? float2(0.f, 0.f) : ctx->mFloat2;
                    index ? pCameraController->onRotate(delta) : pCameraController->onMove(delta);
                }
                return true;
            };
            actionDesc = { DefaultInputActions::CAPTURE_INPUT, [](InputActionContext* ctx) {setEnableCaptureInput(!uiIsFocused() && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);	return true; }, NULL };
            addInputAction(&actionDesc);
            actionDesc = { DefaultInputActions::ROTATE_CAMERA, [](InputActionContext* ctx) { return onCameraInput(ctx, 1); }, NULL };
            addInputAction(&actionDesc);
            actionDesc = { DefaultInputActions::TRANSLATE_CAMERA, [](InputActionContext* ctx) { return onCameraInput(ctx, 0); }, NULL };
            addInputAction(&actionDesc);
            actionDesc = { DefaultInputActions::RESET_CAMERA, [](InputActionContext* ctx) { if (!uiWantTextInput()) pCameraController->resetView(); return true; } };
            addInputAction(&actionDesc);

            GlobalInputActionDesc globalInputActionDesc = { GlobalInputActionDesc::ANY_BUTTON_ACTION, onAnyInput, this };
            setGlobalInputAction(&globalInputActionDesc);
        }

        gFrameIndex = 0;

        return true;
    }

    void Exit()
    {
        exitInputSystem();

        exitCameraController(pCameraController);

        exitUserInterface();

        exitFontSystem();

        exitProfiler();

        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            removeResource(pFrameUB[i]);
            removeResource(pQuadsBuffer[i]);
            removeResource(pSpheresBuffer[i]);
            removeResource(pMaterialsBuffer[i]);
        }

        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            removeFence(pRenderer, pRenderCompleteFences[i]);
            removeSemaphore(pRenderer, pRenderCompleteSemaphores[i]);

            removeCmd(pRenderer, pCmds[i]);
            removeCmdPool(pRenderer, pCmdPools[i]);
        }
        removeSemaphore(pRenderer, pImageAcquiredSemaphore);

        exitResourceLoaderInterface(pRenderer);

        exitScreenshotInterface();

        removeQueue(pRenderer, pGraphicsQueue);

        exitRenderer(pRenderer);
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        gCurrentPathtraceIndex = false;
        resetHistory();

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            // Set the scene
            {
                gActiveMaterialIndex = 0;
                gRootConstants.args.setZ(0); // reset active spheres index.
                gRootConstants.args.setW(0); // reset active quads index.

                // addFirstScene();
                addCornellBox();
                // addOneWeekendScene();
            }

            addShaders();
            addRootSignatures();
            addDescriptorSets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            if (!addSwapChain())
                return false;

            if (!addPathtraceTextures())
                return false;
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            addPipelines();
        }

        prepareDescriptorSets();

        UserInterfaceLoadDesc uiLoad = {};
        uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        uiLoad.mHeight = mSettings.mHeight;
        uiLoad.mWidth = mSettings.mWidth;
        uiLoad.mLoadType = pReloadDesc->mType;
        loadUserInterface(&uiLoad);

        FontSystemLoadDesc fontLoad = {};
        fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        fontLoad.mHeight = mSettings.mHeight;
        fontLoad.mWidth = mSettings.mWidth;
        fontLoad.mLoadType = pReloadDesc->mType;
        loadFontSystem(&fontLoad);

        return true;
    }

    void Unload(ReloadDesc* pReloadDesc)
    {
        waitQueueIdle(pGraphicsQueue);

        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            removePipelines();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            for (uint32_t i = 0; i < 2; ++i)
            {
                removeResource(pPathtraceTextures[i]);
            }

            removeSwapChain(pRenderer, pSwapChain);
        }

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeDescriptorSets();
            removeRootSignatures();
            removeSampler(pRenderer, pSampler);
            removeShaders();
        }
    }

    void Update(float deltaTime)
    {
        updateInputSystem(deltaTime, mSettings.mWidth, mSettings.mHeight);

        pCameraController->update(deltaTime);

        // Update frame data
        {
            // Update sun light direction
            {
                mat4 rotation = mat4::rotationXY(gSunLightRot.x, gSunLightRot.y);
                vec3 newLightDir = vec4((inverse(rotation) * vec4(0, 0, 1, 0))).getXYZ() * -1.f;

                gFrameUB.sunLightDir = float4(v3ToF3(normalize(newLightDir + vec3(1e-7f))), 0.0f);
            }

            // Update camera and projection
            vec2 camRot = pCameraController->getRotationXY();
            vec3 camPos = pCameraController->getViewPosition();
            {
                mat4 viewMat = pCameraController->getViewMatrix();

                const float horizontalFov = PI / 2.f;
                const float aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
                CameraMatrix projMat = CameraMatrix::perspective(horizontalFov, aspectInverse, 1000.f, 0.1f);

                gFrameUB.viewProj = projMat.getPrimaryMatrix() * viewMat;
                gFrameUB.invViewProj = inverse(gFrameUB.viewProj);
                gFrameUB.screenSize = uint2(mSettings.mWidth, mSettings.mHeight);
                gFrameUB.invScreenSize = float2(1.f / (float)gFrameUB.screenSize[0], 1.f / (float)gFrameUB.screenSize[1]);
            }

            // Reset history if camera has moved.
            {
                const float threshold = 1e-10f;
                static vec2 camPrevRotation = vec2(0);
                vec2 rotDiff = camPrevRotation - camRot;

                if ( ( length(f3Tov3(gFrameUB.camPos.getXYZ()) - camPos) > threshold ) || ( dot(rotDiff, rotDiff) > threshold ) )
                {
                    resetHistory();
                    camPrevRotation = camRot;
                    gFrameUB.camPos = float4(v3ToF3(camPos), 1);
                }
            }

            // Update history and frame number.
            {
                gFrameUB.frameNumber = gFrameNumber;
                gFrameUB.historyWeight = 1.f / (gFrameUB.frameNumber + 1);
                
                bformat(&gFrameNumberStr, "Frame: %d", gFrameNumber++);
            }
        }
    }

    void Draw()
    {
        if (pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
        Semaphore* pRenderCompleteSemaphore = pRenderCompleteSemaphores[gFrameIndex];
        Fence* pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];

        // Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
        FenceStatus fenceStatus;
        getFenceStatus(pRenderer, pRenderCompleteFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &pRenderCompleteFence);

        // Update uniform buffers
        {
            BufferUpdateDesc frameCbv = { pFrameUB[gFrameIndex] };
            beginUpdateResource(&frameCbv);
            *(FrameUB*)frameCbv.pMappedData = gFrameUB;
            endUpdateResource(&frameCbv, NULL);

            BufferUpdateDesc sphereRWBuffer = { pSpheresBuffer[gFrameIndex] };
            beginUpdateResource(&sphereRWBuffer);
            memcpy(sphereRWBuffer.pMappedData, gSpheres, sizeof(SphereObject) * MAX_SPHERES_COUNT);
            endUpdateResource(&sphereRWBuffer, NULL);

            BufferUpdateDesc quadRWBuffer = { pQuadsBuffer[gFrameIndex] };
            beginUpdateResource(&quadRWBuffer);
            memcpy(quadRWBuffer.pMappedData, gQuads, sizeof(QuadObject) * MAX_QUADS_COUNT);
            endUpdateResource(&quadRWBuffer, NULL);

            BufferUpdateDesc materialRWBuffer = { pMaterialsBuffer[gFrameIndex] };
            beginUpdateResource(&materialRWBuffer);
            memcpy(materialRWBuffer.pMappedData, gMaterials, sizeof(Material) * MAX_MATERIAL_COUNT);
            endUpdateResource(&materialRWBuffer, NULL);
        }

        // Reset cmd pool for this frame
        resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

        Cmd* cmd = pCmds[gFrameIndex];
        beginCmd(cmd);

        cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

        Texture* prevPathtraceTex = pPathtraceTextures[!gCurrentPathtraceIndex];
        Texture* currPathtraceTex = pPathtraceTextures[gCurrentPathtraceIndex];

        // Compute passes
        if (!gIsPaused)
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Compute - Pathtrace");
            {
                cmdBindPipeline(cmd, pPathtracePipeline);
                cmdBindDescriptorSet(cmd, gCurrentPathtraceIndex, pPathtraceDS_None);
                cmdBindDescriptorSet(cmd, gFrameIndex, pPathtraceDS_PerFrame);
                cmdBindPushConstants(cmd, pPathtraceRootSignature, gPathtraceRootConstantIndex, &gRootConstants);

                uint32_t* threadGroupSize = pPathtraceShader->pReflection->mStageReflections[0].mNumThreadsPerGroup;
                uint32_t groupCountX = (pRenderTarget->mWidth + threadGroupSize[0] - 1) / threadGroupSize[0];
                uint32_t groupCountY = (pRenderTarget->mHeight + threadGroupSize[1] - 1) / threadGroupSize[1];

                cmdDispatch(cmd, groupCountX, groupCountY, 1);
            }
            cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
        }

        // texture barriers
        {
            TextureBarrier textureBarriers[] =
            {
                { prevPathtraceTex, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            };
            cmdResourceBarrier(cmd, 0, NULL, ARRAYSIZE(textureBarriers), textureBarriers, 0, NULL);
        }
        
        if (!gIsPaused)
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Compute - Resolve");
            {
                cmdBindPipeline(cmd, pResolvePipeline);
                cmdBindDescriptorSet(cmd, gCurrentPathtraceIndex, pResolveDS_None);
                cmdBindDescriptorSet(cmd, gFrameIndex, pResolveDS_PerFrame);

                uint32_t* threadGroupSize = pResolveShader->pReflection->mStageReflections[0].mNumThreadsPerGroup;
                uint32_t groupCountX = (pRenderTarget->mWidth + threadGroupSize[0] - 1) / threadGroupSize[0];
                uint32_t groupCountY = (pRenderTarget->mHeight + threadGroupSize[1] - 1) / threadGroupSize[1];

                cmdDispatch(cmd, groupCountX, groupCountY, 1);
            }
            cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
        }

        // Graphics passes
        {
            // Tonemap
            {
                TextureBarrier textureBarriers[] =
                {
                    { currPathtraceTex, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
                };
                RenderTargetBarrier rtBarriers[] =
                {
                    { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
                };
                cmdResourceBarrier(cmd, 0, NULL, ARRAYSIZE(textureBarriers), textureBarriers, ARRAYSIZE(rtBarriers), rtBarriers);

                LoadActionsDesc loadActions = {};
                loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
                cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);
                cmdSetViewport(cmd, 0.f, 0.f, (float)mSettings.mWidth, (float)mSettings.mHeight, 0.f, 1.f);
                cmdSetScissor(cmd, 0, 0, mSettings.mWidth, mSettings.mHeight);

                cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Graphics - Tonemap");
                cmdBindPipeline(cmd, pTonemapPipeline);
                cmdBindDescriptorSet(cmd, gCurrentPathtraceIndex, pTonemapDS_None);

                cmdDraw(cmd, 3, 0);
                cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
            }

            // UI and submit
            {
                LoadActionsDesc loadActions = {};
                loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
                cmdBindRenderTargets(cmd, 1, &pRenderTarget, nullptr, &loadActions, NULL, NULL, -1, -1);
                cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Graphics - Draw UI");

                gFrameTimeDraw.mFontColor = 0xff00ffff;
                gFrameTimeDraw.mFontSize = 18.f;
                gFrameTimeDraw.mFontID = gFontID;
                float2 txtSizePx = cmdDrawCpuProfile(cmd, float2(8.f, 10.f), &gFrameTimeDraw);
                cmdDrawGpuProfile(cmd, float2(8.f, txtSizePx.y + 75.f), gGpuProfileToken, &gFrameTimeDraw);

                cmdDrawUserInterface(cmd);

                cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
                cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

                TextureBarrier textureBarriers[] =
                {
                    { prevPathtraceTex, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
                    { currPathtraceTex, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
                };
                RenderTargetBarrier rtBarriers[] =
                {
                    { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT },
                };
                cmdResourceBarrier(cmd, 0, NULL, ARRAYSIZE(textureBarriers), textureBarriers, ARRAYSIZE(rtBarriers), rtBarriers);

                cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
                endCmd(cmd);
            }

            QueueSubmitDesc submitDesc = {};
            submitDesc.mCmdCount = 1;
            submitDesc.mSignalSemaphoreCount = 1;
            submitDesc.mWaitSemaphoreCount = 1;
            submitDesc.ppCmds = &cmd;
            submitDesc.ppSignalSemaphores = &pRenderCompleteSemaphore;
            submitDesc.ppWaitSemaphores = &pImageAcquiredSemaphore;
            submitDesc.pSignalFence = pRenderCompleteFence;
            queueSubmit(pGraphicsQueue, &submitDesc);
            QueuePresentDesc presentDesc = {};
            presentDesc.mIndex = swapchainImageIndex;
            presentDesc.mWaitSemaphoreCount = 1;
            presentDesc.pSwapChain = pSwapChain;
            presentDesc.ppWaitSemaphores = &pRenderCompleteSemaphore;
            presentDesc.mSubmitDone = true;

            if (gTakeScreenshot)
            {
                if (prepareScreenshot(pSwapChain))
                {
                    // gFrameCounter is of type uint64_t. 
                    // Therefore, it has a maximum of floor(log(2^64-1)) + 1 = 20 digits.
                    char filename[20 + 4];
                    sprintf(filename, "%" PRIu64 ".png", gTotalFrameCounter);

                    captureScreenshot(pSwapChain, swapchainImageIndex, RESOURCE_STATE_PRESENT, filename);
                }
            }

            queuePresent(pGraphicsQueue, &presentDesc);
            flipProfiler();
        }

        gFrameIndex = (gFrameIndex + 1) % gImageCount;
        ++gTotalFrameCounter;

        gCurrentPathtraceIndex = !gCurrentPathtraceIndex;
    }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.ppPresentQueues = &pGraphicsQueue;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = gImageCount;
        swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true, true);
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        swapChainDesc.mFlags = SWAP_CHAIN_CREATION_FLAG_ENABLE_FOVEATED_RENDERING_VR;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

        return pSwapChain != NULL;
    }

    bool addPathtraceTextures()
    {
        // Create empty texture for output of compute shader.
        TextureDesc desc = {};
        desc.mWidth = mSettings.mWidth;
        desc.mHeight = mSettings.mHeight;
        desc.mDepth = 1;
        desc.mArraySize = 1;
        desc.mMipLevels = 1;
        desc.mSampleCount = SAMPLE_COUNT_1;
        desc.mStartState = RESOURCE_STATE_UNORDERED_ACCESS;
        desc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE | DESCRIPTOR_TYPE_RW_TEXTURE;

        // using more floats here is worth it since we can avoid dealing with low precision and numerical errors.
        {
            // desc.mFormat = TinyImageFormat_R8G8B8A8_UNORM;
            desc.mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
        }

        TextureLoadDesc textureDesc = {};
        textureDesc.pDesc = &desc;

        const char* textureNames[2] = { "pPathtraceTexture_Back", "pPathtraceTexture_Front" };

        for (uint32_t i = 0; i < 2; ++i)
        {
            desc.pName = textureNames[i];

            textureDesc.ppTexture = &pPathtraceTextures[i];
            addResource(&textureDesc, NULL);
        }

        return pPathtraceTextures[0] != NULL && pPathtraceTextures[1] != NULL;
    }

    void addDescriptorSets()
    {
        DescriptorSetDesc desc = { pPathtraceRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 2 };
        addDescriptorSet(pRenderer, &desc, &pPathtraceDS_None);

        desc = { pPathtraceRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
        addDescriptorSet(pRenderer, &desc, &pPathtraceDS_PerFrame);


        desc = { pResolveRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 2 };
        addDescriptorSet(pRenderer, &desc, &pResolveDS_None);

        desc = { pResolveRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
        addDescriptorSet(pRenderer, &desc, &pResolveDS_PerFrame);

        desc = { pTonemapRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 2 };
        addDescriptorSet(pRenderer, &desc, &pTonemapDS_None);
    }

    void removeDescriptorSets()
    {
        removeDescriptorSet(pRenderer, pTonemapDS_None);

        removeDescriptorSet(pRenderer, pResolveDS_None);
        removeDescriptorSet(pRenderer, pResolveDS_PerFrame);
        
        removeDescriptorSet(pRenderer, pPathtraceDS_None);
        removeDescriptorSet(pRenderer, pPathtraceDS_PerFrame);
    }

    void addRootSignatures()
    {
        // Pathtrace
        {
            const uint32_t shadersCount = 1;

            Shader* shaders[shadersCount];
            shaders[0] = pPathtraceShader;

            RootSignatureDesc rootDesc = {};
            rootDesc.ppShaders = shaders;
            rootDesc.ppStaticSamplers = NULL;
            rootDesc.mStaticSamplerCount = 0;
            rootDesc.mShaderCount = shadersCount;
            rootDesc.ppStaticSamplerNames = NULL;
            addRootSignature(pRenderer, &rootDesc, &pPathtraceRootSignature);
            gPathtraceRootConstantIndex = getDescriptorIndexFromName(pPathtraceRootSignature, SHADER_TYPE_STR(RootConstants));
        }

        // Resolve
        {
            const uint32_t shadersCount = 1;

            Shader* shaders[shadersCount];
            shaders[0] = pResolveShader;

            RootSignatureDesc rootDesc = {};
            rootDesc.ppShaders = shaders;
            rootDesc.ppStaticSamplers = NULL;
            rootDesc.mStaticSamplerCount = 0;
            rootDesc.mShaderCount = shadersCount;
            rootDesc.ppStaticSamplerNames = NULL;
            addRootSignature(pRenderer, &rootDesc, &pResolveRootSignature);
        }

        // Tonemap
        {
            const char* pStaticSamplers[] = { "nearestSampler" };

            SamplerDesc samplerDesc = { FILTER_NEAREST,
                                        FILTER_NEAREST,
                                        MIPMAP_MODE_NEAREST,
                                        ADDRESS_MODE_CLAMP_TO_EDGE,
                                        ADDRESS_MODE_CLAMP_TO_EDGE,
                                        ADDRESS_MODE_CLAMP_TO_EDGE };
            addSampler(pRenderer, &samplerDesc, &pSampler);

            const uint32_t shadersCount = 1;
            Shader* shaders[shadersCount];
            shaders[0] = pTonemapShader;

            RootSignatureDesc rootDesc = {};
            rootDesc.ppShaders = shaders;
            rootDesc.mStaticSamplerCount = 1;
            rootDesc.mShaderCount = shadersCount;
            rootDesc.ppStaticSamplers = &pSampler;
            rootDesc.ppStaticSamplerNames = pStaticSamplers;
            addRootSignature(pRenderer, &rootDesc, &pTonemapRootSignature);
        }
    }

    void removeRootSignatures()
    {
        removeRootSignature(pRenderer, pTonemapRootSignature);
        removeRootSignature(pRenderer, pResolveRootSignature);
        removeRootSignature(pRenderer, pPathtraceRootSignature);
    }

    void addShaders()
    {
        ShaderLoadDesc compDesc = {};
        compDesc.mStages[0] = { "Pathtrace.comp", NULL, 0 };
        addShader(pRenderer, &compDesc, &pPathtraceShader);

        compDesc.mStages[0] = { "Resolve.comp", NULL, 0 };
        addShader(pRenderer, &compDesc, &pResolveShader);

        ShaderLoadDesc fragDesc = {};
        fragDesc.mStages[0] = { "Quad.vert", NULL, 0 };
        fragDesc.mStages[1] = { "Tonemap.frag", NULL, 0 };
        addShader(pRenderer, &fragDesc, &pTonemapShader);
    }

    void removeShaders()
    {
        removeShader(pRenderer, pTonemapShader);
        removeShader(pRenderer, pResolveShader);
        removeShader(pRenderer, pPathtraceShader);
    }

    void addPipelines()
    {
        // Draw pipelines
        {
            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

            PipelineDesc desc = {};
            desc.pName = "Tonemap";
            desc.mType = PIPELINE_TYPE_GRAPHICS;
            
            GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;

            pipelineSettings.mRenderTargetCount = 1;
            pipelineSettings.pShaderProgram = pTonemapShader;
            pipelineSettings.pRootSignature = pTonemapRootSignature;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
            pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
            pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
            addPipeline(pRenderer, &desc, &pTonemapPipeline);
        }

        // Compute pipelines
        {
            PipelineDesc desc = {};
            desc.mType = PIPELINE_TYPE_COMPUTE;
            ComputePipelineDesc& computeDesc = desc.mComputeDesc;

            desc.pName = "Pathtrace";
            computeDesc.pShaderProgram = pPathtraceShader;
            computeDesc.pRootSignature = pPathtraceRootSignature;
            addPipeline(pRenderer, &desc, &pPathtracePipeline);

            desc.pName = "Resolve";
            computeDesc.pShaderProgram = pResolveShader;
            computeDesc.pRootSignature = pResolveRootSignature;
            addPipeline(pRenderer, &desc, &pResolvePipeline);
        }
    }

    void removePipelines()
    {
        removePipeline(pRenderer, pTonemapPipeline);
        removePipeline(pRenderer, pResolvePipeline);
        removePipeline(pRenderer, pPathtracePipeline);
    }

    void prepareDescriptorSets()
    {
        // Pathtrace - Freq: None
        {
            const uint32_t paramCount = 1;
            DescriptorData params[paramCount] = {};
            params[0].pName = "pathtraceUAV";

            params[0].ppTextures = &pPathtraceTextures[gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 0, pPathtraceDS_None, paramCount, params);

            params[0].ppTextures = &pPathtraceTextures[!gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 1, pPathtraceDS_None, paramCount, params);
        }

        // Pathtrace and Resolve - Freq: PerFrame
        {
            const uint32_t paramCount = 4;
            DescriptorData params_perFrame[paramCount] = {};
            params_perFrame[0].pName = "FrameUB";
            params_perFrame[1].pName = "spheresBuffer";
            params_perFrame[2].pName = "quadsBuffer";
            params_perFrame[3].pName = "materialsBuffer";

            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                params_perFrame[0].ppBuffers = &pFrameUB[i];
                params_perFrame[1].ppBuffers = &pSpheresBuffer[i];
                params_perFrame[2].ppBuffers = &pQuadsBuffer[i];
                params_perFrame[3].ppBuffers = &pMaterialsBuffer[i];

                updateDescriptorSet(pRenderer, i, pResolveDS_PerFrame, 1, params_perFrame);
                updateDescriptorSet(pRenderer, i, pPathtraceDS_PerFrame, paramCount, params_perFrame);
            }
        }

        // Resolve - Freq: None
        {
            const uint32_t paramCount = 2;
            DescriptorData params[paramCount] = {};
            params[0].pName = "pathtraceUAV";
            params[1].pName = "prevPathtraceSRV";

            params[0].ppTextures = &pPathtraceTextures[gCurrentPathtraceIndex];
            params[1].ppTextures = &pPathtraceTextures[!gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 0, pResolveDS_None, paramCount, params);

            params[0].ppTextures = &pPathtraceTextures[!gCurrentPathtraceIndex];
            params[1].ppTextures = &pPathtraceTextures[gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 1, pResolveDS_None, paramCount, params);
        }

        // Tonemap
        {
            const uint32_t paramCount = 1;
            DescriptorData params[paramCount] = {};
            params[0].pName = "pathtraceSRV";

            params[0].ppTextures = &pPathtraceTextures[gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 0, pTonemapDS_None, paramCount, params);

            params[0].ppTextures = &pPathtraceTextures[!gCurrentPathtraceIndex];
            updateDescriptorSet(pRenderer, 1, pTonemapDS_None, paramCount, params);
        }
    }

    void addFirstScene()
    {
        addSphere(float3(0.f, 2.f, 6.f), 2, float3(1, 0, 0), float4(0), 0, float4(0));

        addSphere(float3(0.f, 5.f, 0.f), 4, float3(1, 1, 1), float4(0), 0, float4(0));

        addSphere(float3(20, 4, 0), 4, float3(1, 1, 1), float4(0), 1, float4(1));
        addSphere(float3(20, 4, 10), 4, float3(1, 1, 1), float4(0), 1, float4(1));

        addSphere(float3(-20, 4, 0), 4, float3(1), float4(0), 0.5f, float4(0.f, 0.f, 1.f, 1.f));

        // lamp
        // addSphere(float3(-5, 40, 50), 10, float3(1, 1, 1), float4(0.9922f, 0.7216f, 0.0745f, 5.f), 0, float4(0));

        // ground
        // addSphere(float3(0, -101, 0), 100, float3(0, 1, 0), float4(0), 0, float4(0));
        addQuad(float3(-30.f, 0.f, -30.f), float3(-30.f, 0.f, 30.f), float3(30.f, 0.f, 30.f), float3(30.f, 0.f, -30.f), float3(0, 1, 0), float4(0), 0, float4(0));
    }

    void addCornellBox()
    {
        // left sphere
        addSphere(float3(-9.0f, -12.5f, 20.f), 3.0f, float3(0), float4(0), 1, float4(1));
        
        // middle sphere
        addSphere(float3( 0.0f, -12.5f, 20.f), 3.0f, float3(0.9f, 0.25f, 0.25f), float4(0), 0.8f, float4(0.8f, 0.8f, 0.8f, 0.02f));
        
        // right sphere
        addSphere(float3( 9.0f, -12.5f, 20.f), 3.0f, float3(0.75f, 0.9f, 0.9f), float4(0), 0, float4(0));

        // back quad
        addQuad(float3(-15.5f, -15.5f, 30.f), float3(-15.5f, 15.5f, 30.f), float3(15.5f, 15.5f, 30.f), float3(15.5f, -15.5f, 30.f), float3(0.7f, 0.7f, 0.7f), float4(0), 0, float4(0.7f, 0.7f, 0.7f, 0.f));

        // light
        addQuad(float3(-5.f, 15.4f, 17.5f), float3(-5.f, 15.4f, 22.5f), float3(5.f, 15.4f, 22.5f), float3(5.f, 15.4f, 17.5f), float3(0), float4(1), 0, float4(0));
        
        // left quad
        addQuad(float3(-15.5f, -15.5f, 10.f), float3(-15.5f, 15.5f, 10.f), float3(-15.5f, 15.5f, 30.f), float3(-15.5f, -15.5f, 30.f), float3(0.7f, 0.1f, 0.1f), float4(0), 0, float4(0.7f, 0.1f, 0.1f, 0.f));

        // right quad
        addQuad(float3(15.5f, -15.5f, 10.f), float3(15.5f, -15.5f, 30.f), float3(15.5f, 15.5f, 30.f), float3(15.5f, 15.5f, 10.f), float3(0.1f, 0.7f, 0.1f), float4(0), 0, float4(0.1f, 0.7f, 0.1f, 0.f));

        // top quad
        addQuad(float3(-15.5f, 15.5f, 10.f), float3(-15.5f, 15.5f, 30.f), float3(15.5f, 15.5f, 30.f), float3(15.5f, 15.5f, 10.f), float3(0.7f, 0.7f, 0.7f), float4(0), 0, float4(0.7f, 0.7f, 0.7f, 0.f));

        // bottom quad
        addQuad(float3(-15.5f, -15.5f, 10.f), float3(-15.5f, -15.5f, 30.f), float3(15.5f, -15.5f, 30.f), float3(15.5f, -15.5f, 10.f), float3(0.7f, 0.7f, 0.7f), float4(0), 0, float4(0.7f, 0.7f, 0.7f, 0.f));
    }

    void addOneWeekendScene()
    {

    }

    void addSphere(float3 center, float radius, float3 color, float4 emission, float glossiness, float4 spec)
    {
        int& activeSphereCount = gRootConstants.args.z;
        ASSERT(activeSphereCount < MAX_SPHERES_COUNT);

        int idx = activeSphereCount;

        gSpheres[idx].centerRadius = float4(center, radius);

        gSpheres[idx].matId = uint4(addMaterial(color, emission, glossiness, spec));

        ++activeSphereCount;
    }

    void addQuad(float3 a, float3 b, float3 c, float3 d, float3 color, float4 emission, float glossiness, float4 spec)
    {
        int& activeQuadCount = gRootConstants.args.w;
        ASSERT(activeQuadCount < MAX_QUADS_COUNT);

        int idx = activeQuadCount;

        // pack quad corners.
        gQuads[idx].aXYZbX = float4(a.x, a.y, a.z, b.x);
        gQuads[idx].bYZcXY = float4(b.y, b.z, c.x, c.y);
        gQuads[idx].cZdXYZ = float4(c.z, d.x, d.y, d.z);

        gQuads[idx].matId = uint4(addMaterial(color, emission, glossiness, spec));

        ++activeQuadCount;
    }

    uint32_t addMaterial(float3 color, float4 emission, float glossiness, float4 spec)
    {
        ASSERT(gActiveMaterialIndex < MAX_MATERIAL_COUNT);

        gMaterials[gActiveMaterialIndex].spec = spec;
        gMaterials[gActiveMaterialIndex].emission = emission;
        gMaterials[gActiveMaterialIndex].color = float4(color, 1.0);
        gMaterials[gActiveMaterialIndex].glossiness = float4(glossiness);

        return gActiveMaterialIndex++;
    }

    vec3 randomPointOnUnitSphere() const
    {
        float u = randomFloat01();
        float v = randomFloat01();
        float theta = 2 * PI * u;
        float phi = acosf(2 * v - 1);
        float x = sinf(phi) * cosf(theta);
        float y = sinf(phi) * sinf(theta);
        float z = cosf(phi);
        return vec3(x, y, z);
    }

    Ray screenPointToRay(float2 screenPos, const mat4& invVP) const
    {
        vec3 originPoint = vec3(screenPos.x, screenPos.y, 0.f);
        vec3 targetPoint = vec3(screenPos.x, screenPos.y, 1.f);

        originPoint = screenPointToWorld(originPoint, invVP);
        targetPoint = screenPointToWorld(targetPoint, invVP);

        return Ray(originPoint, targetPoint - originPoint);
    }

    vec3 screenPointToWorld(const vec3& vec, const mat4& invVP) const
    {
        vec4 screenPos(vec.getX() / float(mSettings.mWidth), vec.getY() / float(mSettings.mHeight), vec.getZ(), 1.f);
        screenPos.setX(screenPos.getX() * 2.f - 1.f);
        screenPos.setY(1.f - screenPos.getY() * 2.f);
        screenPos.setZ(screenPos.getZ() * 2.f - 1.f);

        screenPos = invVP * screenPos;
        if (screenPos.getW() != 0.f)
        {
            screenPos.setX(screenPos.getX() / screenPos.getW());
            screenPos.setY(screenPos.getY() / screenPos.getW());
            screenPos.setZ(screenPos.getZ() / screenPos.getW());
        }

        return vec3(screenPos.getX(), screenPos.getY(), screenPos.getZ());
    }
};

DEFINE_APPLICATION_MAIN(PathTracingApp)
