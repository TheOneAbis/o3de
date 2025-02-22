/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Atom/Feature/Mesh/MeshFeatureProcessor.h>
#include <Atom/RHI/BufferFrameAttachment.h>
#include <Atom/RHI/BufferScopeAttachment.h>
#include <Atom/RHI/CommandList.h>
#include <Atom/RHI/FrameScheduler.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/ScopeProducerFunction.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Scene.h>
#include <RayTracing/RayTracingAccelerationStructurePass.h>
#include <RayTracing/RayTracingFeatureProcessor.h>

namespace AZ
{
    namespace Render
    {
        RPI::Ptr<RayTracingAccelerationStructurePass> RayTracingAccelerationStructurePass::Create(const RPI::PassDescriptor& descriptor)
        {
            RPI::Ptr<RayTracingAccelerationStructurePass> rayTracingAccelerationStructurePass = aznew RayTracingAccelerationStructurePass(descriptor);
            return AZStd::move(rayTracingAccelerationStructurePass);
        }

        RayTracingAccelerationStructurePass::RayTracingAccelerationStructurePass(const RPI::PassDescriptor& descriptor)
            : Pass(descriptor)
        {
            // disable this pass if we're on a platform that doesn't support raytracing
            if (RHI::RHISystemInterface::Get()->GetRayTracingSupport() == RHI::MultiDevice::NoDevices)
            {
                SetEnabled(false);
            }
        }

        void RayTracingAccelerationStructurePass::BuildInternal()
        {
            const auto deviceMask{ RHI::RHISystemInterface::Get()->GetRayTracingSupport() };
            const auto deviceCount{ RHI::RHISystemInterface::Get()->GetDeviceCount() };
            for (auto deviceIndex{ 0 }; deviceIndex < deviceCount; ++deviceIndex)
            {
                if ((AZStd::to_underlying(deviceMask) >> deviceIndex) & 1)
                {
                    m_scopeProducers[deviceIndex] = AZStd::make_shared<RHI::ScopeProducerFunctionNoData>(
                        RHI::ScopeId{ AZStd::string(GetPathName().GetCStr() + AZStd::to_string(deviceIndex)) },
                        AZStd::bind(&RayTracingAccelerationStructurePass::SetupFrameGraphDependencies, this, AZStd::placeholders::_1),
                        [](const RHI::FrameGraphCompileContext&)
                        {
                        },
                        AZStd::bind(&RayTracingAccelerationStructurePass::BuildCommandList, this, AZStd::placeholders::_1),
                        RHI::HardwareQueueClass::Compute,
                        deviceIndex);
                }
            }
        }

