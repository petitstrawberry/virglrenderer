/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Top-level function dispatch overrides.  Mandatory because top-
 * level functions have no `_self` for the default dispatcher to bind
 * against.  Each override resolves the function pointer from the
 * loaded host D3D library and forwards the decoded args.
 */

#include "npt_context.h"
#include "npt_library.h"
#include "npt_overrides.h"
#include "npt_renderer.h"

#include "neptune-protocol/npt_protocol_host_toplevel.h"

/* ================================================================== */
/* DXGI factory creation                                                */
/* ================================================================== */

/* CreateDXGIFactory, CreateDXGIFactory1 and CreateDXGIFactory2 all
 * return the same factory; collapse onto CreateDXGIFactory1, the one
 * entry point the library loader resolves.  CreateDXGIFactory2's flags
 * only ask for the debug layer. */
static HRESULT
npt_create_dxgi_factory(const IID *riid, void **ppFactory)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_CreateDXGIFactory1)
      return NPT_E_FAIL;
   return lib->pfn_CreateDXGIFactory1(riid, ppFactory);
}

static HRESULT
npt_override_CreateDXGIFactory(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_CreateDXGIFactory *args)
{
   args->ret = npt_create_dxgi_factory(args->riid, args->ppFactory);
   return args->ret;
}

static HRESULT
npt_override_CreateDXGIFactory1(UNUSED struct npt_dispatch_context *dctx,
                                struct npt_command_CreateDXGIFactory1 *args)
{
   args->ret = npt_create_dxgi_factory(args->riid, args->ppFactory);
   return args->ret;
}

static HRESULT
npt_override_CreateDXGIFactory2(UNUSED struct npt_dispatch_context *dctx,
                                struct npt_command_CreateDXGIFactory2 *args)
{
   args->ret = npt_create_dxgi_factory(args->riid, args->ppFactory);
   return args->ret;
}

/* ================================================================== */
/* D3D11 device creation                                                */
/* ================================================================== */

static HRESULT
npt_override_D3D11CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_D3D11CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D11CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }

   args->ret = lib->pfn_D3D11CreateDevice(
      args->pAdapter,
      args->DriverType,
      /* Software (HMODULE): a host-side module handle would be
       * meaningless to the guest. */
      0,
      args->Flags,
      args->pFeatureLevels,
      args->FeatureLevels,
      args->SDKVersion,
      args->ppDevice,
      args->pFeatureLevel,
      args->ppImmediateContext);
   return args->ret;
}

static HRESULT
npt_override_D3D11On12CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                                   struct npt_command_D3D11On12CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D11On12CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = lib->pfn_D3D11On12CreateDevice(
      args->pDevice, args->Flags, args->pFeatureLevels, args->FeatureLevels,
      args->ppCommandQueues, args->NumQueues, args->NodeMask,
      args->ppDevice, args->ppImmediateContext, args->pChosenFeatureLevel);
   return args->ret;
}

/* ================================================================== */
/* D3D12 device creation                                                */
/* ================================================================== */

static HRESULT
npt_override_D3D12CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_D3D12CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D12CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = lib->pfn_D3D12CreateDevice(args->pAdapter,
                                           args->MinimumFeatureLevel,
                                           args->riid,
                                           args->ppDevice);
   return args->ret;
}

static HRESULT
npt_override_D3D12CreateRootSignatureDeserializer(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12CreateRootSignatureDeserializer *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppRootSignatureDeserializer)
      *args->ppRootSignatureDeserializer = NULL;
   return args->ret;
}

static HRESULT
npt_override_D3D12CreateVersionedRootSignatureDeserializer(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12CreateVersionedRootSignatureDeserializer *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppRootSignatureDeserializer)
      *args->ppRootSignatureDeserializer = NULL;
   return args->ret;
}

/* ID3DBlob accessors via raw vtable slots (3 = GetBufferPointer,
 * 4 = GetBufferSize; see NPT_VTBL_ID3D10Blob_*). */
