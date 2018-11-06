/***************************************************************************
# Copyright (c) 2015, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************************************************************************/
#include "Framework.h"
#include "API/RenderContext.h"
#include "API/Device.h"
#include "glm/gtc/type_ptr.hpp"
#include "D3D12Resource.h"
#include "API/D3D12/D3D12State.h"
#include "API/DescriptorSet.h"
#include "Raytracing/RtProgramVars.h"
#include "Raytracing/RtState.h"

namespace Falcor
{
    static void initBlitData()
    {
        if (gBlitData.pVars == nullptr)
        {
            gBlitData.pPass = FullScreenPass::create("Framework/Shaders/Blit.vs.slang", "Framework/Shaders/Blit.ps.slang");
            gBlitData.pVars = GraphicsVars::create(gBlitData.pPass->getProgram()->getReflector());
            gBlitData.pState = GraphicsState::create();

            gBlitData.pSrcRectBuffer = gBlitData.pVars->getConstantBuffer("SrcRectCB");
            gBlitData.offsetVarOffset = (uint32_t)gBlitData.pSrcRectBuffer->getVariableOffset("gOffset");
            gBlitData.scaleVarOffset = (uint32_t)gBlitData.pSrcRectBuffer->getVariableOffset("gScale");
            gBlitData.prevSrcRectOffset = vec2(-1.0f);
            gBlitData.prevSrcReftScale = vec2(-1.0f);

            Sampler::Desc desc;
            desc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
            gBlitData.pLinearSampler = Sampler::create(desc);
            desc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
            gBlitData.pPointSampler = Sampler::create(desc);
            const auto& pDefaultBlockReflection = gBlitData.pPass->getProgram()->getReflector()->getDefaultParameterBlock();
            gBlitData.texBindLoc = pDefaultBlockReflection->getResourceBinding("gTex");
            gBlitData.samplerBindLoc = pDefaultBlockReflection->getResourceBinding("gSampler");
        }
    }

    void releaseApiData()
    {
        gBlitData.pSrcRectBuffer = nullptr;
        gBlitData.pVars = nullptr;
        gBlitData.pPass = nullptr;
        gBlitData.pState = nullptr;
        gpDrawCommandSig = nullptr;
        gpDrawIndexCommandSig = nullptr;
    }

    RenderContext::~RenderContext()
    {
        releaseApiData();
    }


    RenderContext::SharedPtr RenderContext::create(CommandQueueHandle queue)
    {
        SharedPtr pCtx = SharedPtr(new RenderContext());
        pCtx->mpLowLevelData = LowLevelContextData::create(LowLevelContextData::CommandQueueType::Direct, queue);
        if (pCtx->mpLowLevelData == nullptr)
        {
            return nullptr;
        }
        return pCtx;
    }
    
    void RenderContext::clearRtv(const RenderTargetView* pRtv, const glm::vec4& color)
    {
        resourceBarrier(pRtv->getResource(), Resource::State::RenderTarget);
        mpLowLevelData->getCommandList()->ClearRenderTargetView(pRtv->getApiHandle()->getCpuHandle(0), glm::value_ptr(color), 0, nullptr);
        mCommandsPending = true;
    }

    void RenderContext::clearDsv(const DepthStencilView* pDsv, float depth, uint8_t stencil, bool clearDepth, bool clearStencil)
    {
        uint32_t flags = clearDepth ? D3D12_CLEAR_FLAG_DEPTH : 0;
        flags |= clearStencil ? D3D12_CLEAR_FLAG_STENCIL : 0;

        resourceBarrier(pDsv->getResource(), Resource::State::DepthStencil);
        mpLowLevelData->getCommandList()->ClearDepthStencilView(pDsv->getApiHandle()->getCpuHandle(0), D3D12_CLEAR_FLAGS(flags), depth, stencil, 0, nullptr);
        mCommandsPending = true;
    }

