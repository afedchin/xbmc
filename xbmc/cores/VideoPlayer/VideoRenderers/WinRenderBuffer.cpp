/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <ppl.h>
#include <ppltasks.h>

#include "WinRenderBuffer.h"
#include "cores/VideoPlayer/VideoRenderers/WinRenderer.h"
#ifdef HAVE_LIBMFX
#include "cores/VideoPlayer/DVDCodecs/Video/MFXCodec.h"
#endif
#include "platform/win32/utils/memcpy_sse2.h"
#if defined(HAVE_SSE2)
#include "platform/win32/utils/gpu_memcpy_sse4.h"
#endif
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "settings/MediaSettings.h"
#include "utils/CPUInfo.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#define PLANE_Y 0
#define PLANE_U 1
#define PLANE_V 2
#define PLANE_UV 1
#define PLANE_D3D11 0

static DXGI_FORMAT plane_formats[][2] = {
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM},    // NV12
    {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM}, // P010
    {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM}  // P016
};

using namespace Microsoft::WRL;

CRenderBuffer::CRenderBuffer()
    : frameIdx(0)
    , format(BUFFER_FMT_NONE)
    , videoBuffer(nullptr)
    , primaries(AVCOL_PRI_UNSPECIFIED)
    , color_space(AVCOL_SPC_BT709)
    , full_range(false)
    , bits(8)
    , texBits(8)
    , m_locked(false)
    , m_loaded(false)
    , m_bPending(false)
    , m_soft(false)
    , m_isMultiView(false)
    , m_width(0)
    , m_height(0)
    , m_widthTex(0)
    , m_heightTex(0)
    , m_activePlanes(0)
    , m_mapType(D3D11_MAP_WRITE_DISCARD)
    , m_staging(nullptr)
    , m_rects()
    , m_rectsEx()
{
}

CRenderBuffer::~CRenderBuffer()
{
  Release();
}

void CRenderBuffer::Release()
{
  ReleasePicture();

  m_staging = nullptr;
  for (size_t i = 0; m_locked && i < m_activePlanes; i++)
  {
    // unlock before release
    if (m_textures[i].Get() && m_rects[i].pData)
      m_textures[i].UnlockRect(0);

    m_textures[i].Release();
    m_rects[i] = {};

    if (m_isMultiView)
    {
      // unlock before release
      if (m_texturesEx[i].Get() && m_rectsEx[i].pData)
        m_texturesEx[i].UnlockRect(0);

      m_texturesEx[i].Release();
      m_rectsEx[i] = {};
    }
  }

  m_activePlanes = 0;
  texBits = 8;
  bits = 8;
  pictureFlags = 0;
  m_locked = false;
  m_lockedEx = false;
  m_videoSettings = nullptr;
}

void CRenderBuffer::Lock()
{
  if (IsLocked())
    return;

  SetLocked(true);

  CD3DTexture* textures = GetTextures();
  D3D11_MAPPED_SUBRESOURCE* rects = GetRects();

  for (unsigned i = 0; i < m_activePlanes; i++)
  {
    if (!textures[i].Get())
      continue;

    if (textures[i].LockRect(0, &rects[i], m_mapType) == false)
    {
      memset(&rects[i], 0, sizeof(D3D11_MAPPED_SUBRESOURCE));
      CLog::Log(LOGERROR, "%s - failed to lock texture %d into memory", __FUNCTION__, i);
      SetLocked(false);
    }
  }
}

void CRenderBuffer::Unlock()
{
  if (!IsLocked())
    return;

  SetLocked(false);

  CD3DTexture* textures = GetTextures();
  D3D11_MAPPED_SUBRESOURCE* rects = GetRects();

  for (unsigned i = 0; i < m_activePlanes; i++)
  {
    if (textures[i].Get() && rects[i].pData)
      if (!textures[i].UnlockRect(0))
        CLog::Log(LOGERROR, "% - failed to unlock texture %d", __FUNCTION__, i);

    memset(&rects[i], 0, sizeof(D3D11_MAPPED_SUBRESOURCE));
  }
}

