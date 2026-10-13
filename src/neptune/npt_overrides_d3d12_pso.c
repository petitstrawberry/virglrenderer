/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXIL input-layout placeholders.  The runtime hands the guest UMD bare DXIL
 * program parts, whose input-signature names live in bitcode metadata the
 * guest cannot read, so for a DXIL vertex shader it names each input-layout
 * element "NPTA<register>" instead.  Backends match input-layout elements to
 * the vertex shader by semantic name, so the placeholders are a protocol
 * convention this host must resolve before any backend sees the desc: the
 * backend's DXIL reflection names each register, and elements whose register
 * the shader does not read are dropped, as the guest does for DXBC.
 */

#include <stdlib.h>
#include <string.h>

#include "npt_common.h"
#include "npt_library.h"
#include "npt_overrides.h"
#include "npt_renderer.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

#define NPT_DXIL_PLACEHOLDER     "NPTA"
#define NPT_DXIL_PLACEHOLDER_LEN 4

static bool
npt_pso_is_placeholder(const CHAR *name)
{
   return name && !strncmp((const char *)name, NPT_DXIL_PLACEHOLDER,
                           NPT_DXIL_PLACEHOLDER_LEN);
}

HRESULT
npt_override_ID3D12Device_CreateGraphicsPipelineState(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateGraphicsPipelineState *args,
   PFN_ID3D12Device_CreateGraphicsPipelineState original)
{
   const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc = args->pDesc;
   const D3D12_INPUT_LAYOUT_DESC *il = desc ? &desc->InputLayout : NULL;

   bool placeholders = false;
   if (il && il->pInputElementDescs && desc->VS.pShaderBytecode &&
       il->NumElements <= D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) {
      for (UINT i = 0; i < il->NumElements && !placeholders; i++)
         placeholders =
            npt_pso_is_placeholder(il->pInputElementDescs[i].SemanticName);
   }
   if (!placeholders) {
      args->ret = original(args->_self, desc, args->riid,
                           args->ppPipelineState);
      return args->ret;
   }

   const struct npt_d3d_library *lib = npt_renderer_get_library();
   struct npt_dxil_semantic sem[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT];
   int nsem = 0;
   if (lib && lib->pfn_dxil_input_semantics)
      nsem = lib->pfn_dxil_input_semantics(desc->VS.pShaderBytecode,
                                           desc->VS.BytecodeLength, sem,
                                           (int)ARRAY_SIZE(sem));
   if (nsem <= 0) {
      npt_log("CreateGraphicsPipelineState: DXIL input-layout placeholders "
              "but %s; the pipeline will not bind vertex inputs",
              lib && lib->pfn_dxil_input_semantics
                 ? "VS reflection failed"
                 : "the backend has no DXIL reflection");
      args->ret = original(args->_self, desc, args->riid,
                           args->ppPipelineState);
      return args->ret;
   }

   for (int s = 0; s < nsem; s++)
      sem[s].name[sizeof(sem[s].name) - 1] = '\0';

   D3D12_INPUT_ELEMENT_DESC elems[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT];
   UINT n = 0;
   for (UINT i = 0; i < il->NumElements; i++) {
      D3D12_INPUT_ELEMENT_DESC e = il->pInputElementDescs[i];
      if (npt_pso_is_placeholder(e.SemanticName)) {
         const unsigned reg = (unsigned)strtoul(
            (const char *)e.SemanticName + NPT_DXIL_PLACEHOLDER_LEN, NULL, 10);
         const struct npt_dxil_semantic *hit = NULL;
         for (int s = 0; s < nsem && !hit; s++)
            if (sem[s].reg == reg)
               hit = &sem[s];
         if (!hit)
            continue;
         e.SemanticName = (const CHAR *)hit->name;
         e.SemanticIndex = hit->index;
      }
      elems[n++] = e;
   }

   D3D12_GRAPHICS_PIPELINE_STATE_DESC patched = *desc;
   patched.InputLayout.pInputElementDescs = elems;
   patched.InputLayout.NumElements = n;
   args->ret = original(args->_self, &patched, args->riid,
                        args->ppPipelineState);
   return args->ret;
}