    static void D3D12SetVao(RenderContext* pCtx, ID3D12GraphicsCommandList* pList, const Vao* pVao)
    {
        D3D12_VERTEX_BUFFER_VIEW vb[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        D3D12_INDEX_BUFFER_VIEW ib = {};

        if (pVao)
        {
            // Get the vertex buffers
            for (uint32_t i = 0; i < pVao->getVertexBuffersCount(); i++)
            {
                const Buffer* pVB = pVao->getVertexBuffer(i).get();
                if (pVB)
                {
                    vb[i].BufferLocation = pVB->getGpuAddress();
                    vb[i].SizeInBytes = (uint32_t)pVB->getSize();
                    vb[i].StrideInBytes = pVao->getVertexLayout()->getBufferLayout(i)->getStride();
                    pCtx->resourceBarrier(pVB, Resource::State::VertexBuffer);
                }
            }

            const Buffer* pIB = pVao->getIndexBuffer().get();
            if (pIB)
            {
                ib.BufferLocation = pIB->getGpuAddress();
                ib.SizeInBytes = (uint32_t)pIB->getSize();
                ib.Format = getDxgiFormat(pVao->getIndexBufferFormat());
                pCtx->resourceBarrier(pIB, Resource::State::IndexBuffer);
            }
        }

        pList->IASetVertexBuffers(0, arraysize(vb), vb);
        pList->IASetIndexBuffer(&ib);
    }

    static void D3D12SetFbo(RenderContext* pCtx, const Fbo* pFbo)
    {
        // We are setting the entire RTV array to make sure everything that was previously bound is detached
        uint32_t colorTargets = Fbo::getMaxColorTargetCount();
        auto pNullRtv = RenderTargetView::getNullView();
        std::vector<HeapCpuHandle> pRTV(colorTargets, pNullRtv->getApiHandle()->getCpuHandle(0));
        HeapCpuHandle pDSV = DepthStencilView::getNullView()->getApiHandle()->getCpuHandle(0);

        if (pFbo)
        {
            for (uint32_t i = 0; i < colorTargets; i++)
            {
                auto& pTexture = pFbo->getColorTexture(i);
                if (pTexture)
                {
                    pRTV[i] = pFbo->getRenderTargetView(i)->getApiHandle()->getCpuHandle(0);
                    pCtx->resourceBarrier(pTexture.get(), Resource::State::RenderTarget);
                }
            }

            auto& pTexture = pFbo->getDepthStencilTexture();
            if(pTexture)
            {
                pDSV = pFbo->getDepthStencilView()->getApiHandle()->getCpuHandle(0);
                if (pTexture)
                {
                    pCtx->resourceBarrier(pTexture.get(), Resource::State::DepthStencil);
                }
            }
        }
        ID3D12GraphicsCommandList* pCmdList = pCtx->getLowLevelData()->getCommandList().GetInterfacePtr();
        pCmdList->OMSetRenderTargets(colorTargets, pRTV.data(), FALSE, &pDSV);
    }

    static void D3D12SetSamplePositions(ID3D12GraphicsCommandList* pList, const Fbo* pFbo)
    {
        if (!pFbo) return;
        ID3D12GraphicsCommandList1* pList1;
        pList->QueryInterface(IID_PPV_ARGS(&pList1));

        bool featureSupported = gpDevice->isFeatureSupported(Device::SupportedFeatures::ProgrammableSamplePositionsPartialOnly) ||
                                gpDevice->isFeatureSupported(Device::SupportedFeatures::ProgrammableSamplePositionsFull);

        const auto& samplePos = pFbo->getSamplePositions();

#if _LOG_ENABLED
        if (featureSupported == false && samplePos.size() > 0)
        {
            logError("The FBO specifies programmable sample positions, but the hardware does not support it");
        }
        else if (gpDevice->isFeatureSupported(Device::SupportedFeatures::ProgrammableSamplePositionsPartialOnly) && samplePos.size() > 1)
        {
            logError("The FBO specifies multiple programmable sample positions, but the hardware only supports one");
        }
#endif
        if(featureSupported)
        {
            static_assert(offsetof(Fbo::SamplePosition, xOffset) == offsetof(D3D12_SAMPLE_POSITION, X), "SamplePosition.X");
            static_assert(offsetof(Fbo::SamplePosition, yOffset) == offsetof(D3D12_SAMPLE_POSITION, Y), "SamplePosition.Y");

            if (samplePos.size())
            {
                pList1->SetSamplePositions(pFbo->getSampleCount(), pFbo->getSamplePositionsPixelCount(), (D3D12_SAMPLE_POSITION*)samplePos.data());
            }
            else
            {
                pList1->SetSamplePositions(0, 0, nullptr);
            }
        }
    }

    static void D3D12SetViewports(ID3D12GraphicsCommandList* pList, const GraphicsState::Viewport* vp)
    {
        static_assert(offsetof(GraphicsState::Viewport, originX) == offsetof(D3D12_VIEWPORT, TopLeftX), "VP originX offset");
        static_assert(offsetof(GraphicsState::Viewport, originY) == offsetof(D3D12_VIEWPORT, TopLeftY), "VP originY offset");
        static_assert(offsetof(GraphicsState::Viewport, width) == offsetof(D3D12_VIEWPORT, Width), "VP Width offset");
        static_assert(offsetof(GraphicsState::Viewport, height) == offsetof(D3D12_VIEWPORT, Height), "VP Height offset");
        static_assert(offsetof(GraphicsState::Viewport, minDepth) == offsetof(D3D12_VIEWPORT, MinDepth), "VP MinDepth offset");
        static_assert(offsetof(GraphicsState::Viewport, maxDepth) == offsetof(D3D12_VIEWPORT, MaxDepth), "VP TopLeftX offset");

        pList->RSSetViewports(D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, (D3D12_VIEWPORT*)vp);
    }

    static void D3D12SetScissors(ID3D12GraphicsCommandList* pList, const GraphicsState::Scissor* sc)
    {
        static_assert(offsetof(GraphicsState::Scissor, left) == offsetof(D3D12_RECT, left), "Scissor.left offset");
        static_assert(offsetof(GraphicsState::Scissor, top) == offsetof(D3D12_RECT, top), "Scissor.top offset");
        static_assert(offsetof(GraphicsState::Scissor, right) == offsetof(D3D12_RECT, right), "Scissor.right offset");
        static_assert(offsetof(GraphicsState::Scissor, bottom) == offsetof(D3D12_RECT, bottom), "Scissor.bottom offset");

        pList->RSSetScissorRects(D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, (D3D12_RECT*)sc);
    }

    void RenderContext::prepareForDraw()
    {
        assert(mpGraphicsState);
        // Vao must be valid so at least primitive topology is known
        assert(mpGraphicsState->getVao().get());

        if (is_set(StateBindFlags::Vars, mBindFlags))
        {
            // Apply the vars. Must be first because applyGraphicsVars() might cause a flush
            if (mpGraphicsVars)
            {
                applyGraphicsVars();
            }
            else
            {
                mpLowLevelData->getCommandList()->SetGraphicsRootSignature(RootSignature::getEmpty()->getApiHandle());
            }
        }

#if _ENABLE_NVAPI
        if (mpGraphicsState->isSinglePassStereoEnabled())
        {
            NvAPI_Status ret = NvAPI_D3D12_SetSinglePassStereoMode(mpLowLevelData->getCommandList(), 2, 1, false);
            assert(ret == NVAPI_OK);
        }
#else
        assert(mpGraphicsState->isSinglePassStereoEnabled() == false);
#endif

        mBindGraphicsRootSig = false;

        ID3D12GraphicsCommandList* pList = mpLowLevelData->getCommandList();
        if (is_set(StateBindFlags::Topology, mBindFlags))
        {
            pList->IASetPrimitiveTopology(getD3DPrimitiveTopology(mpGraphicsState->getVao()->getPrimitiveTopology()));
        }
        if (is_set(StateBindFlags::Vao, mBindFlags))
        {
            D3D12SetVao(this, pList, mpGraphicsState->getVao().get());
        }
        if (is_set(StateBindFlags::Fbo, mBindFlags))
        {
            D3D12SetFbo(this, mpGraphicsState->getFbo().get());
        }
        if (is_set(StateBindFlags::SamplePositions, mBindFlags))
        {
            D3D12SetSamplePositions(pList, mpGraphicsState->getFbo().get());
        }
        if (is_set(StateBindFlags::Viewports, mBindFlags))
        {
            D3D12SetViewports(pList, &mpGraphicsState->getViewport(0));
        }
        if (is_set(StateBindFlags::Scissors, mBindFlags))
        {
            D3D12SetScissors(pList, &mpGraphicsState->getScissors(0));
        }
        if (is_set(StateBindFlags::PipelineState, mBindFlags))
        {
            pList->SetPipelineState(mpGraphicsState->getGSO(mpGraphicsVars.get())->getApiHandle());
        }

        BlendState::SharedPtr blendState = mpGraphicsState->getBlendState();
        if (blendState != nullptr)
        {
            pList->OMSetBlendFactor(glm::value_ptr(blendState->getBlendFactor()));
        }

        const auto pDsState = mpGraphicsState->getDepthStencilState();
        pList->OMSetStencilRef(pDsState == nullptr ? 0 : pDsState->getStencilRef());

        mCommandsPending = true;
    }

    void RenderContext::drawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation)
    {
        prepareForDraw();
        mpLowLevelData->getCommandList()->DrawInstanced(vertexCount, instanceCount, startVertexLocation, startInstanceLocation);
    }