void CRenderBuffer::Clear()
{
  // Set Y to 0 and U,V to 128 (RGB 0,0,0) to avoid visual artifacts
  switch (format)
  {
  case BUFFER_FMT_YUV420P:
    memset(m_rects[PLANE_Y].pData, 0, m_rects[PLANE_Y].RowPitch * m_heightTex);
    memset(m_rects[PLANE_U].pData, 0x80, m_rects[PLANE_U].RowPitch * (m_heightTex >> 1));
    memset(m_rects[PLANE_V].pData, 0x80, m_rects[PLANE_V].RowPitch * (m_heightTex >> 1));
    break;
  case BUFFER_FMT_YUV420P10:
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_Y].pData), 0,     m_rects[PLANE_Y].RowPitch * m_heightTex >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_U].pData), 0x200, m_rects[PLANE_U].RowPitch * (m_heightTex >> 1) >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_V].pData), 0x200, m_rects[PLANE_V].RowPitch * (m_heightTex >> 1) >> 1);
    break;
  case BUFFER_FMT_YUV420P16:
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_Y].pData), 0,      m_rects[PLANE_Y].RowPitch * m_heightTex >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_U].pData), 0x8000, m_rects[PLANE_U].RowPitch * (m_heightTex >> 1) >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_V].pData), 0x8000, m_rects[PLANE_V].RowPitch * (m_heightTex >> 1) >> 1);
    break;
  case BUFFER_FMT_NV12:
    memset(m_rects[PLANE_Y].pData, 0, m_rects[PLANE_Y].RowPitch * m_heightTex);
    memset(m_rects[PLANE_UV].pData, 0x80, m_rects[PLANE_UV].RowPitch * (m_heightTex >> 1));

    if (m_isMultiView)
    {
      memset(m_rectsEx[PLANE_Y].pData, 0, m_rectsEx[PLANE_Y].RowPitch * m_heightTex);
      memset(m_rectsEx[PLANE_UV].pData, 0x80, m_rectsEx[PLANE_UV].RowPitch * (m_heightTex >> 1));
    }
    break;
  case BUFFER_FMT_D3D11_BYPASS:
    break;
  case BUFFER_FMT_D3D11_NV12:
  {
    uint8_t* uvData = static_cast<uint8_t*>(m_rects[PLANE_D3D11].pData) + m_rects[PLANE_D3D11].RowPitch * m_heightTex;
    memset(m_rects[PLANE_D3D11].pData, 0, m_rects[PLANE_D3D11].RowPitch * m_heightTex);
    memset(uvData, 0x80, m_rects[PLANE_D3D11].RowPitch * (m_heightTex >> 1));

    if (m_isMultiView)
    {
      uvData = static_cast<uint8_t*>(m_rectsEx[PLANE_D3D11].pData) + m_rectsEx[PLANE_D3D11].RowPitch * m_heightTex;
      memset(m_rectsEx[PLANE_D3D11].pData, 0, m_rectsEx[PLANE_D3D11].RowPitch * m_heightTex);
      memset(uvData, 0x80, m_rectsEx[PLANE_D3D11].RowPitch * (m_heightTex >> 1));
    }
    break;
  }
  case BUFFER_FMT_D3D11_P010:
  {
    wchar_t* uvData = static_cast<wchar_t*>(m_rects[PLANE_D3D11].pData) + m_rects[PLANE_D3D11].RowPitch * (m_heightTex >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_D3D11].pData), 0, m_rects[PLANE_D3D11].RowPitch * m_heightTex >> 1);
    wmemset(uvData, 0x200, m_rects[PLANE_D3D11].RowPitch * (m_heightTex >> 1) >> 1);
    break;
  }
  case BUFFER_FMT_D3D11_P016:
  {
    wchar_t* uvData = static_cast<wchar_t*>(m_rects[PLANE_D3D11].pData) + m_rects[PLANE_D3D11].RowPitch * (m_heightTex >> 1);
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_D3D11].pData), 0, m_rects[PLANE_D3D11].RowPitch * m_heightTex >> 1);
    wmemset(uvData, 0x8000, m_rects[PLANE_D3D11].RowPitch * (m_heightTex >> 1) >> 1);
    break;
  }
  case BUFFER_FMT_UYVY422:
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_Y].pData), 0x0080,
            m_rects[PLANE_Y].RowPitch * (m_heightTex >> 1));
    break;
  case BUFFER_FMT_YUYV422:
    wmemset(static_cast<wchar_t*>(m_rects[PLANE_Y].pData), 0x8000,
            m_rects[PLANE_Y].RowPitch * (m_heightTex >> 1));
    break;
  default:
    break;
  }
}

