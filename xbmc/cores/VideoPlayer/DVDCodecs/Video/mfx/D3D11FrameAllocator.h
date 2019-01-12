/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "BaseFrameAllocator.h"
#include <d3d11.h>
#include <vector>

struct ID3D11VideoDevice;
struct ID3D11VideoContext;

namespace MFX
{

struct CD3D11AllocatorParams : mfxAllocatorParams
{
  ID3D11Device *pDevice;
  bool bUseSingleTexture;

  CD3D11AllocatorParams()
    : pDevice()
    , bUseSingleTexture()
  {
  }
};

class CD3D11FrameAllocator : public CBaseFrameAllocator
{
public:

  CD3D11FrameAllocator();
  virtual ~CD3D11FrameAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override;
  mfxStatus Close() override;
  ID3D11Device* GetD3D11Device() const { return m_initParams.pDevice; };
  mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) override;

protected:
  static DXGI_FORMAT GetDXGIFormat(mfxU32 fourcc);
  mfxStatus CheckRequestType(mfxFrameAllocRequest *request) override;
  mfxStatus ReleaseResponse(mfxFrameAllocResponse *response) override;
  mfxStatus AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;

  struct TextureResource
  {
    std::vector<mfxMemId> outerMids;
    std::vector<ID3D11Texture2D*> textures;
    bool bAlloc;

    TextureResource() : bAlloc(true)
    {
    }
    static bool isAllocated(TextureResource & that)
    {
      return that.bAlloc;
    }
    ID3D11Texture2D* GetTexture(mfxMemId id)
    {
      if (outerMids.empty())
        return nullptr;

      return textures[((uintptr_t)id - (uintptr_t)outerMids.front()) % textures.size()];
    }
    UINT GetSubResource(mfxMemId id)
    {
      if (outerMids.empty())
        return 0;

      return (UINT)(((uintptr_t)id - (uintptr_t)outerMids.front()) / textures.size());
    }
    void Release()
    {
      for (auto& texture : textures)
        texture->Release();

      textures.clear();
      //marking texture as deallocated
      bAlloc = false;
    }
  };

  class TextureSubResource
  {
    TextureResource * m_pTarget;
    ID3D11Texture2D * m_pTexture;
    UINT m_subResource;
  public:
    TextureSubResource(TextureResource * pTarget = nullptr, mfxMemId id = 0)
      : m_pTarget(pTarget)
      , m_pTexture()
      , m_subResource()
    {
      if (nullptr != m_pTarget && !m_pTarget->outerMids.empty())
      {
        ptrdiff_t idx = (uintptr_t)id - (uintptr_t)m_pTarget->outerMids.front();
        m_pTexture = m_pTarget->textures[idx % m_pTarget->textures.size()];
        m_subResource = (UINT)(idx / m_pTarget->textures.size());
      }
    }
    ID3D11Texture2D* GetTexture() const
    {
      return m_pTexture;
    }
    UINT GetSubResource() const
    {
      return m_subResource;
    }
    void Release() const
    {
      if (nullptr != m_pTarget)
        m_pTarget->Release();
    }
  };

  TextureSubResource GetResourceFromMid(mfxMemId);

  CD3D11AllocatorParams m_initParams;
  std::list<TextureResource> m_resourcesByRequest; //each alloc request generates new item in list
  std::vector<std::list<TextureResource>::iterator> m_memIdMap;
};

} // namespace MFX