    void RenderContext::draw(uint32_t vertexCount, uint32_t startVertexLocation)
    {
        drawInstanced(vertexCount, 1, startVertexLocation, 0);
    }

    void RenderContext::drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation)
    {
        prepareForDraw();
        mpLowLevelData->getCommandList()->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
    }

    void RenderContext::drawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
    {
        drawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);
    }

    void RenderContext::drawIndirect(const Buffer* argBuffer, uint64_t argBufferOffset)
    {
        prepareForDraw();
        resourceBarrier(argBuffer, Resource::State::IndirectArg);
        mpLowLevelData->getCommandList()->ExecuteIndirect(gpDrawCommandSig, 1, argBuffer->getApiHandle(), argBufferOffset, nullptr, 0);
    }

    void RenderContext::drawIndexedIndirect(const Buffer* argBuffer, uint64_t argBufferOffset)
    {
        prepareForDraw();
        resourceBarrier(argBuffer, Resource::State::IndirectArg);
        mpLowLevelData->getCommandList()->ExecuteIndirect(gpDrawIndexCommandSig, 1, argBuffer->getApiHandle(), argBufferOffset, nullptr, 0);
    }

    void RenderContext::raytrace(RtProgramVars::SharedPtr pVars, RtState::SharedPtr pState, uint32_t width, uint32_t height)
    {
        raytrace(pVars, pState, width, height, 1);
    }

    void RenderContext::raytrace(RtProgramVars::SharedPtr pVars, RtState::SharedPtr pState, uint32_t width, uint32_t height, uint32_t depth)
    {
        resourceBarrier(pVars->getShaderTable().get(), Resource::State::NonPixelShader);

        Buffer* pShaderTable = pVars->getShaderTable().get();
        uint32_t recordSize = pVars->getRecordSize();
        D3D12_GPU_VIRTUAL_ADDRESS startAddress = pShaderTable->getGpuAddress();

        D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
        raytraceDesc.Width = width;
        raytraceDesc.Height = height;
        raytraceDesc.Depth = depth;

        // RayGen is the first entry in the shader-table
        raytraceDesc.RayGenerationShaderRecord.StartAddress = startAddress + pVars->getRayGenRecordIndex() * recordSize;
        raytraceDesc.RayGenerationShaderRecord.SizeInBytes = recordSize;

        // Miss is the second entry in the shader-table
        raytraceDesc.MissShaderTable.StartAddress = startAddress + pVars->getFirstMissRecordIndex() * recordSize;
        raytraceDesc.MissShaderTable.StrideInBytes = recordSize;
        raytraceDesc.MissShaderTable.SizeInBytes = recordSize * pVars->getMissProgramsCount();

        raytraceDesc.HitGroupTable.StartAddress = startAddress + pVars->getFirstHitRecordIndex() * recordSize;
        raytraceDesc.HitGroupTable.StrideInBytes = recordSize;
        raytraceDesc.HitGroupTable.SizeInBytes = recordSize * pVars->getHitRecordsCount();
        assert(pVars->getShaderTable()->getSize() >= (pVars->getFirstHitRecordIndex() * recordSize) + raytraceDesc.HitGroupTable.SizeInBytes);  // Check that the buffer is sufficiently large to hold the shader table

        auto pCmdList = getLowLevelData()->getCommandList();
        pCmdList->SetComputeRootSignature(pVars->getGlobalVars()->getRootSignature()->getApiHandle().GetInterfacePtr());

        // Dispatch
        GET_COM_INTERFACE(pCmdList, ID3D12GraphicsCommandList4, pList4);
        pList4->SetPipelineState1(pState->getRtso()->getApiHandle().GetInterfacePtr());
        pList4->DispatchRays(&raytraceDesc);
    }

    void RenderContext::initDrawCommandSignatures()
    {
        //Common properties
        D3D12_COMMAND_SIGNATURE_DESC sigDesc;
        sigDesc.NumArgumentDescs = 1;
        sigDesc.NodeMask = 0;
        D3D12_INDIRECT_ARGUMENT_DESC argDesc;

        //Draw 
        sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        sigDesc.pArgumentDescs = &argDesc;
        gpDevice->getApiHandle()->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&gpDrawCommandSig));

        //Draw index
        sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        sigDesc.pArgumentDescs = &argDesc;
        gpDevice->getApiHandle()->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&gpDrawIndexCommandSig));
    }

    void RenderContext::blit(ShaderResourceView::SharedPtr pSrc, RenderTargetView::SharedPtr pDst, const uvec4& srcRect, const uvec4& dstRect, Sampler::Filter filter)
    {
        initBlitData(); // This has to be here and can't be in the constructor. FullScreenPass will allocate some buffers which depends on the ResourceAllocator which depends on the fence inside the RenderContext. Dependencies are fun!
        if (filter == Sampler::Filter::Linear)
        {
            gBlitData.pVars->getDefaultBlock()->setSampler(gBlitData.samplerBindLoc, 0, gBlitData.pLinearSampler);
        }
        else
        {
            gBlitData.pVars->getDefaultBlock()->setSampler(gBlitData.samplerBindLoc, 0, gBlitData.pPointSampler);
        }

        assert(pSrc->getViewInfo().arraySize == 1 && pSrc->getViewInfo().mipCount == 1);
        assert(pDst->getViewInfo().arraySize == 1 && pDst->getViewInfo().mipCount == 1);

        const Texture* pSrcTexture = dynamic_cast<const Texture*>(pSrc->getResource());
        const Texture* pDstTexture = dynamic_cast<const Texture*>(pDst->getResource());
        assert(pSrcTexture != nullptr && pDstTexture != nullptr);

        vec2 srcRectOffset(0.0f);
        vec2 srcRectScale(1.0f);
        uint32_t srcMipLevel = pSrc->getViewInfo().mostDetailedMip;
        uint32_t dstMipLevel = pDst->getViewInfo().mostDetailedMip;
        GraphicsState::Viewport dstViewport(0.0f, 0.0f, (float)pDstTexture->getWidth(dstMipLevel), (float)pDstTexture->getHeight(dstMipLevel), 0.0f, 1.0f);

        // If src rect specified
        if (srcRect.x != (uint32_t)-1)
        {
            const vec2 srcSize(pSrcTexture->getWidth(srcMipLevel), pSrcTexture->getHeight(srcMipLevel));
            srcRectOffset = vec2(srcRect.x, srcRect.y) / srcSize;
            srcRectScale = vec2(srcRect.z - srcRect.x, srcRect.w - srcRect.y) / srcSize;
        }

        // If dest rect specified
        if (dstRect.x != (uint32_t)-1)
        {
            dstViewport = GraphicsState::Viewport((float)dstRect.x, (float)dstRect.y, (float)(dstRect.z - dstRect.x), (float)(dstRect.w - dstRect.y), 0.0f, 1.0f);
        }

        // Update buffer/state
        if (srcRectOffset != gBlitData.prevSrcRectOffset)
        {
            gBlitData.pSrcRectBuffer->setVariable(gBlitData.offsetVarOffset, srcRectOffset);
            gBlitData.prevSrcRectOffset = srcRectOffset;
        }

        if (srcRectScale != gBlitData.prevSrcReftScale)
        {
            gBlitData.pSrcRectBuffer->setVariable(gBlitData.scaleVarOffset, srcRectScale);
            gBlitData.prevSrcReftScale = srcRectScale;
        }

        gBlitData.pState->setViewport(0, dstViewport);

        pushGraphicsState(gBlitData.pState);
        pushGraphicsVars(gBlitData.pVars);

        if (pSrcTexture->getSampleCount() > 1)
        {
            gBlitData.pPass->getProgram()->addDefine("SAMPLE_COUNT", std::to_string(pSrcTexture->getSampleCount()));
        }
        else
        {
            gBlitData.pPass->getProgram()->removeDefine("SAMPLE_COUNT");
        }

        Fbo::SharedPtr pFbo = Fbo::create();
        Texture::SharedPtr pSharedTex = std::const_pointer_cast<Texture>(pDstTexture->shared_from_this());
        pFbo->attachColorTarget(pSharedTex, 0, pDst->getViewInfo().mostDetailedMip, pDst->getViewInfo().firstArraySlice, pDst->getViewInfo().arraySize);
        gBlitData.pState->pushFbo(pFbo, false);
        gBlitData.pVars->getDefaultBlock()->setSrv(gBlitData.texBindLoc, 0, pSrc);
        gBlitData.pPass->execute(this);

        // Release the resources we bound
        gBlitData.pVars->getDefaultBlock()->setSrv(gBlitData.texBindLoc, 0, nullptr);
        gBlitData.pState->popFbo(false);
        popGraphicsState();
        popGraphicsVars();
    }

    void RenderContext::resolveSubresource(const Texture::SharedPtr& pSrc, uint32_t srcSubresource, const Texture::SharedPtr& pDst, uint32_t dstSubresource)
    {
        DXGI_FORMAT format = getDxgiFormat(pDst->getFormat());
        mpLowLevelData->getCommandList()->ResolveSubresource(pDst->getApiHandle(), dstSubresource, pSrc->getApiHandle(), srcSubresource, format);
        mCommandsPending = true;
    }

    void RenderContext::resolveResource(const Texture::SharedPtr& pSrc, const Texture::SharedPtr& pDst)
    {
        bool match = true;
        match = match && (pSrc->getMipCount() == pDst->getMipCount());
        match = match && (pSrc->getArraySize() == pDst->getArraySize());
        if (!match)
        {
            logWarning("Can't resolve a resource. The src and dst textures have a different array-size or mip-count");
        }

        resourceBarrier(pSrc.get(), Resource::State::ResolveSource);
        resourceBarrier(pDst.get(), Resource::State::ResolveDest);

        uint32_t subresourceCount = pSrc->getMipCount() * pSrc->getArraySize();
        for (uint32_t s = 0; s < subresourceCount; s++)
        {
            resolveSubresource(pSrc, s, pDst, s);
        }
    }
}