bool CRenderBuffer::CreateBuffer(const SRenderBufferDesc& desc)
{
  format = desc.Format;
  m_soft = desc.Software;
  m_width = desc.Width;
  m_height = desc.Height;
  m_widthTex = m_width;
  m_heightTex = m_height;
  m_isMultiView = desc.MultiView;
  m_videoSettings = desc.VideoSettings;

  m_mapType = D3D11_MAP_WRITE_DISCARD;
  D3D11_USAGE usage = D3D11_USAGE_DYNAMIC;

  if (m_soft)
  {
    m_mapType = D3D11_MAP_WRITE;
    usage = D3D11_USAGE_STAGING;
  }

  DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;

  switch (format)
  {
  case BUFFER_FMT_YUV420P10:
  case BUFFER_FMT_YUV420P16:
  {
    if (!m_textures[PLANE_Y].Create(m_widthTex, m_heightTex, 1, usage, DXGI_FORMAT_R16_UNORM) ||
        !m_textures[PLANE_U].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, DXGI_FORMAT_R16_UNORM) ||
        !m_textures[PLANE_V].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, DXGI_FORMAT_R16_UNORM))
      return false;

    m_activePlanes = 3;
    texBits = (format == BUFFER_FMT_YUV420P10) ? 10 : 16;
    break;
  }
  case BUFFER_FMT_YUV420P:
  {
    if (!m_textures[PLANE_Y].Create(m_widthTex, m_heightTex, 1, usage, DXGI_FORMAT_R8_UNORM) ||
        !m_textures[PLANE_U].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, DXGI_FORMAT_R8_UNORM) ||
        !m_textures[PLANE_V].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, DXGI_FORMAT_R8_UNORM))
      return false;

    m_activePlanes = 3;
    break;
  }
  case BUFFER_FMT_NV12:
  {
    DXGI_FORMAT uvFormat = DXGI_FORMAT_R8G8_UNORM;
    // FL 9.x doesn't support DXGI_FORMAT_R8G8_UNORM, so we have to use SNORM and correct values in shader
    if (!DX::Windowing()->IsFormatSupport(uvFormat, D3D11_FORMAT_SUPPORT_TEXTURE2D))
      uvFormat = DXGI_FORMAT_R8G8_SNORM;

    if (!m_textures[PLANE_Y].Create(m_widthTex, m_heightTex, 1, usage, DXGI_FORMAT_R8_UNORM) ||
        !m_textures[PLANE_UV].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, uvFormat))
      return false;

    if (m_isMultiView)
    {
      if (!m_texturesEx[PLANE_Y].Create(m_widthTex, m_heightTex, 1, usage, DXGI_FORMAT_R8_UNORM) ||
          !m_texturesEx[PLANE_UV].Create(m_widthTex >> 1, m_heightTex >> 1, 1, usage, uvFormat))
        return false;
    }

    m_activePlanes = 2;
    break;
  }
  case BUFFER_FMT_D3D11_BYPASS:
  {
    m_activePlanes = 2;
    break;
  }
  case BUFFER_FMT_D3D11_NV12:
  case BUFFER_FMT_D3D11_P010:
  case BUFFER_FMT_D3D11_P016:
  {
    // some drivers don't allow not aligned decoder textures.
    m_widthTex = FFALIGN(m_width, 32);
    m_heightTex = FFALIGN(m_height, 32);
    if (format == BUFFER_FMT_D3D11_NV12)
      dxgi_format = DXGI_FORMAT_NV12;
    else if (format == BUFFER_FMT_D3D11_P010)
      dxgi_format = DXGI_FORMAT_P010;
    else if (format == BUFFER_FMT_D3D11_P016)
      dxgi_format = DXGI_FORMAT_P016;

    if (!m_textures[PLANE_D3D11].Create(m_widthTex, m_heightTex, 1, usage, dxgi_format))
      return false;

    if (m_isMultiView)
    {
      if (!m_texturesEx[PLANE_D3D11].Create(m_widthTex, m_heightTex, 1, usage, dxgi_format))
        return false;
    }

    m_activePlanes = 2;
    break;
  }
  case BUFFER_FMT_UYVY422:
  {
    if (!m_textures[PLANE_Y].Create(m_widthTex >> 1, m_heightTex, 1, usage, DXGI_FORMAT_B8G8R8A8_UNORM))
      return false;

    m_activePlanes = 1;
    break;
  }
  case BUFFER_FMT_YUYV422:
  {
    if (!m_textures[PLANE_Y].Create(m_widthTex >> 1, m_heightTex, 1, usage, DXGI_FORMAT_B8G8R8A8_UNORM))
      return false;

    m_activePlanes = 1;
    break;
  }
  default:;
  }
  return true;
}