typedef void *(NPT_STDMETHODCALLTYPE *pfn_npt_blob_get_pointer)(void *self);
typedef SIZE_T (NPT_STDMETHODCALLTYPE *pfn_npt_blob_get_size)(void *self);

#define NPT_E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)

/* Copy a host ID3DBlob into a guest-capacity byte window.  *size_inout
 * carries the guest capacity in and the actual/required size out;
 * *data_inout is the window, cleared when nothing was written.
 *
 * Both the reply encoder and the guest's reply decoder size the payload
 * from the returned *size_inout, and the guest reserved its reply window
 * from the capacity it sent.  So the returned size may exceed capacity
 * (the guest needs it to retry with an exact allocation) only if the
 * data pointer is cleared with it -- otherwise the encoder reads past
 * the decoder's capacity-sized temp buffer and the guest writes past its
 * own. */
static HRESULT
npt_blob_copy_out(ID3DBlob *blob, UINT *size_inout, void **data_inout)
{
   void *data_out = *data_inout;
   *data_inout = NULL;

   if (!size_inout)
      return NPT_S_OK;
   if (!blob) {
      *size_inout = 0;
      return NPT_S_OK;
   }

   void **vtbl = npt_com_vtable(blob);
   void *src = NPT_COM_VTBL_FUNC(pfn_npt_blob_get_pointer, vtbl,
                                 NPT_VTBL_ID3D10Blob_GetBufferPointer)(blob);
   SIZE_T size = NPT_COM_VTBL_FUNC(pfn_npt_blob_get_size, vtbl,
                                   NPT_VTBL_ID3D10Blob_GetBufferSize)(blob);

   UINT capacity = *size_inout;
   *size_inout = (UINT)size;
   if (!data_out || size > capacity)
      return NPT_E_NOT_SUFFICIENT_BUFFER;
   if (src && size)
      memcpy(data_out, src, size);
   *data_inout = data_out;
   return NPT_S_OK;
}

static void
npt_serialize_clear_sizes(UINT *blob_size, UINT *error_size)
{
   if (blob_size)
      *blob_size = 0;
   if (error_size)
      *error_size = 0;
}

/* Move the serializer's blobs into the reply buffers and drop the host
 * copies.  Error text is advisory: an over-long diagnostic degrades to
 * size-only rather than failing the whole call. */
static HRESULT
npt_serialize_finish(HRESULT hr, ID3DBlob *blob, ID3DBlob *error_blob,
                     UINT *blob_size, void **blob_data,
                     UINT *error_size, void **error_data)
{
   (void)npt_blob_copy_out(error_blob, error_size, error_data);
   if (hr >= 0) {
      const HRESULT copy_hr = npt_blob_copy_out(blob, blob_size, blob_data);
      if (copy_hr < 0)
         hr = copy_hr;
   } else {
      if (blob_size)
         *blob_size = 0;
      *blob_data = NULL;
   }

   if (blob)
      npt_com_release(blob);
   if (error_blob)
      npt_com_release(error_blob);
   return hr;
}

/* The serializers dereference the parameter/sampler arrays without
 * validating, and this runs in the shared render server: a malformed
 * guest desc must fail cleanly, not SIGSEGV the worker. */
static bool
npt_root_params_valid(UINT num_params, const void *params,
                      UINT num_samplers, const void *samplers)
{
   return (!num_params || params) && (!num_samplers || samplers);
}

static bool
npt_root_signature_desc_valid(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
   return desc && npt_root_params_valid(desc->NumParameters,
                                        desc->pParameters,
                                        desc->NumStaticSamplers,
                                        desc->pStaticSamplers);
}

