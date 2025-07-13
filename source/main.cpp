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

#include "cbt_shared.h"

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

enum GPUTimers
{
    Timer_Subdivision = 0,
    Timer_SumReduction,
    Timer_DrawLEB,
    Timer_COUNT
};

struct UIData
{
    Backends Backend = Backend_GPU;
    DisplayModes DisplayMode = DisplayMode_Wireframe;

	float2 Target{ 0.2371f, 0.7104f };
    int CBTMaxDepth = 12;

    std::bitset<CBT_Bit_COUNT> CBTFlags;

    // Updated by the application to display in the UI (in milliseconds)
    std::array<float, Timer_COUNT> TimerData{};
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
    nvrhi::CommandListHandle m_CommandList;

    enum Shaders
    {
        Shader_Triangle_Wireframe_VS = 0,
        Shader_Triangle_Wireframe_PS,
        Shader_Triangle_Fill_VS,
        Shader_Triangle_Fill_PS,
        Shader_Target_VS,
        Shader_Target_PS,
        Shader_LEB_Dispatcher_CS,
        Shader_CBT_Dispatcher_CS,
        Shader_CBT_Split_CS,
        Shader_CBT_Merge_CS,
        Shader_CBT_SumReduction_PrePass_CS,
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

    // Bindings are separated as such to be as modular as possible
    // All kernels' bindings are a combination of some of these layouts
    enum Bindings
    {
        Bindings_Constants = 0,
        Bindings_IndirectArgs,
	    Bindings_CBTReadOnly,
        Bindings_CBTReadWrite,
        Bindings_COUNT
    };
    nvrhi::BindingLayoutHandle m_BindingLayouts[Bindings_COUNT];
    nvrhi::BindingSetHandle m_BindingSets[Bindings_COUNT];

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
        Pipeline_CBT_SumReductionPrePass,
        Pipeline_CBT_SumReduction,
        ComputePipeline_COUNT
    };
    nvrhi::ComputePipelineHandle m_ComputePipelines[ComputePipeline_COUNT];

    cbt_Tree* m_CBT = nullptr;
    inline static constexpr uint s_CBTInitDepth = 1;
    nvrhi::BufferHandle m_CBTBuffer;

    std::vector<std::array<nvrhi::TimerQueryHandle, Timer_COUNT>> m_Timers; // One set of timers per back buffer to avoid blocking
    uint m_TimerSetIndex = 0;

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
    nvrhi::ITimerQuery* GetTimer(GPUTimers timer) const { return m_Timers.at(m_TimerSetIndex).at(timer); }