        void RayTracingAccelerationStructurePass::FrameBeginInternal(FramePrepareParams params)
        {
            const auto deviceMask{ RHI::RHISystemInterface::Get()->GetRayTracingSupport() };
            const auto deviceCount{ RHI::RHISystemInterface::Get()->GetDeviceCount() };
            for (auto deviceIndex{ 0 }; deviceIndex < deviceCount; ++deviceIndex)
            {
                if ((AZStd::to_underlying(deviceMask) >> deviceIndex) & 1)
                {
                    params.m_frameGraphBuilder->ImportScopeProducer(*m_scopeProducers[deviceIndex]);
                }
            }

            ReadbackScopeQueryResults();

            RPI::Scene* scene = m_pipeline->GetScene();
            RayTracingFeatureProcessor* rayTracingFeatureProcessor = scene->GetFeatureProcessor<RayTracingFeatureProcessor>();

            if (rayTracingFeatureProcessor)
            {
                m_rayTracingRevisionOutDated = rayTracingFeatureProcessor->GetRevision() != m_rayTracingRevision;
                if (m_rayTracingRevisionOutDated)
                {
                    m_rayTracingRevision = rayTracingFeatureProcessor->GetRevision();

                    RHI::RayTracingBufferPools& rayTracingBufferPools = rayTracingFeatureProcessor->GetBufferPools();
                    RayTracingFeatureProcessor::SubMeshVector& subMeshes = rayTracingFeatureProcessor->GetSubMeshes();

                    // create the TLAS descriptor
                    RHI::RayTracingTlasDescriptor tlasDescriptor;
                    RHI::RayTracingTlasDescriptor* tlasDescriptorBuild = tlasDescriptor.Build();

                    uint32_t instanceIndex = 0;
                    for (auto& subMesh : subMeshes)
                    {
                        tlasDescriptorBuild->Instance()
                            ->InstanceID(instanceIndex)
                            ->InstanceMask(subMesh.m_mesh->m_instanceMask)
                            ->HitGroupIndex(0)
                            ->Blas(subMesh.m_blas)
                            ->Transform(subMesh.m_mesh->m_transform)
                            ->NonUniformScale(subMesh.m_mesh->m_nonUniformScale)
                            ->Transparent(subMesh.m_material.m_irradianceColor.GetA() < 1.0f);

                        instanceIndex++;
                    }

                    unsigned proceduralHitGroupIndex = 1; // Hit group 0 is used for normal meshes
                    const auto& proceduralGeometryTypes = rayTracingFeatureProcessor->GetProceduralGeometryTypes();
                    AZStd::unordered_map<Name, unsigned> geometryTypeMap;
                    geometryTypeMap.reserve(proceduralGeometryTypes.size());
                    for (auto it = proceduralGeometryTypes.cbegin(); it != proceduralGeometryTypes.cend(); ++it)
                    {
                        geometryTypeMap[it->m_name] = proceduralHitGroupIndex++;
                    }

                    for (const auto& proceduralGeometry : rayTracingFeatureProcessor->GetProceduralGeometries())
                    {
                        tlasDescriptorBuild->Instance()
                            ->InstanceID(instanceIndex)
                            ->InstanceMask(proceduralGeometry.m_instanceMask)
                            ->HitGroupIndex(geometryTypeMap[proceduralGeometry.m_typeHandle->m_name])
                            ->Blas(proceduralGeometry.m_blas)
                            ->Transform(proceduralGeometry.m_transform)
                            ->NonUniformScale(proceduralGeometry.m_nonUniformScale);
                        instanceIndex++;
                    }

                    // create the TLAS buffers based on the descriptor
                    RHI::Ptr<RHI::RayTracingTlas>& rayTracingTlas = rayTracingFeatureProcessor->GetTlas();
                    rayTracingTlas->CreateBuffers(
                        RHI::RHISystemInterface::Get()->GetRayTracingSupport(), &tlasDescriptor, rayTracingBufferPools);
                }

                // update and compile the RayTracingSceneSrg and RayTracingMaterialSrg
                // Note: the timing of this update is very important, it needs to be updated after the TLAS is allocated so it can
                // be set on the RayTracingSceneSrg for this frame, and the ray tracing mesh data in the RayTracingSceneSrg must
                // exactly match the TLAS.  Any mismatch in this data may result in a TDR.
                rayTracingFeatureProcessor->UpdateRayTracingSrgs();
            }
        }

        RHI::Ptr<RPI::Query> RayTracingAccelerationStructurePass::GetQuery(RPI::ScopeQueryType queryType)
        {
            auto typeIndex{ static_cast<uint32_t>(queryType) };
            if (!m_scopeQueries[typeIndex])
            {
                RHI::Ptr<RPI::Query> query;
                switch (queryType)
                {
                case RPI::ScopeQueryType::Timestamp:
                    query = RPI::GpuQuerySystemInterface::Get()->CreateQuery(
                        RHI::QueryType::Timestamp, RHI::QueryPoolScopeAttachmentType::Global, RHI::ScopeAttachmentAccess::Write);
                    break;
                case RPI::ScopeQueryType::PipelineStatistics:
                    query = RPI::GpuQuerySystemInterface::Get()->CreateQuery(
                        RHI::QueryType::PipelineStatistics, RHI::QueryPoolScopeAttachmentType::Global,
                        RHI::ScopeAttachmentAccess::Write);
                    break;
                }

                m_scopeQueries[typeIndex] = query;
            }

            return m_scopeQueries[typeIndex];
        }

