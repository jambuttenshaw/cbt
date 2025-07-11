#include <donut/app/ApplicationBase.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_renderer.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>

#include <bitset>

#define CBT_IMPLEMENTATION
#include "cbt.h"

#define LEB_IMPLEMENTATION
#include "leb.h"

using namespace donut;
using namespace donut::math;

static const char* g_WindowTitle = "Concurrent Binary Tree";

enum Backends : int
{
	Backend_CPU = 0,
    Backend_GPU,
    Backend_COUNT
};

enum DisplayModes : int
{
    DisplayMode_Wireframe = 0,
    DisplayMode_Fill,
    DisplayMode_COUNT
};

enum CBTBitFlags
{
    CBT_Bit_Reset = 0, // Reset the tree object to its initial depth
    CBT_Bit_Create,  // Recreate a new tree and buffer (with a new max depth)
    CBT_Bit_COUNT,
};

struct UIData
{
    Backends Backend = Backend_GPU;
    DisplayModes DisplayMode = DisplayMode_Wireframe;

	float2 Target{ 0.2371f, 0.7104f };
    int CBTMaxDepth = 16;

    std::bitset<CBT_Bit_COUNT> CBTFlags;
};

// Methods for performing CBT split / merge logic on the CPU
float Wedge(const float* a, const float* b)
{
    return a[0] * b[1] - a[1] * b[0];
}

bool IsInside(const float faceVertices[][3], float2 t)
{
    float target[2] = { t.x, t.y };
    float v1[2] = { faceVertices[0][0], faceVertices[1][0] };
    float v2[2] = { faceVertices[0][1], faceVertices[1][1] };
    float v3[2] = { faceVertices[0][2], faceVertices[1][2] };
    float x1[2] = { v2[0] - v1[0], v2[1] - v1[1] };
    float x2[2] = { v3[0] - v2[0], v3[1] - v2[1] };
    float x3[2] = { v1[0] - v3[0], v1[1] - v3[1] };
    float y1[2] = { target[0] - v1[0], target[1] - v1[1] };
    float y2[2] = { target[0] - v2[0], target[1] - v2[1] };
    float y3[2] = { target[0] - v3[0], target[1] - v3[1] };
    float w1 = Wedge(x1, y1);
    float w2 = Wedge(x2, y2);
    float w3 = Wedge(x3, y3);

    return (w1 >= 0.0f) && (w2 >= 0.0f) && (w3 >= 0.0f);
}

void UpdateSubdivisionCpuCallback_Split(
    cbt_Tree* cbt,
    const cbt_Node node,
    const void* userData
) {
    const float2* target = reinterpret_cast<const float2*>(userData);

    float faceVertices[][3] = {
        {0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f}
    };

    leb_DecodeNodeAttributeArray_Square(node, 2, faceVertices);

    if (IsInside(faceVertices, *target)) {
        leb_SplitNode_Square(cbt, node);
    }
}

void UpdateSubdivisionCpuCallback_Merge(
    cbt_Tree* cbt,
    const cbt_Node node,
    const void* userData
) {
    const float2* target = reinterpret_cast<const float2*>(userData);

    float baseFaceVertices[][3] = {
        {0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f}
    };
    float topFaceVertices[][3] = {
        {0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f}
    };

    leb_DiamondParent diamondParent = leb_DecodeDiamondParent_Square(node);

    leb_DecodeNodeAttributeArray_Square(diamondParent.base, 2, baseFaceVertices);
    leb_DecodeNodeAttributeArray_Square(diamondParent.top, 2, topFaceVertices);

    if (!IsInside(baseFaceVertices, *target) && !IsInside(topFaceVertices, *target)) {
        leb_MergeNode_Square(cbt, node, diamondParent);
    }
}

class CBTSubdivision : public app::IRenderPass
{
private:
	UIData& m_UI;

    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;

    enum Shaders
    {
        Shader_Triangle_VS = 0,
        Shader_Triangle_PS,
        Shader_Target_VS,
        Shader_Target_PS,
        Shader_LEB_Dispatcher_CS,
        Shader_CBT_Dispatcher_CS,
        Shader_CBT_Split_CS,
        Shader_CBT_Merge_CS,
        Shader_CBT_SumReduction_CS,
        Shader_COUNT,
    };
    nvrhi::ShaderHandle m_Shaders[Shader_COUNT];

    struct IndirectArgs
    {
        nvrhi::DispatchIndirectArguments DispatchArgs;
        nvrhi::DrawIndirectArguments DrawArgs;
    };
    nvrhi::BufferHandle m_IndirectArgsBuffer;