bool CRenderBuffer::UploadBuffer()
{
  if (!videoBuffer)
    return false;

  Lock();

  bool ret = false;
  switch (format)
  {
  case BUFFER_FMT_D3D11_BYPASS:
  {
    // rewrite dimension to actual values for proper usage in shaders
    // for DXVA buffer
    const auto dxvaBuf = dynamic_cast<DXVA::CDXVAOutputBuffer*>(videoBuffer);
    if (dxvaBuf)
    {
      m_widthTex = dxvaBuf->width;
      m_heightTex = dxvaBuf->height;
    }
#ifdef HAVE_LIBMFX
    // for MVC buffer
    const auto mvcBuf = dynamic_cast<CMVCPicture*>(videoBuffer);
    if (mvcBuf)
    {
      m_widthTex = mvcBuf->width;
      m_heightTex = mvcBuf->height;
    }
#endif
    ret = true;
    break;
  }
  case BUFFER_FMT_D3D11_NV12:
  case BUFFER_FMT_D3D11_P010:
  case BUFFER_FMT_D3D11_P016:
  {
    ret = CopyToD3D11();
    break;
  }
  case BUFFER_FMT_NV12:
  case BUFFER_FMT_YUV420P:
  case BUFFER_FMT_YUV420P10:
  case BUFFER_FMT_YUV420P16:
  case BUFFER_FMT_UYVY422:
  case BUFFER_FMT_YUYV422:
  {
    ret = CopyBuffer();
    break;
  }
  default:
    break;
  }

  Unlock();
  SetLoaded(ret);

  return ret;
}

void CRenderBuffer::AppendPicture(const VideoPicture& picture)
{
  videoBuffer = picture.videoBuffer;
  videoBuffer->Acquire();

  pictureFlags = picture.iFlags;
  primaries = static_cast<AVColorPrimaries>(picture.color_primaries);
  color_space = static_cast<AVColorSpace>(picture.color_space);
  color_transfer = static_cast<AVColorTransferCharacteristic>(picture.color_transfer);
  full_range = picture.color_range == 1;
  bits = picture.colorBits;
  m_stereoMode = picture.stereoMode;

  hasDisplayMetadata = picture.hasDisplayMetadata;
  displayMetadata = picture.displayMetadata;
  lightMetadata = picture.lightMetadata;
  if (picture.hasLightMetadata && picture.lightMetadata.MaxCLL)
    hasLightMetadata = true;

  if (videoBuffer->GetFormat() == AV_PIX_FMT_D3D11VA_VLD)
    QueueCopyBuffer();

  m_loaded = false;
  m_loadedEx = false;
}

void CRenderBuffer::ReleasePicture()
{
  if (videoBuffer)
    videoBuffer->Release();
  videoBuffer = nullptr;

  m_planes[0] = nullptr;
  m_planes[1] = nullptr;
  m_loaded = false;

  if (m_isMultiView)
  {
    m_planesEx[0] = nullptr;
    m_planesEx[1] = nullptr;
    m_loadedEx = false;
  }
  m_stereoMode = "";
}

HRESULT CRenderBuffer::GetResource(ID3D11Resource** ppResource, unsigned* index)
{
  if (!ppResource)
    return E_POINTER;

  if (format == BUFFER_FMT_D3D11_BYPASS)
  {
    unsigned arrayIdx = 0;
    HRESULT hr = GetHWResource(ppResource, &arrayIdx);
    if (FAILED(hr))
    {
      CLog::LogF(LOGERROR, "unable to open d3d11va resource.");
    }
    else if (index)
    {
      *index = arrayIdx;
    }
    return hr;
  }
  else
  {
    ComPtr<ID3D11Resource> pResource = GetTextures()[0].Get();
    *ppResource = pResource.Detach();
    if (index)
      *index = 0;
  }
  return S_OK;
}

