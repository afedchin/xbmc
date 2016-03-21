#pragma once
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

#include "BaseFrameAllocator.h"
#include <d3d11.h>
#include <vector>
#include <map>

struct ID3D11VideoDevice;
struct ID3D11VideoContext;

namespace MFX
{

struct D3D11AllocatorParams : mfxAllocatorParams
{
  ID3D11Device *pDevice;
  bool bUseSingleTexture;

  D3D11AllocatorParams()
    : pDevice()
    , bUseSingleTexture()
  {
  }
};

class D3D11FrameAllocator : public BaseFrameAllocator
{
public:

  D3D11FrameAllocator();
  virtual ~D3D11FrameAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override;
  mfxStatus Close() override;
  ID3D11Device * GetD3D11Device() { return m_initParams.pDevice; };
  mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) override;

protected:
  static  DXGI_FORMAT ConverColortFormat(mfxU32 fourcc);
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
        return NULL;

      return textures[((uintptr_t)id - (uintptr_t)outerMids.front()) % textures.size()];
    }
    UINT GetSubResource(mfxMemId id)
    {
      if (outerMids.empty())
        return NULL;

      return (UINT)(((uintptr_t)id - (uintptr_t)outerMids.front()) / textures.size());
    }
    void Release()
    {
      size_t i = 0;
      for (i = 0; i < textures.size(); i++)
      {
        textures[i]->Release();
      }
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
    TextureSubResource(TextureResource * pTarget = NULL, mfxMemId id = 0)
      : m_pTarget(pTarget)
      , m_pTexture()
      , m_subResource()
    {
      if (NULL != m_pTarget && !m_pTarget->outerMids.empty())
      {
        ptrdiff_t idx = (uintptr_t)id - (uintptr_t)m_pTarget->outerMids.front();
        m_pTexture = m_pTarget->textures[idx % m_pTarget->textures.size()];
        m_subResource = (UINT)(idx / m_pTarget->textures.size());
      }
    }
    ID3D11Texture2D* GetTexture()const
    {
      return m_pTexture;
    }
    UINT GetSubResource()const
    {
      return m_subResource;
    }
    void Release()
    {
      if (NULL != m_pTarget)
        m_pTarget->Release();
    }
  };

  TextureSubResource GetResourceFromMid(mfxMemId);

  D3D11AllocatorParams        m_initParams;
  std::list<TextureResource> m_resourcesByRequest; //each alloc request generates new item in list
  typedef std::list<TextureResource>::iterator referenceType;
  std::vector<referenceType>  m_memIdMap;
};

} // namespace MFX

#endif // TARGET_WINDOWS