    enum CBTBindings
    {
	    CBTBinding_ReadOnly = 0,
        CBTBinding_ReadWrite,
        CBTBinding_COUNT
    };
    nvrhi::BindingLayoutHandle m_CBTBindingLayouts[CBTBinding_COUNT];
    nvrhi::BindingSetHandle m_CBTBindingSets[CBTBinding_COUNT];

    nvrhi::BindingLayoutHandle m_ConstantsBindingLayout;
    nvrhi::BindingSetHandle m_ConstantsBindingSet;

    nvrhi::BindingLayoutHandle m_IndirectArgsBindingLayout;
	nvrhi::BindingSetHandle m_IndirectArgsBindingSet;

    nvrhi::CommandListHandle m_CommandList;

    enum GraphicsPipelines
    {
        Pipeline_Triangles_Wireframe = 0,
	    Pipeline_Triangles_Fill,
        Pipeline_Target,
        GraphicsPipeline_COUNT
    };
    nvrhi::GraphicsPipelineHandle m_GraphicsPipelines[GraphicsPipeline_COUNT];

    enum ComputePipelines
    {
	    Pipeline_LEB_Dispatcher = 0,
	    Pipeline_CBT_Dispatcher,
        Pipeline_CBT_Split,
        Pipeline_CBT_Merge,
        Pipeline_CBT_SumReduction,
        ComputePipeline_COUNT
    };
    nvrhi::ComputePipelineHandle m_ComputePipelines[ComputePipeline_COUNT];

    cbt_Tree* m_CBT = nullptr;
    inline static constexpr uint s_CBTInitDepth = 1;
    nvrhi::BufferHandle m_CBTBuffer;

    uint64_t FrameCounter = 0;

public:
    using IRenderPass::IRenderPass;

    CBTSubdivision(app::DeviceManager* deviceManager, UIData& ui)
	    : IRenderPass(deviceManager)
		, m_UI(ui)
    {
    }

    ~CBTSubdivision()
    {
        if (m_CBT) cbt_Release(m_CBT);
    }

    const std::shared_ptr<engine::ShaderFactory>& GetShaderFactory() const { return m_ShaderFactory; }
    int64_t GetCBTNodeCount() const { return m_CBT ? cbt_NodeCount(m_CBT) : 0; }