ID3D11View* CRenderBuffer::GetView(unsigned idx)
{
  CD3DTexture* textures = GetTextures();

  switch (format)
  {
  case BUFFER_FMT_D3D11_BYPASS:
  {
    ComPtr<ID3D11ShaderResourceView>* planes = GetPlanes();

    if (planes[idx])
      return planes[idx].Get();

    unsigned arrayIdx;
    ComPtr<ID3D11Resource> pResource;
    ComPtr<ID3D11Device> pD3DDevice = DX::DeviceResources::Get()->GetD3DDevice();

    HRESULT hr = GetHWResource(pResource.GetAddressOf(), &arrayIdx);
    if (FAILED(hr))
    {
      CLog::LogF(LOGERROR, "unable to open d3d11va resource.");
      return nullptr;
    }

    DXGI_FORMAT format = DXGI_FORMAT_NV12; // nv12 by default
    auto dxva = dynamic_cast<DXVA::CDXVAOutputBuffer*>(videoBuffer);
    if (dxva)
      format = dxva->format;
    else if (!m_isMultiView)
      return nullptr;

    DXGI_FORMAT plane_format = plane_formats[format - DXGI_FORMAT_NV12][idx];
    CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2DARRAY, plane_format, 0, 1,
                                             arrayIdx, 1);

    hr = pD3DDevice->CreateShaderResourceView(pResource.Get(), &srvDesc, planes[idx].ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      CLog::LogF(LOGERROR, "unable to create SRV for decoder surface (%d)", plane_format);
      return nullptr;
    }

    return planes[idx].Get();
  }
  case BUFFER_FMT_D3D11_NV12:
    return textures[PLANE_D3D11].GetShaderResource(idx ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
  case BUFFER_FMT_D3D11_P010:
  case BUFFER_FMT_D3D11_P016:
    return textures[PLANE_D3D11].GetShaderResource(idx ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM);
  case BUFFER_FMT_NV12:
  case BUFFER_FMT_YUV420P:
  case BUFFER_FMT_YUV420P10:
  case BUFFER_FMT_YUV420P16:
  case BUFFER_FMT_UYVY422:
  case BUFFER_FMT_YUYV422:
  default:
  {
    return textures[idx].GetShaderResource();
  }
  }
}

void CRenderBuffer::GetDataPtr(unsigned idx, void** pData, int* pStride)
{
  D3D11_MAPPED_SUBRESOURCE* rects = GetRects();

  if (pData)
    *pData = rects[idx].pData;
  if (pStride)
    *pStride = rects[idx].RowPitch;
}

bool CRenderBuffer::MapPlane(unsigned idx, void** pData, int* pStride)
{
  CD3DTexture* textures = GetTextures();

  D3D11_MAPPED_SUBRESOURCE res;
  if (!textures[idx].LockRect(0, &res, D3D11_MAP_READ))
  {
    CLog::Log(LOGERROR, "%s - failed to lock buffer textures into memory.", __FUNCTION__);
    *pData = nullptr;
    *pStride = 0;
    return false;
  }

  *pData = res.pData;
  *pStride = res.RowPitch;
  return true;
}

bool CRenderBuffer::UnmapPlane(unsigned idx)
{
  CD3DTexture* textures = GetTextures();

  if (!textures[idx].UnlockRect(0))
  {
    CLog::Log(LOGERROR, "%s - failed to unlock buffer texture.", __FUNCTION__);
    return false;
  }
  return true;
}

void CRenderBuffer::QueueCopyBuffer()
{
  if (!videoBuffer)
    return;

  if (format < BUFFER_FMT_D3D11_BYPASS)
  {
    CopyToStaging();
  }
}

