//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/hdSt/renderPass.h"
#include "pxr/imaging/hd/drawItem.h"
#include "pxr/imaging/hd/indirectDrawBatch.h"
#include "pxr/imaging/hd/renderContextCaps.h"
#include "pxr/imaging/hd/renderPassShader.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/shaderCode.h"
#include "pxr/imaging/hd/vtBufferSource.h"

#include "pxr/base/gf/frustum.h"

PXR_NAMESPACE_OPEN_SCOPE

HdSt_RenderPass::HdSt_RenderPass(HdRenderIndex *index,
                                 HdRprimCollection const &collection)
    : HdRenderPass(index, collection)
    , _lastCullingDisabledState(false)
    , _collectionVersion(0)
    , _collectionChanged(false)
{
}

HdSt_RenderPass::~HdSt_RenderPass()
{
    /* NOTHING */
}

void
HdSt_RenderPass::_Execute(HdRenderPassStateSharedPtr const &renderPassState,
                          TfTokenVector const& renderTags)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // CPU frustum culling (if chosen)
    _PrepareCommandBuffer(renderPassState);

    // Get the resource registry
    HdResourceRegistrySharedPtr const& resourceRegistry =
        GetRenderIndex()->GetResourceRegistry();

    // renderTags.empty() means draw everything in the collection.
    if (renderTags.empty()) {
        for (_HdCommandBufferMap::iterator it  = _cmdBuffers.begin();
                                           it != _cmdBuffers.end(); it++) {
            it->second.PrepareDraw(renderPassState, resourceRegistry);
            it->second.ExecuteDraw(renderPassState, resourceRegistry);
        }
    } else {
        TF_FOR_ALL(tag, renderTags) {
            // Check if the render tag has an associated command buffer
            if (_cmdBuffers.count(*tag) == 0) {
                continue;
            }

            // GPU frustum culling (if chosen)
            _cmdBuffers[*tag].PrepareDraw(renderPassState, resourceRegistry);
            _cmdBuffers[*tag].ExecuteDraw(renderPassState, resourceRegistry);
        }
    }
}

void
HdSt_RenderPass::_MarkCollectionDirty()
{
    // Force any cached data based on collection to be refreshed.
    _collectionChanged = true;
    _collectionVersion = 0;
}

void
HdSt_RenderPass::_PrepareCommandBuffer(
    HdRenderPassStateSharedPtr const &renderPassState)
{
    HD_TRACE_FUNCTION();
    // ------------------------------------------------------------------- #
    // SCHEDULE PREPARATION
    // ------------------------------------------------------------------- #
    // We know what must be drawn and that the stream needs to be updated, 
    // so iterate over each prim, cull it and schedule it to be drawn.

    HdChangeTracker const &tracker = GetRenderIndex()->GetChangeTracker();
    HdRenderContextCaps const &caps = HdRenderContextCaps::GetInstance();
    HdRprimCollection const &collection = GetRprimCollection();

    const int
       collectionVersion = tracker.GetCollectionVersion(collection.GetName());

    const int shaderBindingsVersion = tracker.GetShaderBindingsVersion();

    const bool 
       skipCulling = TfDebug::IsEnabled(HD_DISABLE_FRUSTUM_CULLING) ||
           (caps.multiDrawIndirectEnabled
               && Hd_IndirectDrawBatch::IsEnabledGPUFrustumCulling());

    const bool 
       cameraChanged = true,
       extentsChanged = true,
       collectionChanged = _collectionChanged 
                           || (_collectionVersion != collectionVersion);

    const bool cullingStateJustChanged = 
                                    skipCulling != _lastCullingDisabledState;
    _lastCullingDisabledState = skipCulling;

    bool freezeCulling = TfDebug::IsEnabled(HD_FREEZE_CULL_FRUSTUM);

    // Bypass freezeCulling if  collection has changed
    // Need to add extents in here as well, once they are
    // hooked up to detect proper change.
    if(collectionChanged || cullingStateJustChanged) {
        freezeCulling = false;
    }

    // Now either the collection is dirty or culling needs to be applied.
    if (collectionChanged) {
        HD_PERF_COUNTER_INCR(HdPerfTokens->collectionsRefreshed);
        TF_DEBUG(HD_COLLECTION_CHANGED).Msg("CollectionChanged: %s "
                                            "version: %d -> %d\n", 
                                             collection.GetName().GetText(),
                                             _collectionVersion,
                                             collectionVersion);

        HdRenderIndex::HdDrawItemView items = 
            GetRenderIndex()->GetDrawItems(collection);

        // This loop will extract the tags and bucket the geometry in 
        // the different command buffers.
        size_t itemCount = 0;
        _cmdBuffers.clear();
        for (HdRenderIndex::HdDrawItemView::iterator it = items.begin();
                                                    it != items.end(); it++ ) {
            _cmdBuffers[it->first].SwapDrawItems(&it->second, 
                                                 shaderBindingsVersion);
            itemCount += _cmdBuffers[it->first].GetTotalSize();
        }

        _collectionVersion = collectionVersion;
        _collectionChanged = false;
        HD_PERF_COUNTER_SET(HdTokens->totalItemCount, itemCount);
    } else {
        // validate command buffer to not include expired drawItems,
        // which could be produced by migrating BARs at the new repr creation.
        for (_HdCommandBufferMap::iterator it  = _cmdBuffers.begin(); 
                                           it != _cmdBuffers.end(); it++) {
            it->second.RebuildDrawBatchesIfNeeded(shaderBindingsVersion);
        }
    }

    if(skipCulling) {
        // Since culling state is stored across renders,
        // we need to update all items visible state
        for (_HdCommandBufferMap::iterator it = _cmdBuffers.begin(); 
                                           it != _cmdBuffers.end(); it++) {
            it->second.SyncDrawItemVisibility(tracker.GetVisibilityChangeCount());
        }

        TF_DEBUG(HD_DRAWITEMS_CULLED).Msg("CULLED: skipped\n");
    }
    else {
        // XXX: this process should be moved to Hd_DrawBatch::PrepareDraw
        //      to be consistent with GPU culling.
        if((!freezeCulling)
            && (collectionChanged || cameraChanged || extentsChanged)) {
            // Re-cull the command buffer. 
            for (_HdCommandBufferMap::iterator it  = _cmdBuffers.begin(); 
                                               it != _cmdBuffers.end(); it++) {
                it->second.FrustumCull(renderPassState->GetCullMatrix());
            }
        }

        for (_HdCommandBufferMap::iterator it  = _cmdBuffers.begin(); 
                                           it != _cmdBuffers.end(); it++) {
            TF_DEBUG(HD_DRAWITEMS_CULLED).Msg("CULLED: %zu drawItems\n", 
                                                 it->second.GetCulledSize());
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