    bool Init()
    {
        std::shared_ptr<vfs::RootFileSystem> rootFS = std::make_shared<vfs::RootFileSystem>();
        rootFS->mount("/shaders/donut", 
            app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        rootFS->mount("/shaders/app", 
            app::GetDirectoryWithExecutable() / "shaders/cbt" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));

        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), rootFS, "/shaders");

        m_Shaders[Shader_Triangle_VS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_Shaders[Shader_Triangle_PS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_Shaders[Shader_Target_VS] = m_ShaderFactory->CreateShader("app/target.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_Shaders[Shader_Target_PS] = m_ShaderFactory->CreateShader("app/target.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);

        m_Shaders[Shader_LEB_Dispatcher_CS] = m_ShaderFactory->CreateShader("app/dispatcher.hlsl", "leb_dispatcher_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_Dispatcher_CS] = m_ShaderFactory->CreateShader("app/dispatcher.hlsl", "cbt_dispatcher_cs", nullptr, nvrhi::ShaderType::Compute);

        std::vector<engine::ShaderMacro> splitMacros{ {"FLAG_SPLIT", "1"} };
        m_Shaders[Shader_CBT_Split_CS] = m_ShaderFactory->CreateShader("app/subdivision.hlsl", "main_cs", &splitMacros, nvrhi::ShaderType::Compute);
        std::vector<engine::ShaderMacro> mergeMacros{ {"FLAG_SPLIT", "0"} };
        m_Shaders[Shader_CBT_Merge_CS] = m_ShaderFactory->CreateShader("app/subdivision.hlsl", "main_cs", &splitMacros, nvrhi::ShaderType::Compute);

        m_Shaders[Shader_CBT_SumReduction_CS] = m_ShaderFactory->CreateShader("app/sum_reduction.hlsl", "main_cs", nullptr, nvrhi::ShaderType::Compute);

        if (std::ranges::any_of(m_Shaders, [](const auto& shader){ return !shader; }))
            return false;

        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.setByteSize(sizeof(IndirectArgs))
				.setCanHaveTypedViews(true)
                .setCanHaveUAVs(true)
				.setStructStride(sizeof(IndirectArgs))
				.setIsDrawIndirectArgs(true)
                .setInitialState(nvrhi::ResourceStates::IndirectArgument)
				.setKeepInitialState(true)
                .setDebugName("IndirectArgs");
            m_IndirectArgsBuffer = GetDevice()->createBuffer(bufferDesc);
        }

        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setRegisterSpace(0);

            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)
            };
            m_CBTBindingLayouts[CBTBinding_ReadOnly] = GetDevice()->createBindingLayout(layoutDesc);

            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)
            };
            m_CBTBindingLayouts[CBTBinding_ReadWrite] = GetDevice()->createBindingLayout(layoutDesc);
        }

        {
	        nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
				.setRegisterSpace(0)
				.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint2)));
            m_ConstantsBindingLayout = GetDevice()->createBindingLayout(layoutDesc);

            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(uint2)));
            m_ConstantsBindingSet = GetDevice()->createBindingSet(setDesc, m_ConstantsBindingLayout);
        }

        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpace(0)
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
            m_IndirectArgsBindingLayout = GetDevice()->createBindingLayout(layoutDesc);

            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_IndirectArgsBuffer));
            m_IndirectArgsBindingSet = GetDevice()->createBindingSet(setDesc, m_IndirectArgsBindingLayout);
        }

        {
            nvrhi::ComputePipelineDesc psoDesc;
            psoDesc.setComputeShader(m_Shaders[Shader_LEB_Dispatcher_CS])
                .addBindingLayout(m_CBTBindingLayouts[CBTBinding_ReadOnly])
                .addBindingLayout(m_IndirectArgsBindingLayout);
            m_ComputePipelines[Pipeline_LEB_Dispatcher] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Dispatcher_CS]);
            m_ComputePipelines[Pipeline_CBT_Dispatcher] = GetDevice()->createComputePipeline(psoDesc);
        }
        {
            nvrhi::ComputePipelineDesc psoDesc;
            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Split_CS])
                .addBindingLayout(m_CBTBindingLayouts[CBTBinding_ReadWrite])
                .addBindingLayout(m_ConstantsBindingLayout);
            m_ComputePipelines[Pipeline_CBT_Split] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Merge_CS]);
            m_ComputePipelines[Pipeline_CBT_Merge] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_SumReduction_CS]);
            m_ComputePipelines[Pipeline_CBT_SumReduction] = GetDevice()->createComputePipeline(psoDesc);
        }

        m_CommandList = GetDevice()->createCommandList();
        m_CommandList->open();

        {
            IndirectArgs indirectArgs;
            indirectArgs.DrawArgs.vertexCount = 3;

            m_CommandList->beginTrackingBufferState(m_IndirectArgsBuffer, nvrhi::ResourceStates::CopyDest);
            m_CommandList->writeBuffer(m_IndirectArgsBuffer, &indirectArgs, sizeof(indirectArgs));
        }

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);

        // Will queue the CBT to be created on the first frame
        m_UI.CBTFlags.set(CBT_Bit_Create);

        return true;
    }

    void CreateCBTBuffer()
    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.setByteSize(cbt_HeapByteSize(m_CBT))
            .setCanHaveTypedViews(true)
			.setStructStride(sizeof(uint))
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::Common)
            .setKeepInitialState(true)
            .setDebugName("CBT");
        m_CBTBuffer = GetDevice()->createBuffer(bufferDesc);
    }

    void CopyToCBTBuffer()
    {
        m_CommandList->writeBuffer(m_CBTBuffer, cbt_GetHeap(m_CBT), cbt_HeapByteSize(m_CBT));
    }

    void CreateCBTBindingSets()
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_CBTBuffer)
        };
        m_CBTBindingSets[CBTBinding_ReadOnly] = GetDevice()->createBindingSet(setDesc, m_CBTBindingLayouts[CBTBinding_ReadOnly]);

        setDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CBTBuffer)
        };
        m_CBTBindingSets[CBTBinding_ReadWrite] = GetDevice()->createBindingSet(setDesc, m_CBTBindingLayouts[CBTBinding_ReadWrite]);
    }

    void CreateGraphicsPipelines(nvrhi::IFramebuffer* framebuffer)
    {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.renderState.depthStencilState.depthTestEnable = false;

        {
            psoDesc.VS = m_Shaders[Shader_Triangle_VS];
            psoDesc.PS = m_Shaders[Shader_Triangle_PS];
            psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
            psoDesc.bindingLayouts = { m_CBTBindingLayouts[CBTBinding_ReadOnly], m_ConstantsBindingLayout };
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;

            psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
            m_GraphicsPipelines[Pipeline_Triangles_Wireframe] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);

            psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Fill;
            m_GraphicsPipelines[Pipeline_Triangles_Fill] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
        }
        {
            psoDesc.VS = m_Shaders[Shader_Target_VS];
            psoDesc.PS = m_Shaders[Shader_Target_PS];
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.bindingLayouts = { m_ConstantsBindingLayout };
            psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Fill;

            m_GraphicsPipelines[Pipeline_Target] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
        }
    }

    void BackBufferResizing() override
    {
        for (auto& pipeline : m_GraphicsPipelines)
			pipeline = nullptr;
    }

    void UpdateSubdivision()
    {
        static int pingPong = 0;

        if (m_UI.Backend == Backend_CPU)
        {
            if (pingPong == 0) 
                cbt_Update(m_CBT, &UpdateSubdivisionCpuCallback_Split, &m_UI.Target);
            else
                cbt_Update(m_CBT, &UpdateSubdivisionCpuCallback_Merge, &m_UI.Target);

            CopyToCBTBuffer();
        }
        else
        {
            m_CommandList->beginMarker("Update Subdivision");

            // Write indirect args for subdivision kernel
            {
				m_CommandList->beginMarker("CBT Dispatch");

            	nvrhi::ComputeState state;
                state.pipeline = m_ComputePipelines[Pipeline_CBT_Dispatcher];
                state.bindings = { m_CBTBindingSets[CBTBinding_ReadOnly], m_IndirectArgsBindingSet };
                m_CommandList->setComputeState(state);

                m_CommandList->dispatch(1);
                m_CommandList->endMarker();
            }

            // Dispatch subdivision
            {
                m_CommandList->beginMarker(pingPong ? "Subdivision: Merge" : "Subdivision: Split");

                nvrhi::ComputeState state;
                state.pipeline = m_ComputePipelines[pingPong ? Pipeline_CBT_Merge : Pipeline_CBT_Split];
                state.bindings = { m_CBTBindingSets[CBTBinding_ReadWrite], m_ConstantsBindingSet };
                state.indirectParams = m_IndirectArgsBuffer;
                m_CommandList->setComputeState(state);

                float2 constants = m_UI.Target;
                m_CommandList->setPushConstants(&constants, sizeof(constants));

                m_CommandList->dispatchIndirect(offsetof(IndirectArgs, DispatchArgs));
                m_CommandList->endMarker();
            }

            // Perform sum reduction
            {
                m_CommandList->beginMarker("Sum Reduction");

                nvrhi::ComputeState state;
                state.pipeline = m_ComputePipelines[Pipeline_CBT_SumReduction];
                state.bindings = { m_CBTBindingSets[CBTBinding_ReadWrite], m_ConstantsBindingSet };
                m_CommandList->setComputeState(state);

                int it = static_cast<int>(cbt_MaxDepth(m_CBT));
                while (--it >= 0) {
                    int cnt = 1 << it;
                    int numGroup = (cnt >= 256) ? (cnt >> 8) : 1;

                    uint2 constants = { static_cast<uint>(it), 0 };
                    m_CommandList->setPushConstants(&constants, sizeof(constants));

                    m_CommandList->dispatch(static_cast<uint32_t>(numGroup));

                    nvrhi::utils::BufferUavBarrier(m_CommandList, m_CBTBuffer);
                    m_CommandList->commitBarriers();
                }
                m_CommandList->endMarker();
            }

            m_CommandList->endMarker();
        }

        pingPong = 1 - pingPong;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void DrawLeb(nvrhi::IFramebuffer* framebuffer)
    {
        m_CommandList->beginMarker("Draw LEB");

        {
            nvrhi::ComputeState state;
            state.pipeline = m_ComputePipelines[Pipeline_LEB_Dispatcher];
            state.bindings = { m_CBTBindingSets[CBTBinding_ReadOnly], m_IndirectArgsBindingSet };
            m_CommandList->setComputeState(state);

            m_CommandList->dispatch(1);
        }

        {
            nvrhi::GraphicsState state;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
            state.pipeline = m_GraphicsPipelines[m_UI.DisplayMode == DisplayMode_Wireframe ? Pipeline_Triangles_Wireframe : Pipeline_Triangles_Fill];
            state.bindings = { m_CBTBindingSets[CBTBinding_ReadOnly], m_ConstantsBindingSet };
            state.indirectParams = m_IndirectArgsBuffer;
            m_CommandList->setGraphicsState(state);

            uint2 constants = { static_cast<uint>(cbt_NodeCount(m_CBT)), static_cast<uint>(m_UI.DisplayMode) };
            m_CommandList->setPushConstants(&constants, sizeof(constants));

            m_CommandList->drawIndirect(offsetof(IndirectArgs, DrawArgs));
        }

    	m_CommandList->endMarker();
    }

    void DrawTarget(nvrhi::IFramebuffer* framebuffer)
    {
        m_CommandList->beginMarker("Draw Target");

        nvrhi::GraphicsState state;
        state.framebuffer = framebuffer;
        state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
        state.pipeline = m_GraphicsPipelines[Pipeline_Target];
        state.bindings = { m_ConstantsBindingSet };
        m_CommandList->setGraphicsState(state);

        float2 target = m_UI.Target * 2.0f - 1.0f;
        m_CommandList->setPushConstants(&target, sizeof(target));

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        m_CommandList->draw(args);

        m_CommandList->endMarker();
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        if (std::ranges::any_of(m_GraphicsPipelines, [](const auto& pipeline) { return !pipeline; }))
        {
            CreateGraphicsPipelines(framebuffer);
        }

        m_CommandList->open();

        {
            std::string frameMarker = "Frame " + std::to_string(FrameCounter++);
            m_CommandList->beginMarker(frameMarker.c_str());
        }

        if (m_UI.CBTFlags.test(CBT_Bit_Create))
        {
            if (m_CBT) cbt_Release(m_CBT);
            m_CBT = cbt_CreateAtDepth(m_UI.CBTMaxDepth, s_CBTInitDepth);
            CreateCBTBuffer();
            if (m_UI.Backend != Backend_CPU) CopyToCBTBuffer();
            CreateCBTBindingSets();
        }
        else if (m_UI.CBTFlags.test(CBT_Bit_Reset))
        {
            cbt_ResetToDepth(m_CBT, s_CBTInitDepth);
            if (m_UI.Backend != Backend_CPU) CopyToCBTBuffer();
        }
        m_UI.CBTFlags.reset();

        UpdateSubdivision();

        nvrhi::utils::ClearColorAttachment(m_CommandList, framebuffer, 0, nvrhi::Color(1.f));

        DrawLeb(framebuffer);
        DrawTarget(framebuffer);

        m_CommandList->endMarker();

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
    }

};

