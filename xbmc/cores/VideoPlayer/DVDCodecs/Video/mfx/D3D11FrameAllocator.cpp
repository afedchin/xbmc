/*
 *      Copyright (C) 2005-2016 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifdef TARGET_WINDOWS

#include "D3D11FrameAllocator.h"
#include "system.h"
#include "utils/Log.h"
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
    mfxHDL y = x;
    x = (mfxHDL)(1 + (size_t)(x));
    return y;
  }
};


D3D11FrameAllocator::D3D11FrameAllocator()
{
}

D3D11FrameAllocator::~D3D11FrameAllocator()
{
  Close();
}

D3D11FrameAllocator::TextureSubResource D3D11FrameAllocator::GetResourceFromMid(mfxMemId mid)
{
  size_t index = (size_t)mid - 1;

  if (m_memIdMap.size() <= index)
    return TextureSubResource();

  //reverse iterator dereferencing
  TextureResource * p = &(*m_memIdMap[index]);
  if (!p->bAlloc)
    return TextureSubResource();

  return TextureSubResource(p, mid);
}

mfxStatus D3D11FrameAllocator::Init(mfxAllocatorParams *pParams)
{
  D3D11AllocatorParams *pd3d11Params = 0;
  pd3d11Params = dynamic_cast<D3D11AllocatorParams *>(pParams);

  if (nullptr == pd3d11Params ||
      nullptr == pd3d11Params->pDevice)
  {
    return MFX_ERR_NOT_INITIALIZED;
  }

  m_initParams = *pd3d11Params;

  return MFX_ERR_NONE;
}

mfxStatus D3D11FrameAllocator::Close()
{
  mfxStatus sts = BaseFrameAllocator::Close();
  for (referenceType i = m_resourcesByRequest.begin(); i != m_resourcesByRequest.end(); i++)
  {
    i->Release();
  }
  m_resourcesByRequest.clear();
  m_memIdMap.clear();

  return sts;
}

mfxStatus D3D11FrameAllocator::LockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  return MFX_ERR_UNSUPPORTED;
}

mfxStatus D3D11FrameAllocator::UnlockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  return MFX_ERR_UNSUPPORTED;
}

mfxStatus D3D11FrameAllocator::GetFrameHDL(mfxMemId mid, mfxHDL *handle)
{
  if (NULL == handle)
    return MFX_ERR_INVALID_HANDLE;

  TextureSubResource sr = GetResourceFromMid(mid);

  if (!sr.GetTexture())
    return MFX_ERR_INVALID_HANDLE;

  mfxHDLPair *pPair = (mfxHDLPair*)handle;

  pPair->first = sr.GetTexture();
  pPair->second = (mfxHDL)(UINT_PTR)sr.GetSubResource();

  return MFX_ERR_NONE;
}

mfxStatus D3D11FrameAllocator::CheckRequestType(mfxFrameAllocRequest *request)
{
  mfxStatus sts = BaseFrameAllocator::CheckRequestType(request);
  if (MFX_ERR_NONE != sts)
    return sts;

  if (request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET)
    return MFX_ERR_NONE;
  else
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus D3D11FrameAllocator::ReleaseResponse(mfxFrameAllocResponse *response)
{
  if (NULL == response)
    return MFX_ERR_NULL_PTR;

  if (response->mids && 0 != response->NumFrameActual)
  {
    //check whether texture exsist
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
mfxStatus D3D11FrameAllocator::AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  HRESULT hRes;

  DXGI_FORMAT colorFormat = ConverColortFormat(request->Info.FourCC);
  if (DXGI_FORMAT_UNKNOWN == colorFormat)
    return MFX_ERR_UNSUPPORTED;

  TextureResource newTexture;

  D3D11_TEXTURE2D_DESC desc = { 0 };

  desc.Width = request->Info.Width;
  desc.Height = request->Info.Height;

  desc.MipLevels = 1;
  //number of subresources is 1 in case of not single texture
  desc.ArraySize = m_initParams.bUseSingleTexture ? request->NumFrameSuggested : 1;
  desc.Format = ConverColortFormat(request->Info.FourCC);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  desc.BindFlags = D3D11_BIND_DECODER;

  ID3D11Texture2D* pTexture2D = nullptr;
  for (size_t i = 0; i < request->NumFrameSuggested / desc.ArraySize; i++)
  {
    hRes = m_initParams.pDevice->CreateTexture2D(&desc, nullptr, &pTexture2D);

    if (FAILED(hRes))
    {
      CLog::Log(LOGERROR, "%s: CreateTexture2D(%lld) failed, hr = 0x%08lx\n", __FUNCTION__, (long long)i, hRes);
      return MFX_ERR_MEMORY_ALLOC;
    }
    newTexture.textures.push_back(pTexture2D);
  }

  // mapping to self created handles array, starting from zero or from last assigned handle + 1
  sequence<mfxHDL> seq_initializer(m_resourcesByRequest.empty() ? 0 : m_resourcesByRequest.back().outerMids.back());

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
  std::list <TextureResource>::iterator it_last = m_resourcesByRequest.end();
  //fill map
  std::fill_n(std::back_inserter(m_memIdMap), request->NumFrameSuggested, --it_last);

  return MFX_ERR_NONE;
}

DXGI_FORMAT D3D11FrameAllocator::ConverColortFormat(mfxU32 fourcc)
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

#endif // TARGET_WINDOWS