        template<typename Func>
        inline void RayTracingAccelerationStructurePass::ExecuteOnTimestampQuery(Func&& func)
        {
            if (IsTimestampQueryEnabled())
            {
                auto query{ GetQuery(RPI::ScopeQueryType::Timestamp) };
                if (query)
                {
                    func(query);
                }
            }
        }

        template<typename Func>
        inline void RayTracingAccelerationStructurePass::ExecuteOnPipelineStatisticsQuery(Func&& func)
        {
            if (IsPipelineStatisticsQueryEnabled())
            {
                auto query{ GetQuery(RPI::ScopeQueryType::PipelineStatistics) };
                if (query)
                {
                    func(query);
                }
            }
        }

        RPI::TimestampResult RayTracingAccelerationStructurePass::GetTimestampResultInternal() const
        {
            RPI::TimestampResult result{};

            for (auto& [deviceIndex, timestampResult] : m_timestampResults)
            {
                result.Add(timestampResult);
            }

            return result;
        }

        RPI::PipelineStatisticsResult RayTracingAccelerationStructurePass::GetPipelineStatisticsResultInternal() const
        {
            RPI::PipelineStatisticsResult result{};

            for (auto& [deviceIndex, statisticsResult] : m_statisticsResults)
            {
                result += statisticsResult;
            }

            return result;
        }

        void RayTracingAccelerationStructurePass::SetupFrameGraphDependencies(RHI::FrameGraphInterface frameGraph)
        {
            RPI::Scene* scene = m_pipeline->GetScene();
            RayTracingFeatureProcessor* rayTracingFeatureProcessor = scene->GetFeatureProcessor<RayTracingFeatureProcessor>();

            if (rayTracingFeatureProcessor)
            {
                if (m_rayTracingRevisionOutDated)
                {
                    // create the TLAS buffers based on the descriptor
                    RHI::Ptr<RHI::RayTracingTlas>& rayTracingTlas = rayTracingFeatureProcessor->GetTlas();

                    // import and attach the TLAS buffer
                    const RHI::Ptr<RHI::Buffer>& rayTracingTlasBuffer = rayTracingTlas->GetTlasBuffer();
                    if (rayTracingTlasBuffer && rayTracingFeatureProcessor->HasGeometry())
                    {
                        AZ::RHI::AttachmentId tlasAttachmentId = rayTracingFeatureProcessor->GetTlasAttachmentId();
                        if (frameGraph.GetAttachmentDatabase().IsAttachmentValid(tlasAttachmentId) == false)
                        {
                            [[maybe_unused]] RHI::ResultCode result = frameGraph.GetAttachmentDatabase().ImportBuffer(tlasAttachmentId, rayTracingTlasBuffer);
                            AZ_Assert(result == RHI::ResultCode::Success, "Failed to import ray tracing TLAS buffer with error %d", result);
                        }

                        uint32_t tlasBufferByteCount = aznumeric_cast<uint32_t>(rayTracingTlasBuffer->GetDescriptor().m_byteCount);
                        RHI::BufferViewDescriptor tlasBufferViewDescriptor = RHI::BufferViewDescriptor::CreateRayTracingTLAS(tlasBufferByteCount);

                        RHI::BufferScopeAttachmentDescriptor desc;
                        desc.m_attachmentId = tlasAttachmentId;
                        desc.m_bufferViewDescriptor = tlasBufferViewDescriptor;
                        desc.m_loadStoreAction.m_loadAction = AZ::RHI::AttachmentLoadAction::DontCare;

                        frameGraph.UseShaderAttachment(desc, RHI::ScopeAttachmentAccess::Write, RHI::ScopeAttachmentStage::RayTracingShader);
                    }
                }

                // Attach output data from the skinning pass. This is needed to ensure that this pass is executed after
                // the skinning pass has finished. We assume that the pipeline has a skinning pass with this output available.
                if (rayTracingFeatureProcessor->GetSkinnedMeshCount() > 0)
                {
                    auto skinningPassPtr = FindAdjacentPass(AZ::Name("SkinningPass"));
                    auto skinnedMeshOutputStreamBindingPtr = skinningPassPtr->FindAttachmentBinding(AZ::Name("SkinnedMeshOutputStream"));
                    [[maybe_unused]] auto result = frameGraph.UseShaderAttachment(skinnedMeshOutputStreamBindingPtr->m_unifiedScopeDesc.GetAsBuffer(), RHI::ScopeAttachmentAccess::Read, RHI::ScopeAttachmentStage::RayTracingShader);
                    AZ_Assert(result == AZ::RHI::ResultCode::Success, "Failed to attach SkinnedMeshOutputStream buffer with error %d", result);
                }

                AddScopeQueryToFrameGraph(frameGraph);
            }
        }