static HRESULT
npt_override_D3D12SerializeRootSignature(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12SerializeRootSignature *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D12SerializeRootSignature) {
      npt_serialize_clear_sizes(args->pBlobSize, args->pErrorBlobSize);
      args->ret = NPT_E_NOTIMPL;
      return args->ret;
   }

   if (!npt_root_signature_desc_valid(args->pRootSignature)) {
      npt_serialize_clear_sizes(args->pBlobSize, args->pErrorBlobSize);
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   ID3DBlob *blob = NULL;
   ID3DBlob *error_blob = NULL;
   const HRESULT hr = lib->pfn_D3D12SerializeRootSignature(
      args->pRootSignature, args->Version, &blob, &error_blob);

   args->ret = npt_serialize_finish(hr, blob, error_blob,
                                    args->pBlobSize, &args->pBlobData,
                                    args->pErrorBlobSize,
                                    &args->pErrorBlobData);
   return args->ret;
}

static bool
npt_versioned_root_signature_desc_valid(
   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc)
{
   if (!desc)
      return false;
   switch (desc->Version) {
   case D3D_ROOT_SIGNATURE_VERSION_1_0:
      return npt_root_signature_desc_valid(&desc->Desc_1_0);
   case D3D_ROOT_SIGNATURE_VERSION_1_1:
      return npt_root_params_valid(desc->Desc_1_1.NumParameters,
                                   desc->Desc_1_1.pParameters,
                                   desc->Desc_1_1.NumStaticSamplers,
                                   desc->Desc_1_1.pStaticSamplers);
   case D3D_ROOT_SIGNATURE_VERSION_1_2:
      return npt_root_params_valid(desc->Desc_1_2.NumParameters,
                                   desc->Desc_1_2.pParameters,
                                   desc->Desc_1_2.NumStaticSamplers,
                                   desc->Desc_1_2.pStaticSamplers);
   default:
      /* Unknown version: the backend switches on Version before touching
       * any array, so let it reject the desc. */
      return true;
   }
}

static HRESULT
npt_override_D3D12SerializeVersionedRootSignature(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12SerializeVersionedRootSignature *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D12SerializeVersionedRootSignature) {
      npt_serialize_clear_sizes(args->pBlobSize, args->pErrorBlobSize);
      args->ret = NPT_E_NOTIMPL;
      return args->ret;
   }

   if (!npt_versioned_root_signature_desc_valid(args->pRootSignature)) {
      npt_serialize_clear_sizes(args->pBlobSize, args->pErrorBlobSize);
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   ID3DBlob *blob = NULL;
   ID3DBlob *error_blob = NULL;
   const HRESULT hr = lib->pfn_D3D12SerializeVersionedRootSignature(
      args->pRootSignature, &blob, &error_blob);

   args->ret = npt_serialize_finish(hr, blob, error_blob,
                                    args->pBlobSize, &args->pBlobData,
                                    args->pErrorBlobSize,
                                    &args->pErrorBlobData);
   return args->ret;
}

static HRESULT
npt_override_DXGIDeclareAdapterRemovalSupport(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_DXGIDeclareAdapterRemovalSupport *args)
{
   args->ret = NPT_S_OK;
   return args->ret;
}

/* ================================================================== */
/* Override table                                                       */
/* ================================================================== */

struct npt_dispatch_toplevel_overrides npt_toplevel_overrides = {
   .CreateDXGIFactory                              = npt_override_CreateDXGIFactory,
   .CreateDXGIFactory1                             = npt_override_CreateDXGIFactory1,
   .CreateDXGIFactory2                             = npt_override_CreateDXGIFactory2,
   .DXGIDeclareAdapterRemovalSupport               = npt_override_DXGIDeclareAdapterRemovalSupport,
   .D3D11CreateDevice                              = npt_override_D3D11CreateDevice,
   .D3D11On12CreateDevice                          = npt_override_D3D11On12CreateDevice,
   .D3D12CreateDevice                              = npt_override_D3D12CreateDevice,
   .D3D12CreateRootSignatureDeserializer           = npt_override_D3D12CreateRootSignatureDeserializer,
   .D3D12CreateVersionedRootSignatureDeserializer  = npt_override_D3D12CreateVersionedRootSignatureDeserializer,
   .D3D12SerializeRootSignature                    = npt_override_D3D12SerializeRootSignature,
   .D3D12SerializeVersionedRootSignature           = npt_override_D3D12SerializeVersionedRootSignature,
};
