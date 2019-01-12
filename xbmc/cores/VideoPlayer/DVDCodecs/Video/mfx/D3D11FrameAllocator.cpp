/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "D3D11FrameAllocator.h"
#include "utils/log.h"
#include <algorithm>
#include <iterator>

namespace MFX
{

//for generating sequence of mfx handles
template <typename T>
struct sequence {
  T x;
  sequence(T seed) : x(seed) { }
};

template <>
struct sequence<mfxHDL> {
  mfxHDL x;
  sequence(mfxHDL seed) : x(seed) { }

  mfxHDL operator ()()
  {
    const mfxHDL y = x;
    x = reinterpret_cast<mfxHDL>(1 + reinterpret_cast<size_t>(x));
    return y;
  }
};


CD3D11FrameAllocator::CD3D11FrameAllocator() = default;

CD3D11FrameAllocator::~CD3D11FrameAllocator()
{
  CD3D11FrameAllocator::Close();
}

CD3D11FrameAllocator::TextureSubResource CD3D11FrameAllocator::GetResourceFromMid(mfxMemId mid)
{
  const size_t index = reinterpret_cast<size_t>(mid) - 1;

  if (m_memIdMap.size() <= index)
    return {};

  //reverse iterator dereferencing
  TextureResource * p = &*m_memIdMap[index];
  if (!p->bAlloc)
    return {};

  return {p, mid};
}

mfxStatus CD3D11FrameAllocator::Init(mfxAllocatorParams *pParams)
{
  auto* pd3d11Params = dynamic_cast<CD3D11AllocatorParams*>(pParams);
  if (nullptr == pd3d11Params ||
      nullptr == pd3d11Params->pDevice)
  {
    return MFX_ERR_NOT_INITIALIZED;
  }

  m_initParams = *pd3d11Params;

  return MFX_ERR_NONE;
}

mfxStatus CD3D11FrameAllocator::Close()
{
  const mfxStatus sts = CBaseFrameAllocator::Close();

  for (auto& i : m_resourcesByRequest)
    i.Release();

  m_resourcesByRequest.clear();
  m_memIdMap.clear();

  return sts;
}

mfxStatus CD3D11FrameAllocator::LockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  return MFX_ERR_UNSUPPORTED;
}

mfxStatus CD3D11FrameAllocator::UnlockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  return MFX_ERR_UNSUPPORTED;
}

mfxStatus CD3D11FrameAllocator::GetFrameHDL(mfxMemId mid, mfxHDL *handle)
{
  if (nullptr == handle)
    return MFX_ERR_INVALID_HANDLE;

  TextureSubResource sr = GetResourceFromMid(mid);

  if (!sr.GetTexture())
    return MFX_ERR_INVALID_HANDLE;

  const auto pPair = reinterpret_cast<mfxHDLPair*>(handle);

  pPair->first = sr.GetTexture();
  pPair->second = reinterpret_cast<mfxHDL>(static_cast<UINT_PTR>(sr.GetSubResource()));

  return MFX_ERR_NONE;
}

mfxStatus CD3D11FrameAllocator::CheckRequestType(mfxFrameAllocRequest *request)
{
  const mfxStatus sts = CBaseFrameAllocator::CheckRequestType(request);
  if (MFX_ERR_NONE != sts)
    return sts;

  if (request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET)
    return MFX_ERR_NONE;
  else
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus CD3D11FrameAllocator::ReleaseResponse(mfxFrameAllocResponse *response)
{
  if (nullptr == response)
    return MFX_ERR_NULL_PTR;

  if (response->mids && 0 != response->NumFrameActual)
  {
    //check whether texture exist
    TextureSubResource sr = GetResourceFromMid(response->mids[0]);

    if (!sr.GetTexture())
      return MFX_ERR_NULL_PTR;

    sr.Release();

    //if texture is last it is possible to remove also all handles from map to reduce fragmentation
    //search for allocated chunk
    if (m_resourcesByRequest.end() == std::find_if(m_resourcesByRequest.begin(), m_resourcesByRequest.end(), TextureResource::isAllocated))
    {
      m_resourcesByRequest.clear();
      m_memIdMap.clear();
    }
  }

  return MFX_ERR_NONE;
}
mfxStatus CD3D11FrameAllocator::AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  UINT supported;

  const DXGI_FORMAT format = GetDXGIFormat(request->Info.FourCC);
  if (DXGI_FORMAT_UNKNOWN == format)
    return MFX_ERR_UNSUPPORTED;

  TextureResource newTexture;

  D3D11_TEXTURE2D_DESC desc = {};

  desc.Width = request->Info.Width;
  desc.Height = request->Info.Height;

  desc.MipLevels = 1;
  //number of sub-resources is 1 in case of not single texture
  desc.ArraySize = m_initParams.bUseSingleTexture ? request->NumFrameSuggested : 1;
  desc.Format = GetDXGIFormat(request->Info.FourCC);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  desc.BindFlags = D3D11_BIND_DECODER;
  if (SUCCEEDED(m_initParams.pDevice->CheckFormatSupport(desc.Format, &supported)) &&
      (supported & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
  {
    desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
  }

  ID3D11Texture2D* pTexture2D = nullptr;
  for (size_t i = 0; i < request->NumFrameSuggested / desc.ArraySize; i++)
  {
    HRESULT hRes = m_initParams.pDevice->CreateTexture2D(&desc, nullptr, &pTexture2D);

    if (FAILED(hRes))
    {
      CLog::Log(LOGERROR, "%s: CreateTexture2D(%lld) failed, hr = 0x%08lx\n", __FUNCTION__, static_cast<long long>(i), hRes);
      return MFX_ERR_MEMORY_ALLOC;
    }
    newTexture.textures.push_back(pTexture2D);
  }

  // mapping to self created handles array, starting from zero or from last assigned handle + 1
  sequence<mfxHDL> seq_initializer(m_resourcesByRequest.empty() ? nullptr : m_resourcesByRequest.back().outerMids.back());

  //incrementing starting index
  //1. 0(NULL) is invalid memid
  //2. back is last index not new one
  seq_initializer();

  std::generate_n(std::back_inserter(newTexture.outerMids), request->NumFrameSuggested, seq_initializer);

  //saving texture resources
  m_resourcesByRequest.push_back(newTexture);

  //providing pointer to mids externally
  response->mids = &m_resourcesByRequest.back().outerMids.front();
  response->NumFrameActual = request->NumFrameSuggested;

  //iterator prior end()
  auto it_last = m_resourcesByRequest.end();
  //fill map
  std::fill_n(std::back_inserter(m_memIdMap), request->NumFrameSuggested, --it_last);

  return MFX_ERR_NONE;
}

DXGI_FORMAT CD3D11FrameAllocator::GetDXGIFormat(mfxU32 fourcc)
{
  switch (fourcc)
  {
  case MFX_FOURCC_NV12:
    return DXGI_FORMAT_NV12;

  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

} // namespace MFX