bool CRenderBuffer::CopyToD3D11()
{
  if (!IsLocked() || !GetRects()[PLANE_D3D11].pData)
    return false;

  // destination
  D3D11_MAPPED_SUBRESOURCE rect = GetRects()[PLANE_D3D11];
  uint8_t* pData = static_cast<uint8_t*>(rect.pData);
  uint8_t* dst[] = {pData, pData + m_heightTex * rect.RowPitch};
  int dstStride[] = {static_cast<int>(rect.RowPitch), static_cast<int>(rect.RowPitch)};

  // source
  uint8_t* src[3];
  int srcStrides[3];
#ifdef HAVE_LIBMFX
  if (UseExtendedBuffer())
  {
    auto mvc = dynamic_cast<CMVCPicture*>(videoBuffer);
    mvc->GetPlanesExt(src);
    mvc->GetStridesExt(srcStrides);
  }
  else
#endif
  {
    videoBuffer->GetPlanes(src);
    videoBuffer->GetStrides(srcStrides);
  }

  const unsigned width = m_width;
  const unsigned height = m_height;

  const AVPixelFormat buffer_format = videoBuffer->GetFormat();
  // copy to texture
  if (buffer_format == AV_PIX_FMT_NV12 || 
      buffer_format == AV_PIX_FMT_P010 ||
      buffer_format == AV_PIX_FMT_P016)
  {
    Concurrency::parallel_invoke(
        [&]() {
          // copy Y
          copy_plane(src[0], srcStrides[0], height, width, dst[0], dstStride[0]);
        },
        [&]() {
          // copy UV
          copy_plane(src[1], srcStrides[1], height >> 1, width, dst[1], dstStride[1]);
        });
    // copy cache size of UV line again to fix Intel cache issue
    copy_plane(src[1], srcStrides[1], 1, 32, dst[1], dstStride[1]);
  }
  // convert 8bit
  else if (buffer_format == AV_PIX_FMT_YUV420P)
  {
    Concurrency::parallel_invoke(
        [&]() {
          // copy Y
          copy_plane(src[0], srcStrides[0], height, width, dst[0], dstStride[0]);
        },
        [&]() {
          // convert U+V -> UV
          convert_yuv420_nv12_chrome(&src[1], &srcStrides[1], height, width, dst[1], dstStride[1]);
        });
    // copy cache size of UV line again to fix Intel cache issue
    // height and width multiplied by two because they will be divided by func
    convert_yuv420_nv12_chrome(&src[1], &srcStrides[1], 2, 64, dst[1], dstStride[1]);
  }
  // convert 10/16bit
  else if (buffer_format == AV_PIX_FMT_YUV420P10 || buffer_format == AV_PIX_FMT_YUV420P16)
  {
    const uint8_t bpp = buffer_format == AV_PIX_FMT_YUV420P10 ? 10 : 16;
    Concurrency::parallel_invoke(
        [&]() {
          // copy Y
          copy_plane(src[0], srcStrides[0], height, width, dst[0], dstStride[0], bpp);
        },
        [&]() {
          // convert U+V -> UV
          convert_yuv420_p01x_chrome(&src[1], &srcStrides[1], height, width, dst[1], dstStride[1],
                                     bpp);
        });
    // copy cache size of UV line again to fix Intel cache issue
    // height multiplied by two because it will be divided by func
    convert_yuv420_p01x_chrome(&src[1], &srcStrides[1], 2, 32, dst[1], dstStride[1], bpp);
  }
  return true;
}

bool CRenderBuffer::CopyToStaging()
{
  unsigned index;
  ComPtr<ID3D11Resource> pResource;
  HRESULT hr = GetHWResource(pResource.GetAddressOf(), &index);

  if (FAILED(hr))
  {
    CLog::LogF(LOGERROR, "unable to open d3d11va resource.");
    return false;
  }

  if (!m_staging)
  {
    // create staging texture
    ComPtr<ID3D11Texture2D> surface;
    if (SUCCEEDED(pResource.As(&surface)))
    {
      D3D11_TEXTURE2D_DESC tDesc;
      surface->GetDesc(&tDesc);

      CD3D11_TEXTURE2D_DESC sDesc(tDesc);
      sDesc.ArraySize = 1;
      sDesc.Usage = D3D11_USAGE_STAGING;
      sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sDesc.BindFlags = 0;

      if (SUCCEEDED(DX::DeviceResources::Get()->GetD3DDevice()->CreateTexture2D(
              &sDesc, nullptr, m_staging.GetAddressOf())))
        m_sDesc = sDesc;
    }
  }

  if (m_staging)
  {
    ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetImmediateContext();
    // queue copying content from decoder texture to temporary texture.
    // actual data copying will be performed before rendering
    pContext->CopySubresourceRegion(m_staging.Get(), D3D11CalcSubresource(0, 0, 1), 0, 0, 0,
                                    pResource.Get(), D3D11CalcSubresource(0, index, 1), nullptr);
    m_bPending = true;
  }

  return m_staging != nullptr;
}