        void RayTracingAccelerationStructurePass::BuildCommandList(const RHI::FrameGraphExecuteContext& context)
        {
            RPI::Scene* scene = m_pipeline->GetScene();
            RayTracingFeatureProcessor* rayTracingFeatureProcessor = scene->GetFeatureProcessor<RayTracingFeatureProcessor>();

            if (!rayTracingFeatureProcessor)
            {
                return;
            }

            if (!rayTracingFeatureProcessor->GetTlas()->GetTlasBuffer())
            {
                return;
            }

            if (!m_rayTracingRevisionOutDated && rayTracingFeatureProcessor->GetSkinnedMeshCount() == 0)
            {
                // TLAS is up to date
                return;
            }

            if (!rayTracingFeatureProcessor->HasGeometry())
            {
                // no ray tracing meshes in the scene
                return;
            }

            BeginScopeQuery(context);

            // build newly added or skinned BLAS objects
            AZStd::vector<const AZ::RHI::DeviceRayTracingBlas*> changedBlasList;
            RayTracingFeatureProcessor::BlasInstanceMap& blasInstances = rayTracingFeatureProcessor->GetBlasInstances();
            for (auto& blasInstance : blasInstances)
            {
                const bool isSkinnedMesh = blasInstance.second.m_isSkinnedMesh;
                const bool buildBlas = (blasInstance.second.m_blasBuilt & RHI::MultiDevice::DeviceMask(1 << context.GetDeviceIndex())) ==
                    RHI::MultiDevice::NoDevices;
                if (buildBlas || isSkinnedMesh)
                {
                    for (auto submeshIndex = 0; submeshIndex < blasInstance.second.m_subMeshes.size(); ++submeshIndex)
                    {
                        auto& submeshBlasInstance = blasInstance.second.m_subMeshes[submeshIndex];
                        changedBlasList.push_back(submeshBlasInstance.m_blas->GetDeviceRayTracingBlas(context.GetDeviceIndex()).get());
                        if (buildBlas)
                        {
                            // Always build the BLAS, if it has not previously been built
                            context.GetCommandList()->BuildBottomLevelAccelerationStructure(*submeshBlasInstance.m_blas->GetDeviceRayTracingBlas(context.GetDeviceIndex()));
                            continue;
                        }

                        // Determine if a skinned mesh BLAS needs to be updated or completely rebuilt. For now, we want to rebuild a BLAS every
                        // SKINNED_BLAS_REBUILD_FRAME_INTERVAL frames, while updating it all other frames. This is based on the assumption that
                        // by adding together the asset ID hash, submesh index, and frame count, we get a value that allows us to uniformly
                        // distribute rebuilding all skinned mesh BLASs over all frames.
                        auto assetGuid = blasInstance.first.m_guid.GetHash();
                        if (isSkinnedMesh && (assetGuid + submeshIndex + m_frameCount) % SKINNED_BLAS_REBUILD_FRAME_INTERVAL != 0)
                        {
                            // Skinned mesh that simply needs an update
                            context.GetCommandList()->UpdateBottomLevelAccelerationStructure(
                                *submeshBlasInstance.m_blas->GetDeviceRayTracingBlas(context.GetDeviceIndex()));
                        }
                        else
                        {
                            // Fall back to building the BLAS in any case
                            context.GetCommandList()->BuildBottomLevelAccelerationStructure(
                                *submeshBlasInstance.m_blas->GetDeviceRayTracingBlas(context.GetDeviceIndex()));
                        }
                    }

                    AZStd::lock_guard lock(rayTracingFeatureProcessor->GetBlasBuiltMutex());
                    blasInstance.second.m_blasBuilt |= RHI::MultiDevice::DeviceMask(1 << context.GetDeviceIndex());
                }
            }

            // build the TLAS object
            context.GetCommandList()->BuildTopLevelAccelerationStructure(*rayTracingFeatureProcessor->GetTlas()->GetDeviceRayTracingTlas(context.GetDeviceIndex()), changedBlasList);

            ++m_frameCount;

            EndScopeQuery(context);
        }