    bool Init()
    {
        std::shared_ptr<vfs::RootFileSystem> rootFS = std::make_shared<vfs::RootFileSystem>();
        rootFS->mount("/shaders/donut", 
            app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        rootFS->mount("/shaders/app", 
            app::GetDirectoryWithExecutable() / "shaders/cbt" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));

        // Create shaders
        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), rootFS, "/shaders");

        m_Shaders[Shader_Triangle_Wireframe_VS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "wireframe_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_Shaders[Shader_Triangle_Wireframe_PS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "wireframe_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_Shaders[Shader_Triangle_Fill_VS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "fill_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_Shaders[Shader_Triangle_Fill_PS] = m_ShaderFactory->CreateShader("app/triangles.hlsl", "fill_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_Shaders[Shader_Target_VS] = m_ShaderFactory->CreateShader("app/target.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_Shaders[Shader_Target_PS] = m_ShaderFactory->CreateShader("app/target.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);

        m_Shaders[Shader_LEB_Dispatcher_CS] = m_ShaderFactory->CreateShader("app/dispatcher.hlsl", "leb_dispatcher_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_Dispatcher_CS] = m_ShaderFactory->CreateShader("app/dispatcher.hlsl", "cbt_dispatcher_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_Split_CS] = m_ShaderFactory->CreateShader("app/subdivision.hlsl", "split_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_Merge_CS] = m_ShaderFactory->CreateShader("app/subdivision.hlsl", "merge_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_SumReduction_PrePass_CS] = m_ShaderFactory->CreateShader("app/sum_reduction.hlsl", "sum_reduction_prepass_cs", nullptr, nvrhi::ShaderType::Compute);
        m_Shaders[Shader_CBT_SumReduction_CS] = m_ShaderFactory->CreateShader("app/sum_reduction.hlsl", "sum_reduction_cs", nullptr, nvrhi::ShaderType::Compute);

        if (std::ranges::any_of(m_Shaders, [](const auto& shader){ return !shader; }))
            return false;

        // Create indirect args buffer
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

        // Setup bindings
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::All)
                .setRegisterSpace(CONSTANTS_REGISTER_SPACE)
                .setRegisterSpaceIsDescriptorSet(true)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint2)));
            m_BindingLayouts[Bindings_Constants] = GetDevice()->createBindingLayout(layoutDesc);

            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(uint2)));
            m_BindingSets[Bindings_Constants] = GetDevice()->createBindingSet(setDesc, m_BindingLayouts[Bindings_Constants]);
        }
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setRegisterSpace(CBT_REGISTER_SPACE)
                .setRegisterSpaceIsDescriptorSet(true);

            layoutDesc.setVisibility(nvrhi::ShaderType::All);
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)
            };
            m_BindingLayouts[Bindings_CBTReadOnly] = GetDevice()->createBindingLayout(layoutDesc);

            layoutDesc.setVisibility(nvrhi::ShaderType::All);
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)
            };
            m_BindingLayouts[Bindings_CBTReadWrite] = GetDevice()->createBindingLayout(layoutDesc);
        }
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpace(INDIRECT_ARGS_REGISTER_SPACE)
                .setRegisterSpaceIsDescriptorSet(true)
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
            m_BindingLayouts[Bindings_IndirectArgs] = GetDevice()->createBindingLayout(layoutDesc);

            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_IndirectArgsBuffer));
            m_BindingSets[Bindings_IndirectArgs] = GetDevice()->createBindingSet(setDesc, m_BindingLayouts[Bindings_IndirectArgs]);
        }

        // Create compute pipelines (graphics pipelines need to know the framebuffer)
        {
            nvrhi::ComputePipelineDesc psoDesc;
            psoDesc.setComputeShader(m_Shaders[Shader_LEB_Dispatcher_CS])
                .addBindingLayout(m_BindingLayouts[Bindings_CBTReadOnly])
                .addBindingLayout(m_BindingLayouts[Bindings_IndirectArgs]);
            m_ComputePipelines[Pipeline_LEB_Dispatcher] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Dispatcher_CS]);
            m_ComputePipelines[Pipeline_CBT_Dispatcher] = GetDevice()->createComputePipeline(psoDesc);
        }
        {
            nvrhi::ComputePipelineDesc psoDesc;
            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Split_CS])
                .addBindingLayout(m_BindingLayouts[Bindings_CBTReadWrite])
                .addBindingLayout(m_BindingLayouts[Bindings_Constants]);
            m_ComputePipelines[Pipeline_CBT_Split] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_Merge_CS]);
            m_ComputePipelines[Pipeline_CBT_Merge] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_SumReduction_PrePass_CS]);
            m_ComputePipelines[Pipeline_CBT_SumReductionPrePass] = GetDevice()->createComputePipeline(psoDesc);

            psoDesc.setComputeShader(m_Shaders[Shader_CBT_SumReduction_CS]);
            m_ComputePipelines[Pipeline_CBT_SumReduction] = GetDevice()->createComputePipeline(psoDesc);
        }

        // Create GPU timer queries
        for (size_t i = 0; i < GetDeviceManager()->GetDeviceParams().swapChainBufferCount; i++)
        {
            auto& timers = m_Timers.emplace_back();
            for (auto& timer : timers)
            {
				timer = GetDevice()->createTimerQuery();
                GetDevice()->resetTimerQuery(timer);
            }
        }

        // Upload initial data to indirect args (instance/group count will be modified by dispatcher kernels)
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

    void CopyToCBTBuffer() const
    {
        m_CommandList->writeBuffer(m_CBTBuffer, cbt_GetHeap(m_CBT), cbt_HeapByteSize(m_CBT));
    }

    void CreateCBTBindingSets()
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_CBTBuffer)
        };
        m_BindingSets[Bindings_CBTReadOnly] = GetDevice()->createBindingSet(setDesc, m_BindingLayouts[Bindings_CBTReadOnly]);

        setDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CBTBuffer)
        };
        m_BindingSets[Bindings_CBTReadWrite] = GetDevice()->createBindingSet(setDesc, m_BindingLayouts[Bindings_CBTReadWrite]);
    }

    void CreateGraphicsPipelines(nvrhi::IFramebuffer* framebuffer)
    {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.renderState.depthStencilState.depthTestEnable = false;

        {
            psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
            psoDesc.bindingLayouts = { m_BindingLayouts[Bindings_CBTReadOnly] };
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front; // CBT library produces triangles with wrong winding order

            psoDesc.VS = m_Shaders[Shader_Triangle_Wireframe_VS];
            psoDesc.PS = m_Shaders[Shader_Triangle_Wireframe_PS];
            psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
            m_GraphicsPipelines[Pipeline_Triangles_Wireframe] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);

            psoDesc.VS = m_Shaders[Shader_Triangle_Fill_VS];
            psoDesc.PS = m_Shaders[Shader_Triangle_Fill_PS];
            psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Fill;
            m_GraphicsPipelines[Pipeline_Triangles_Fill] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
        }
        {
            psoDesc.VS = m_Shaders[Shader_Target_VS];
            psoDesc.PS = m_Shaders[Shader_Target_PS];
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.bindingLayouts = { m_BindingLayouts[Bindings_Constants] };
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;

            m_GraphicsPipelines[Pipeline_Target] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
        }
    }

    void StartTimer(GPUTimers timerLabel) const
    {
        auto timer = GetTimer(timerLabel);
        if (GetDevice()->pollTimerQuery(timer))
        {
            // Send timer data to UI in milliseconds
            m_UI.TimerData[timerLabel] = GetDevice()->getTimerQueryTime(timer) * 1000.0f;
            GetDevice()->resetTimerQuery(timer);
        }

        m_CommandList->beginTimerQuery(timer);
    }

    void StopTimer(GPUTimers timerLabel) const
    {
    	m_CommandList->endTimerQuery(GetTimer(timerLabel));
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
                state.bindings = { m_BindingSets[Bindings_CBTReadOnly], m_BindingSets[Bindings_IndirectArgs] };
                m_CommandList->setComputeState(state);

                m_CommandList->dispatch(1);
                m_CommandList->endMarker();
            }

            // Dispatch subdivision
            {
                m_CommandList->beginMarker(pingPong ? "Subdivision: Merge" : "Subdivision: Split");
                StartTimer(Timer_Subdivision);

                nvrhi::ComputeState state;
                state.pipeline = m_ComputePipelines[pingPong ? Pipeline_CBT_Merge : Pipeline_CBT_Split];
                state.bindings = { m_BindingSets[Bindings_CBTReadWrite], m_BindingSets[Bindings_Constants] };
                state.indirectParams = m_IndirectArgsBuffer;
                m_CommandList->setComputeState(state);

                float2 constants = m_UI.Target;
                m_CommandList->setPushConstants(&constants, sizeof(constants));

                m_CommandList->dispatchIndirect(offsetof(IndirectArgs, DispatchArgs));

                StopTimer(Timer_Subdivision);
                m_CommandList->endMarker();
            }

            // Perform sum reduction
            {
                m_CommandList->beginMarker("Sum Reduction");
                StartTimer(Timer_SumReduction);

                int it = static_cast<int>(cbt_MaxDepth(m_CBT));

                nvrhi::ComputeState state;

                {
                    state.pipeline = m_ComputePipelines[Pipeline_CBT_SumReductionPrePass];
                    state.bindings = { m_BindingSets[Bindings_CBTReadWrite], m_BindingSets[Bindings_Constants] };
                    m_CommandList->setComputeState(state);

                    int cnt = ((1 << it) >> 5);
                    int numGroup = (cnt >= 256) ? (cnt >> 8) : 1;

                    uint2 constants = { static_cast<uint>(it), 0 };
                    m_CommandList->setPushConstants(&constants, sizeof(constants));

                    nvrhi::utils::BufferUavBarrier(m_CommandList, m_CBTBuffer);
                    m_CommandList->commitBarriers();

                    m_CommandList->dispatch(static_cast<uint32_t>(numGroup));

                    it -= 5;
                }

                state.pipeline = m_ComputePipelines[Pipeline_CBT_SumReduction];
                state.bindings = { m_BindingSets[Bindings_CBTReadWrite], m_BindingSets[Bindings_Constants] };
                m_CommandList->setComputeState(state);

                while (--it >= 0) 
                {
                    int cnt = 1 << it;
                    int numGroup = (cnt >= 256) ? (cnt >> 8) : 1;

                    uint2 constants = { static_cast<uint>(it), 0 };
                    m_CommandList->setPushConstants(&constants, sizeof(constants));

                    nvrhi::utils::BufferUavBarrier(m_CommandList, m_CBTBuffer);
                    m_CommandList->commitBarriers();

                    m_CommandList->dispatch(static_cast<uint32_t>(numGroup));
                }

                StopTimer(Timer_SumReduction);
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
        StartTimer(Timer_DrawLEB);

        {
            nvrhi::ComputeState state;
            state.pipeline = m_ComputePipelines[Pipeline_LEB_Dispatcher];
            state.bindings = { m_BindingSets[Bindings_CBTReadOnly], m_BindingSets[Bindings_IndirectArgs] };
            m_CommandList->setComputeState(state);

            m_CommandList->dispatch(1);
        }

        {
            nvrhi::GraphicsState state;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
            state.pipeline = m_GraphicsPipelines[m_UI.DisplayMode == DisplayMode_Wireframe ? Pipeline_Triangles_Wireframe : Pipeline_Triangles_Fill];
            state.bindings = { m_BindingSets[Bindings_CBTReadOnly] };
            state.indirectParams = m_IndirectArgsBuffer;
            m_CommandList->setGraphicsState(state);

            m_CommandList->drawIndirect(offsetof(IndirectArgs, DrawArgs));
        }

        StopTimer(Timer_DrawLEB);
    	m_CommandList->endMarker();
    }

    void DrawTarget(nvrhi::IFramebuffer* framebuffer)
    {
        m_CommandList->beginMarker("Draw Target");

        nvrhi::GraphicsState state;
        state.framebuffer = framebuffer;
        state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
        state.pipeline = m_GraphicsPipelines[Pipeline_Target];
        state.bindings = { m_BindingSets[Bindings_Constants] };
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
        // Check if any timers have data available
        {
            const auto& timers = m_Timers.at(m_TimerSetIndex);
            for (size_t timerIndex = 0; timerIndex < timers.size(); timerIndex++)
            {
                const auto& timer = timers[timerIndex];
	            if (GetDevice()->pollTimerQuery(timer))
	            {
                    
	            }
            }
        }
        m_TimerSetIndex = (m_TimerSetIndex + 1) % m_Timers.size();

        if (std::ranges::any_of(m_GraphicsPipelines, [](const auto& pipeline) { return !pipeline; }))
        {
            CreateGraphicsPipelines(framebuffer);
        }

        m_CommandList->open();

        {
            std::string frameMarker = "Frame " + std::to_string(GetFrameIndex());
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
        m_UI.CBTFlags[CBT_Bit_Create] = ImGui::SliderInt("MaxDepth", &m_UI.CBTMaxDepth, 6, 24);
        m_UI.CBTFlags[CBT_Bit_Reset] = ImGui::Button("Reset");

        ImGui::Separator();

        if (m_UI.Backend == Backend_GPU)
        {
            ImGui::LabelText("Subdivision (GPU)", "%.3f ms", m_UI.TimerData[Timer_Subdivision]);
            ImGui::LabelText("Sum Reduction (GPU)", "%.3f ms", m_UI.TimerData[Timer_SumReduction]);
            ImGui::LabelText("Draw LEB (GPU)", "%.3f ms", m_UI.TimerData[Timer_DrawLEB]);
        }

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
