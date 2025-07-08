#include <donut/app/ApplicationBase.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_renderer.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>

#define CBT_IMPLEMENTATION
#include "cbt.h"

#define LEB_IMPLEMENTATION
#include "leb.h"

using namespace donut;
using namespace donut::math;

static const char* g_WindowTitle = "Concurrent Binary Tree";

struct UIData
{
    inline static constexpr uint s_CBTInitDepth = 1;
    inline static constexpr uint s_CBTMaxDepth = 20;

	int Backend = 0;

	float2 Target{ 0.25f, 0.75f };
    int MaxDepth = s_CBTInitDepth;

    cbt_Tree* CBT = nullptr;
};

class CBTSubdivision : public app::IRenderPass
{
private:
	UIData& m_UI;

    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;

    nvrhi::ShaderHandle m_TestTriangleVertexShader;
    nvrhi::ShaderHandle m_TestTrianglePixelShader;
    nvrhi::ShaderHandle m_TriangleVertexShader;
    nvrhi::ShaderHandle m_TrianglePixelShader;
    nvrhi::ShaderHandle m_TargetVertexShader;
    nvrhi::ShaderHandle m_TargetPixelShader;

    nvrhi::BufferHandle m_CBTBuffer;

    enum CBTBindings
    {
	    CBTBinding_ReadOnly,
        CBTBinding_ReadWrite,
        CBTBinding_COUNT
    };
    nvrhi::BindingLayoutHandle m_CBTBindingLayouts[CBTBinding_COUNT];
    nvrhi::BindingSetHandle m_CBTBindingSets[CBTBinding_COUNT];

    nvrhi::BindingLayoutHandle m_ConstantsBindingLayout;
    nvrhi::BindingSetHandle m_ConstantsBindingSet;

    nvrhi::CommandListHandle m_CommandList;

    enum GraphicsPipelines
    {
	    Pipeline_Test_Triangles_Solid = 0,
	    Pipeline_Test_Triangles_Wireframe,
        Pipeline_Triangles_Wireframe,
        Pipeline_Target,
        Pipeline_COUNT
    };
    nvrhi::GraphicsPipelineHandle m_Pipelines[Pipeline_COUNT];

public:
    using IRenderPass::IRenderPass;

    CBTSubdivision(app::DeviceManager* deviceManager, UIData& ui)
	    : IRenderPass(deviceManager)
		, m_UI(ui)
    {
    }

    ~CBTSubdivision()
    {
        if (m_UI.CBT)
        {
            cbt_Release(m_UI.CBT);
        }
    }

    const std::shared_ptr<engine::ShaderFactory>& GetShaderFactory() const { return m_ShaderFactory; }