class UIRenderer : public app::ImGui_Renderer
{
private:
    UIData& m_UI;
    CBTSubdivision& m_App;

public:
    UIRenderer(app::DeviceManager* deviceManager, UIData& ui, CBTSubdivision& app)
	    : ImGui_Renderer(deviceManager)
		, m_UI(ui)
		, m_App(app)
    {}

protected:
    virtual void buildUI() override
    {
	    ImGui::Begin("Options");

        const char* eBackends[] = { "CPU", "GPU" };
        ImGui::Combo("Backend", reinterpret_cast<int*>(&m_UI.Backend), eBackends, 2);

        const char* eDisplayModes[] = { "Wireframe", "Fill" };
        ImGui::Combo("Display Mode", reinterpret_cast<int*>(&m_UI.DisplayMode), eDisplayModes, 2);

        ImGui::SliderFloat("TargetX", &m_UI.Target.x, 0, 1);
        ImGui::SliderFloat("TargetY", &m_UI.Target.y, 0, 1);
        m_UI.CBTFlags[CBT_Bit_Create] = ImGui::SliderInt("MaxDepth", &m_UI.CBTMaxDepth, 6, 20);
        m_UI.CBTFlags[CBT_Bit_Reset] = ImGui::Button("Reset");

        ImGui::Separator();

    	ImGui::Text("Node count: %d", static_cast<int>(m_App.GetCBTNodeCount()));

        ImGui::End();
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 720;
    deviceParams.backBufferHeight = 720;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true; 
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }
    
    {
		UIData ui;

        CBTSubdivision app(deviceManager, ui);
        UIRenderer uiRenderer(deviceManager, ui, app);
        if (app.Init() && uiRenderer.Init(app.GetShaderFactory()))
        {
            deviceManager->AddRenderPassToBack(&app);
            deviceManager->AddRenderPassToBack(&uiRenderer);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&uiRenderer);
            deviceManager->RemoveRenderPass(&app);
        }
    }
    
    deviceManager->Shutdown();
    delete deviceManager;

    return 0;
}
