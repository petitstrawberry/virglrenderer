/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device dispatch overrides, and the table that installs them.
 * The shared-HANDLE entry points are rejected under the policy in
 * npt_overrides_shared_handle.c; they live here so the table they share
 * with CheckFeatureSupport needs no cross-file declaration.
 *
 * CheckFeatureSupport travels as an opaque in/out byte blob, which works
 * only for pointer-free feature structs.  FEATURE_LEVELS embeds a guest
 * pointer, so it gets a wire convention -- the requested-levels array is
 * appended after the struct, the pointer field travels as NULL -- and
 * this override rebuilds the pointer for the call and scrubs it from the
 * reply.  The remaining pointer-bearing features have no such convention
 * and are rejected: their pointer would arrive holding whatever the
 * guest left in it, and the backend dereferences it -- for
 * PROTECTED_RESOURCE_SESSION_TYPES, writes through it -- inside the
 * render server.
 */

#include "npt_common.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

static HRESULT
reject_ID3D12Device_CreateSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateSharedHandle *args,
   UNUSED PFN_ID3D12Device_CreateSharedHandle original)
{
   if (args->pHandle)
      *args->pHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D12Device_OpenSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_OpenSharedHandle *args,
   UNUSED PFN_ID3D12Device_OpenSharedHandle original)
{
   if (args->ppvObj)
      *args->ppvObj = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D12Device_OpenSharedHandleByName(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_OpenSharedHandleByName *args,
   UNUSED PFN_ID3D12Device_OpenSharedHandleByName original)
{
   if (args->pNTHandle)
      *args->pNTHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device_CheckFeatureSupport(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CheckFeatureSupport *args,
   PFN_ID3D12Device_CheckFeatureSupport original)
{
   if (args->Feature == D3D12_FEATURE_FEATURE_LEVELS) {
      D3D12_FEATURE_DATA_FEATURE_LEVELS *fl = args->pFeatureSupportData;
      if (!fl || args->FeatureSupportDataSize < sizeof(*fl)) {
         args->ret = NPT_E_INVALIDARG;
         return args->ret;
      }
      const size_t appended =
         args->FeatureSupportDataSize - sizeof(*fl);
      if ((size_t)fl->NumFeatureLevels * sizeof(D3D_FEATURE_LEVEL) !=
          appended) {
         args->ret = NPT_E_INVALIDARG;
         return args->ret;
      }
      fl->pFeatureLevelsRequested =
         (const D3D_FEATURE_LEVEL *)((uint8_t *)fl + sizeof(*fl));
      args->ret = original(args->_self, args->Feature, fl,
                           (UINT)sizeof(*fl));
      /* Never leak a host pointer back through the reply blob. */
      fl->pFeatureLevelsRequested = NULL;
      return args->ret;
   }

   if (args->Feature == D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES ||
       args->Feature == D3D12_FEATURE_QUERY_META_COMMAND) {
      npt_log("CheckFeatureSupport: pointer-bearing feature %u has no wire "
              "convention; rejected", (unsigned)args->Feature);
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   args->ret = original(args->_self, args->Feature,
                        args->pFeatureSupportData,
                        args->FeatureSupportDataSize);
   return args->ret;
}

/* The guest can never export a fence or a heap -- every shared-HANDLE
 * entry point above is rejected -- so their SHARED flags only make a
 * backend build export machinery for nothing (d3dmetal-native backs a
 * SHARED heap with impostor textures and a SHARED fence with a producer
 * that runs on every Signal).  The guest answers the flag back itself
 * where the API reports it. */
#define NPT_D3D12_FENCE_SHARE_FLAGS \
   (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)

/* vkd3d-proton's private heap flag, set by the guest on presentable
 * resources so a Linux host exports them as linear dma-bufs.  It is not
 * a D3D12_HEAP_FLAGS value, and the darwin backend validates against
 * the real ones. */
#define NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF ((D3D12_HEAP_FLAGS)0x40000000)

static D3D12_HEAP_FLAGS
npt_backend_resource_heap_flags(D3D12_HEAP_FLAGS flags)
{
#ifdef __APPLE__
   flags &= ~NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF;
#endif
   return flags;
}

static HRESULT
npt_override_ID3D12Device_CreateFence(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateFence *args,
   PFN_ID3D12Device_CreateFence original)
{
   args->ret = original(args->_self, args->InitialValue,
                        args->Flags & ~NPT_D3D12_FENCE_SHARE_FLAGS,
                        args->riid, args->ppFence);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device_CreateHeap(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateHeap *args,
   PFN_ID3D12Device_CreateHeap original)
{
   D3D12_HEAP_DESC desc;
   const D3D12_HEAP_DESC *pdesc = args->pDesc;
   if (pdesc) {
      desc = *pdesc;
      desc.Flags &= ~D3D12_HEAP_FLAG_SHARED;
      pdesc = &desc;
   }
   args->ret = original(args->_self, pdesc, args->riid, args->ppvHeap);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device4_CreateHeap1(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device4_CreateHeap1 *args,
   PFN_ID3D12Device4_CreateHeap1 original)
{
   D3D12_HEAP_DESC desc;
   const D3D12_HEAP_DESC *pdesc = args->pDesc;
   if (pdesc) {
      desc = *pdesc;
      desc.Flags &= ~D3D12_HEAP_FLAG_SHARED;
      pdesc = &desc;
   }
   args->ret = original(args->_self, pdesc, args->pProtectedSession,
                        args->riid, args->ppvHeap);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device_CreateCommittedResource(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateCommittedResource *args,
   PFN_ID3D12Device_CreateCommittedResource original)
{
   args->ret = original(args->_self, args->pHeapProperties,
                        npt_backend_resource_heap_flags(args->HeapFlags),
                        args->pDesc, args->InitialResourceState,
                        args->pOptimizedClearValue, args->riidResource,
                        args->ppvResource);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device4_CreateCommittedResource1(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device4_CreateCommittedResource1 *args,
   PFN_ID3D12Device4_CreateCommittedResource1 original)
{
   args->ret = original(args->_self, args->pHeapProperties,
                        npt_backend_resource_heap_flags(args->HeapFlags),
                        args->pDesc, args->InitialResourceState,
                        args->pOptimizedClearValue, args->pProtectedSession,
                        args->riidResource, args->ppvResource);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device8_CreateCommittedResource2(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device8_CreateCommittedResource2 *args,
   PFN_ID3D12Device8_CreateCommittedResource2 original)
{
   args->ret = original(args->_self, args->pHeapProperties,
                        npt_backend_resource_heap_flags(args->HeapFlags),
                        args->pDesc, args->InitialResourceState,
                        args->pOptimizedClearValue, args->pProtectedSession,
                        args->riidResource, args->ppvResource);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device10_CreateCommittedResource3(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device10_CreateCommittedResource3 *args,
   PFN_ID3D12Device10_CreateCommittedResource3 original)
{
   args->ret = original(args->_self, args->pHeapProperties,
                        npt_backend_resource_heap_flags(args->HeapFlags),
                        args->pDesc, args->InitialLayout,
                        args->pOptimizedClearValue, args->pProtectedSession,
                        args->NumCastableFormats, args->pCastableFormats,
                        args->riidResource, args->ppvResource);
   return args->ret;
}

struct npt_dispatch_id3d12device_overrides npt_id3d12device_overrides = {
   .CreateFence = npt_override_ID3D12Device_CreateFence,
   .CreateHeap = npt_override_ID3D12Device_CreateHeap,
   .CreateCommittedResource = npt_override_ID3D12Device_CreateCommittedResource,
   .CreateSharedHandle = reject_ID3D12Device_CreateSharedHandle,
   .OpenSharedHandle = reject_ID3D12Device_OpenSharedHandle,
   .OpenSharedHandleByName = reject_ID3D12Device_OpenSharedHandleByName,
   .CheckFeatureSupport = npt_override_ID3D12Device_CheckFeatureSupport,
};

struct npt_dispatch_id3d12device4_overrides npt_id3d12device4_overrides = {
   .CreateHeap1 = npt_override_ID3D12Device4_CreateHeap1,
   .CreateCommittedResource1 =
      npt_override_ID3D12Device4_CreateCommittedResource1,
};

struct npt_dispatch_id3d12device8_overrides npt_id3d12device8_overrides = {
   .CreateCommittedResource2 =
      npt_override_ID3D12Device8_CreateCommittedResource2,
};

struct npt_dispatch_id3d12device10_overrides npt_id3d12device10_overrides = {
   .CreateCommittedResource3 =
      npt_override_ID3D12Device10_CreateCommittedResource3,
};