void CRenderBuffer::CopyFromStaging()
{
  if (!IsLocked())
    return;

  D3D11_MAPPED_SUBRESOURCE* rects = GetRects();
  ComPtr<ID3D11DeviceContext> pContext(DX::DeviceResources::Get()->GetImmediateContext());
  D3D11_MAPPED_SUBRESOURCE rectangle;

  if (SUCCEEDED(pContext->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &rectangle)))
  {
    void* (*copy_func)(void* d, const void* s, size_t size) =
#if defined(HAVE_SSE2)
        ((g_cpuInfo.GetCPUFeatures() & CPU_FEATURE_SSE4) != 0) ? gpu_memcpy :
#endif
                                                               memcpy;

    uint8_t* s_y = static_cast<uint8_t*>(rectangle.pData);
    uint8_t* s_uv = static_cast<uint8_t*>(rectangle.pData) + m_sDesc.Height * rectangle.RowPitch;
    uint8_t* d_y = static_cast<uint8_t*>(rects[PLANE_Y].pData);
    uint8_t* d_uv = static_cast<uint8_t*>(rects[PLANE_UV].pData);

    if (rects[PLANE_Y].RowPitch == rectangle.RowPitch &&
        rects[PLANE_UV].RowPitch == rectangle.RowPitch)
    {
      Concurrency::parallel_invoke(
          [&]() {
            // copy Y
            copy_func(d_y, s_y, rectangle.RowPitch * m_height);
          },
          [&]() {
            // copy UV
            copy_func(d_uv, s_uv, rectangle.RowPitch * m_height >> 1);
          });
    }
    else
    {
      Concurrency::parallel_invoke(
          [&]() {
            // copy Y
            for (unsigned y = 0; y < m_height; ++y)
            {
              copy_func(d_y, s_y, rects[PLANE_Y].RowPitch);
              s_y += rectangle.RowPitch;
              d_y += rects[PLANE_Y].RowPitch;
            }
          },
          [&]() {
            // copy UV
            for (unsigned y = 0; y<m_height>> 1; ++y)
            {
              copy_func(d_uv, s_uv, rects[PLANE_UV].RowPitch);
              s_uv += rectangle.RowPitch;
              d_uv += rects[PLANE_UV].RowPitch;
            }
          });
    }
    pContext->Unmap(m_staging.Get(), 0);
  }
}

bool CRenderBuffer::CopyBuffer()
{
  const AVPixelFormat buffer_format = videoBuffer->GetFormat();
  D3D11_MAPPED_SUBRESOURCE* rects = GetRects();

  if (buffer_format == AV_PIX_FMT_D3D11VA_VLD)
  {
    if (m_bPending)
    {
      CopyFromStaging();
      m_bPending = false;
    }
    return true;
  }

  if (buffer_format == AV_PIX_FMT_YUV420P || buffer_format == AV_PIX_FMT_YUV420P10 ||
      buffer_format == AV_PIX_FMT_YUV420P16 || buffer_format == AV_PIX_FMT_NV12)
  {
    uint8_t* bufData[3];
    int srcLines[3];
#ifdef HAVE_LIBMFX
    if (UseExtendedBuffer())
    {
      auto mvc = dynamic_cast<CMVCPicture*>(videoBuffer);
      mvc->GetPlanesExt(bufData);
      mvc->GetStridesExt(srcLines);
    }
    else
#endif
    {
      videoBuffer->GetPlanes(bufData);
      videoBuffer->GetStrides(srcLines);
    }
    std::vector<Concurrency::task<void>> tasks;

    for (unsigned plane = 0; plane < m_activePlanes; ++plane)
    {
      uint8_t* dst = static_cast<uint8_t*>(rects[plane].pData);
      uint8_t* src = bufData[plane];
      int srcLine = srcLines[plane];
      int dstLine = rects[plane].RowPitch;
      int height = plane == 0 ? m_height : m_height >> 1;

      auto task = Concurrency::create_task([src, dst, srcLine, dstLine, height]() {
        if (srcLine == dstLine)
        {
          memcpy(dst, src, srcLine * height);
        }
        else
        {
          uint8_t* s = src;
          uint8_t* d = dst;
          for (int i = 0; i < height; ++i)
          {
            memcpy(d, s, std::min(srcLine, dstLine));
            d += dstLine;
            s += srcLine;
          }
        }
      });
      tasks.push_back(task);
    }

    // event based await is required on WinRT because
    // blocking WinRT STA threads with task.wait() isn't allowed
    auto sync = std::make_shared<Concurrency::event>();
    when_all(tasks.begin(), tasks.end()).then([&sync]() { sync->set(); });
    sync->wait();
    return true;
  }

  if (buffer_format == AV_PIX_FMT_YUYV422 || buffer_format == AV_PIX_FMT_UYVY422)
  {
    uint8_t* bufData[3];
    int srcLines[3];
    videoBuffer->GetPlanes(bufData);
    videoBuffer->GetStrides(srcLines);

    uint8_t* src = bufData[PLANE_Y];
    uint8_t* dst = static_cast<uint8_t*>(rects[PLANE_Y].pData);
    int srcLine = srcLines[PLANE_Y];
    int dstLine = rects[PLANE_Y].RowPitch;

    if (srcLine == dstLine)
    {
      memcpy(dst, src, dstLine * m_height);
    }
    else
    {
      for (unsigned i = 0; i < m_height; i++)
      {
        memcpy(dst, src, std::min(srcLine, dstLine));
        src += srcLine;
        dst += dstLine;
      }
    }
    return true;
  }
  return false;
}