        void RayTracingAccelerationStructurePass::AddScopeQueryToFrameGraph(RHI::FrameGraphInterface frameGraph)
        {
            const auto addToFrameGraph = [&frameGraph](RHI::Ptr<RPI::Query> query)
            {
              query->AddToFrameGraph(frameGraph);
            };

            ExecuteOnTimestampQuery(addToFrameGraph);
            ExecuteOnPipelineStatisticsQuery(addToFrameGraph);
        }

        void RayTracingAccelerationStructurePass::BeginScopeQuery(const RHI::FrameGraphExecuteContext& context)
        {
            const auto beginQuery = [&context, this](RHI::Ptr<RPI::Query> query)
            {
              if (query->BeginQuery(context) == RPI::QueryResultCode::Fail)
              {
                  AZ_UNUSED(this); // Prevent unused warning in release builds
                  AZ_WarningOnce(
                      "RayTracingAccelerationStructurePass", false,
                      "BeginScopeQuery failed. Make sure AddScopeQueryToFrameGraph was called in SetupFrameGraphDependencies"
                      " for this pass: %s",
                      this->RTTI_GetTypeName());
              }
            };

            ExecuteOnTimestampQuery(beginQuery);
            ExecuteOnPipelineStatisticsQuery(beginQuery);
        }

        void RayTracingAccelerationStructurePass::EndScopeQuery(const RHI::FrameGraphExecuteContext& context)
        {
            const auto endQuery = [&context](const RHI::Ptr<RPI::Query>& query)
            {
              query->EndQuery(context);
            };

            // This scope query implementation should be replaced by the feature linked below on GitHub:
            // [GHI-16945] Feature Request - Add GPU timestamp and pipeline statistic support for scopes
            ExecuteOnTimestampQuery(endQuery);
            ExecuteOnPipelineStatisticsQuery(endQuery);
        }

        void RayTracingAccelerationStructurePass::ReadbackScopeQueryResults()
        {
            const auto deviceMask{ RHI::RHISystemInterface::Get()->GetRayTracingSupport() };
            const auto deviceCount{ RHI::RHISystemInterface::Get()->GetDeviceCount() };
            for (auto deviceIndex{ 0 }; deviceIndex < deviceCount; ++deviceIndex)
            {
                if ((AZStd::to_underlying(deviceMask) >> deviceIndex) & 1)
                {
                    ExecuteOnTimestampQuery(
                        [this, deviceIndex](const RHI::Ptr<RPI::Query>& query)
                        {
                            const uint32_t TimestampResultQueryCount{ 2u };
                            uint64_t timestampResult[TimestampResultQueryCount] = { 0 };
                            query->GetLatestResult(&timestampResult, sizeof(uint64_t) * TimestampResultQueryCount, deviceIndex);
                            m_timestampResults[deviceIndex] =
                                RPI::TimestampResult(timestampResult[0], timestampResult[1], RHI::HardwareQueueClass::Compute);
                        });

                    ExecuteOnPipelineStatisticsQuery(
                        [this, deviceIndex](const RHI::Ptr<RPI::Query>& query)
                        {
                            query->GetLatestResult(&m_statisticsResults[deviceIndex], sizeof(RPI::PipelineStatisticsResult), deviceIndex);
                        });
                }
            }
        }
    }   // namespace RPI
}   // namespace AZ