    bool Init()
    {
        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();

        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/cbt" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        std::shared_ptr<vfs::RootFileSystem> rootFS = std::make_shared<vfs::RootFileSystem>();
        rootFS->mount("/shaders/donut", frameworkShaderPath);
        rootFS->mount("/shaders/app", appShaderPath);

        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), rootFS, "/shaders");

        m_TriangleVertexShader = m_ShaderFactory->CreateShader("app/triangles.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_TrianglePixelShader = m_ShaderFactory->CreateShader("app/triangles.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_TestTriangleVertexShader = m_ShaderFactory->CreateShader("app/shaders.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_TestTrianglePixelShader = m_ShaderFactory->CreateShader("app/shaders.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_TargetVertexShader = m_ShaderFactory->CreateShader("app/target.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_TargetPixelShader = m_ShaderFactory->CreateShader("app/target.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);

        if (!m_TriangleVertexShader || !m_TrianglePixelShader
         || !m_TestTriangleVertexShader || !m_TestTrianglePixelShader
         || !m_TargetVertexShader || !m_TargetPixelShader)
        {
            return false;
        }

        m_UI.CBT = cbt_CreateAtDepth(m_UI.s_CBTMaxDepth, m_UI.MaxDepth);

        {
            uint64_t byteSize = cbt_HeapByteSize(m_UI.CBT);

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.setByteSize(byteSize)
				.setCanHaveRawViews(true)
				.setCanHaveUAVs(true)
				.setInitialState(nvrhi::ResourceStates::Common)
				.setKeepInitialState(true)
				.setDebugName("CBT");
	        m_CBTBuffer = GetDevice()->createBuffer(bufferDesc);


        }

        {
            nvrhi::BindingLayoutDesc layoutDesc;
            nvrhi::BindingSetDesc setDesc;

            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(0));
            setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_SRV(0, m_CBTBuffer));

            m_CBTBindingLayouts[CBTBinding_ReadOnly] = GetDevice()->createBindingLayout(layoutDesc);
            m_CBTBindingSets[CBTBinding_ReadOnly] = GetDevice()->createBindingSet(setDesc, m_CBTBindingLayouts[CBTBinding_ReadOnly]);


            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0));
            setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, m_CBTBuffer));

            m_CBTBindingLayouts[CBTBinding_ReadWrite] = GetDevice()->createBindingLayout(layoutDesc);
            m_CBTBindingSets[CBTBinding_ReadWrite] = GetDevice()->createBindingSet(setDesc, m_CBTBindingLayouts[CBTBinding_ReadWrite]);
        }

        {
	        nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex)
				.addItem(nvrhi::BindingLayoutItem::PushConstants(0, 2 * sizeof(uint)));

            m_ConstantsBindingLayout = GetDevice()->createBindingLayout(layoutDesc);

            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, 2 * sizeof(uint)));

            m_ConstantsBindingSet = GetDevice()->createBindingSet(setDesc, m_ConstantsBindingLayout);
        }

        m_CommandList = GetDevice()->createCommandList();
        m_CommandList->open();

        {
	        // Upload CBT to GPU
            m_CommandList->beginTrackingBufferState(m_CBTBuffer, nvrhi::ResourceStates::CopyDest);
            m_CommandList->writeBuffer(m_CBTBuffer, cbt_GetHeap(m_UI.CBT), cbt_HeapByteSize(m_UI.CBT));
            m_CommandList->setBufferState(m_CBTBuffer, nvrhi::ResourceStates::ShaderResource);
        }

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);

        return true;
    }

    void BackBufferResizing() override
    {
        for (auto& pipeline : m_Pipelines)
			pipeline = nullptr;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }
    
    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        if (!m_Pipelines[0])
        {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;

            {
                psoDesc.VS = m_TestTriangleVertexShader;
                psoDesc.PS = m_TestTrianglePixelShader;
                psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
                psoDesc.bindingLayouts = { m_ConstantsBindingLayout };

                m_Pipelines[Pipeline_Test_Triangles_Solid] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);

                psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
                m_Pipelines[Pipeline_Test_Triangles_Wireframe] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
            }
            {
                psoDesc.VS = m_TriangleVertexShader;
                psoDesc.PS = m_TrianglePixelShader;
                psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
                psoDesc.bindingLayouts = { m_CBTBindingLayouts[CBTBinding_ReadOnly]};
                psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;

                m_Pipelines[Pipeline_Triangles_Wireframe] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
            }
            {
                psoDesc.VS = m_TargetVertexShader;
                psoDesc.PS = m_TargetPixelShader;
                psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
                psoDesc.bindingLayouts = { m_ConstantsBindingLayout };
                psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Fill;

                m_Pipelines[Pipeline_Target] = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
            }
        }

        m_CommandList->open();

        nvrhi::utils::ClearColorAttachment(m_CommandList, framebuffer, 0, nvrhi::Color(1.f));

        nvrhi::GraphicsState state;
        state.framebuffer = framebuffer;
        state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
        state.bindings = { m_ConstantsBindingSet };

        nvrhi::DrawArguments args;
        if (false)
        {
            state.pipeline = m_Pipelines[Pipeline_Test_Triangles_Wireframe];
            m_CommandList->setGraphicsState(state);

            uint2 wireframe{ 1, 0 };
            m_CommandList->setPushConstants(&wireframe, sizeof(wireframe));

	        args.vertexCount = 3;
            args.instanceCount = static_cast<uint32_t>(cbt_NodeCount(m_UI.CBT));
            m_CommandList->draw(args);
        }

        {
            state.pipeline = m_Pipelines[Pipeline_Triangles_Wireframe];
            state.bindings = { m_CBTBindingSets[CBTBinding_ReadOnly] };
            m_CommandList->setGraphicsState(state);

            args.vertexCount = 3;
            args.instanceCount = static_cast<uint32_t>(cbt_NodeCount(m_UI.CBT));
            m_CommandList->draw(args);
        }

        {
            state.pipeline = m_Pipelines[Pipeline_Target];
            state.bindings = { m_ConstantsBindingSet };
            m_CommandList->setGraphicsState(state);

            float2 target = m_UI.Target * 2.0f - 1.0f;
            target.y = -target.y;
            m_CommandList->setPushConstants(&target, sizeof(target));

            args.vertexCount = 4;
            m_CommandList->draw(args);
        }

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
    }

};

class UIRenderer : public app::ImGui_Renderer
{
private:
    UIData& m_UI;

public:
    UIRenderer(app::DeviceManager* deviceManager, UIData& ui)
	    : ImGui_Renderer(deviceManager)
		, m_UI(ui)
    {
    }

protected:
    virtual void buildUI() override
    {
	    ImGui::Begin("Options");

        const char* eBackends[] = { "CPU", "GPU" };

        ImGui::Combo("Backend", &m_UI.Backend, &eBackends[0], 2);
        ImGui::SliderFloat("TargetX", &m_UI.Target.x, 0, 1);
        ImGui::SliderFloat("TargetY", &m_UI.Target.y, 0, 1);
        ImGui::SliderInt("MaxDepth", &m_UI.MaxDepth, 6, 20);
        ImGui::Button("Reset");

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
        UIRenderer uiRenderer(deviceManager, ui);
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