HRESULT CRenderBuffer::GetHWResource(ID3D11Resource** ppResource, unsigned* arrayIdx)
{
  if (!ppResource)
    return E_POINTER;
  if (!arrayIdx)
    return E_POINTER;

  HRESULT hr = E_UNEXPECTED;
  ComPtr<ID3D11Resource> pResource;
  unsigned index = -1;

  auto dxva = dynamic_cast<DXVA::CDXVAOutputBuffer*>(videoBuffer);
  if (dxva)
  {
    if (dxva->shared)
    {
      HANDLE sharedHandle = dxva->GetHandle();
      if (sharedHandle == INVALID_HANDLE_VALUE)
        return E_HANDLE;

      ComPtr<ID3D11Device> pD3DDevice = DX::DeviceResources::Get()->GetD3DDevice();
      hr = pD3DDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Resource),
                                          reinterpret_cast<void**>(pResource.GetAddressOf()));
    }
    else
    {
      if (dxva->view)
      {
        dxva->view->GetResource(&pResource);
        hr = S_OK;
      }
      else
        hr = E_UNEXPECTED;
    }
    index = dxva->GetIdx();
  }
#ifdef HAVE_LIBMFX
  auto mvc = dynamic_cast<CMVCPicture*>(videoBuffer);
  if (mvc)
  {
    hr = mvc->GetHWResource(UseExtendedBuffer(), &pResource, &index);
  }
#endif

  if (SUCCEEDED(hr))
  {
    *ppResource = pResource.Detach();
    *arrayIdx = index;
  }

  return hr;
}

bool CRenderBuffer::IsLoaded()
{
  return UseExtendedView() ? m_loadedEx : m_loaded;
}

void CRenderBuffer::SetLoaded(bool loaded)
{
  if (UseExtendedView())
    m_loadedEx = loaded;
  else
    m_loaded = loaded;
}

bool CRenderBuffer::IsLocked()
{
  return UseExtendedView() ? m_lockedEx : m_locked;
}

void CRenderBuffer::SetLocked(bool locked)
{
  if (UseExtendedView())
    m_lockedEx = locked;
  else
    m_locked = locked;
}

CD3DTexture* CRenderBuffer::GetTextures()
{
  return UseExtendedView() ? m_texturesEx : m_textures;
}

D3D11_MAPPED_SUBRESOURCE* CRenderBuffer::GetRects()
{
  return UseExtendedView() ? m_rectsEx : m_rects;
}

ComPtr<ID3D11ShaderResourceView>* CRenderBuffer::GetPlanes()
{
  return UseExtendedView() ? m_planesEx : m_planes;
}

bool CRenderBuffer::UseExtendedView() const
{
  if (!m_isMultiView)
    return false;

  CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
  const RENDER_STEREO_MODE stereo_mode = context.GetStereoMode();

  if (stereo_mode != RENDER_STEREO_MODE_OFF && stereo_mode != RENDER_STEREO_MODE_MONO)
  {
    const int stereo_view = context.GetStereoView();
    if (m_videoSettings && m_videoSettings->GetVideoSettings()->m_StereoInvert)
      return stereo_view == RENDER_STEREO_VIEW_LEFT;

    return stereo_view == RENDER_STEREO_VIEW_RIGHT;
  }
  return false;
}

bool CRenderBuffer::UseExtendedBuffer() const
{
  return UseExtendedView() && m_stereoMode == "block_lr";
}
