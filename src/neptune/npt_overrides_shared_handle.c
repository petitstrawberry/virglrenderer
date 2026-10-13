/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared-HANDLE policy: cross-process / cross-device shared HANDLEs
 * are not supported.
 *
 * A shared HANDLE returned by Create*SharedHandle / GetSharedHandle
 * is a Windows kernel-object token valid only in the minting
 * process.  The guest has no way to consume a host-process token
 * without a guest-side wrapping layer, and the inverse OpenShared*
 * path would round-trip an unchecked uintptr_t into the host D3D
 * library's dereference path — an exploitable footgun.
 *
 * Every shared-HANDLE entry point fails with E_INVALIDARG before
 * reaching the host library, zeroing output handles/objects so the
 * post-dispatch register pass skips registration.  The ID3D12Device
 * ones are in npt_overrides_d3d12_device.c, with the rest of that
 * interface's table.
 */

#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d11device.h"
#include "neptune-protocol/npt_protocol_host_id3d11fence.h"
#include "neptune-protocol/npt_protocol_host_idxgiresource.h"

/* -------------------------------------------------------------------- */
/* Outbound: Create*SharedHandle / GetSharedHandle                      */
/* -------------------------------------------------------------------- */

static HRESULT
reject_IDXGIResource_GetSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_IDXGIResource_GetSharedHandle *args,
   UNUSED PFN_IDXGIResource_GetSharedHandle original)
{
   if (args->pSharedHandle)
      *args->pSharedHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_IDXGIResource1_CreateSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_IDXGIResource1_CreateSharedHandle *args,
   UNUSED PFN_IDXGIResource1_CreateSharedHandle original)
{
   if (args->pHandle)
      *args->pHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D11Fence_CreateSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Fence_CreateSharedHandle *args,
   UNUSED PFN_ID3D11Fence_CreateSharedHandle original)
{
   if (args->pHandle)
      *args->pHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

/* -------------------------------------------------------------------- */
/* Inbound: OpenShared*                                                 */
/* -------------------------------------------------------------------- */

static HRESULT
reject_ID3D11Device_OpenSharedResource(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Device_OpenSharedResource *args,
   UNUSED PFN_ID3D11Device_OpenSharedResource original)
{
   if (args->ppResource)
      *args->ppResource = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D11Device1_OpenSharedResource1(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Device1_OpenSharedResource1 *args,
   UNUSED PFN_ID3D11Device1_OpenSharedResource1 original)
{
   if (args->ppResource)
      *args->ppResource = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D11Device1_OpenSharedResourceByName(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Device1_OpenSharedResourceByName *args,
   UNUSED PFN_ID3D11Device1_OpenSharedResourceByName original)
{
   if (args->ppResource)
      *args->ppResource = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D11Device5_OpenSharedFence(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Device5_OpenSharedFence *args,
   UNUSED PFN_ID3D11Device5_OpenSharedFence original)
{
   if (args->ppFence)
      *args->ppFence = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

/* -------------------------------------------------------------------- */
/* Override tables                                                      */
/* -------------------------------------------------------------------- */

struct npt_dispatch_idxgiresource_overrides npt_idxgiresource_overrides = {
   .GetSharedHandle = reject_IDXGIResource_GetSharedHandle,
};

struct npt_dispatch_idxgiresource1_overrides npt_idxgiresource1_overrides = {
   .CreateSharedHandle = reject_IDXGIResource1_CreateSharedHandle,
};

struct npt_dispatch_id3d11device_overrides npt_id3d11device_overrides = {
   .OpenSharedResource = reject_ID3D11Device_OpenSharedResource,
};

struct npt_dispatch_id3d11device1_overrides npt_id3d11device1_overrides = {
   .OpenSharedResource1 = reject_ID3D11Device1_OpenSharedResource1,
   .OpenSharedResourceByName = reject_ID3D11Device1_OpenSharedResourceByName,
};

/* No fence can be exported (CreateSharedHandle is rejected), so SHARED
 * would only make the backend set up export machinery for nothing. */
static HRESULT
npt_override_ID3D11Device5_CreateFence(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11Device5_CreateFence *args,
   PFN_ID3D11Device5_CreateFence original)
{
   args->ret = original(args->_self, args->InitialValue,
                        (D3D11_FENCE_FLAG)(args->Flags &
                                           ~D3D11_FENCE_FLAG_SHARED),
                        args->ReturnedInterface, args->ppFence);
   return args->ret;
}

struct npt_dispatch_id3d11device5_overrides npt_id3d11device5_overrides = {
   .OpenSharedFence = reject_ID3D11Device5_OpenSharedFence,
   .CreateFence = npt_override_ID3D11Device5_CreateFence,
};

struct npt_dispatch_id3d11fence_overrides npt_id3d11fence_overrides = {
   .CreateSharedHandle = reject_ID3D11Fence_CreateSharedHandle,
};
