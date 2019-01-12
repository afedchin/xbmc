/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDCodecs/Video/DXVA.h"
#include <wrl/client.h>

class CVideoBuffer;
struct VideoPicture;

enum EBufferFormat
{
  BUFFER_FMT_NONE = 0,
  BUFFER_FMT_YUV420P,
  BUFFER_FMT_YUV420P10,
  BUFFER_FMT_YUV420P16,
  BUFFER_FMT_NV12,
  BUFFER_FMT_UYVY422,
  BUFFER_FMT_YUYV422,
  BUFFER_FMT_D3D11_BYPASS,
  BUFFER_FMT_D3D11_NV12,
  BUFFER_FMT_D3D11_P010,
  BUFFER_FMT_D3D11_P016,
};

class CVideoSettings;

struct IVideoSettingsHolder
{
  virtual ~IVideoSettingsHolder() = default;
  virtual CVideoSettings* GetVideoSettings() = 0;
};

struct SRenderBufferDesc
{
  EBufferFormat Format = BUFFER_FMT_NONE;
  unsigned Width = 0;
  unsigned Height = 0;
  bool Software = false;
  bool MultiView = false;
  IVideoSettingsHolder* VideoSettings = nullptr;
};

class CRenderBuffer
{
public:
  explicit CRenderBuffer();
  ~CRenderBuffer();

  void Release();     // Release any allocated resource
  void Lock();        // Prepare the buffer to receive data from VideoPlayer
  void Unlock();      // VideoPlayer finished filling the buffer with data
  void Clear();       // clear the buffer with solid black

  bool CreateBuffer(const SRenderBufferDesc& desc);
  bool UploadBuffer();
  void AppendPicture(const VideoPicture& picture);
  void ReleasePicture();

  unsigned int GetActivePlanes() const
  {
    return m_activePlanes;
  }
  HRESULT GetResource(ID3D11Resource** ppResource, unsigned* index);
  ID3D11View* GetView(unsigned idx = 0);

  void GetDataPtr(unsigned idx, void** pData, int* pStride);
  bool MapPlane(unsigned idx, void** pData, int* pStride);
  bool UnmapPlane(unsigned idx);

  unsigned GetWidth() const
  {
    return m_widthTex;
  }
  unsigned GetHeight() const
  {
    return m_heightTex;
  }
  bool IsValid() const
  {
    return m_activePlanes > 0;
  }
  void QueueCopyBuffer();
  bool IsLoaded();

  unsigned int frameIdx;
  unsigned int pictureFlags = 0;
  EBufferFormat format;
  CVideoBuffer* videoBuffer;
  AVColorPrimaries primaries;
  AVColorSpace color_space;
  AVColorTransferCharacteristic color_transfer;
  bool full_range;
  int bits;
  uint8_t texBits;

  bool hasDisplayMetadata = false;
  bool hasLightMetadata = false;
  AVMasteringDisplayMetadata displayMetadata;
  AVContentLightMetadata lightMetadata;

private:
  bool CopyToD3D11();
  bool CopyToStaging();
  void CopyFromStaging();
  bool CopyBuffer();
  HRESULT GetHWResource(ID3D11Resource** ppResource, unsigned* arrayIdx);

  void SetLoaded(bool loaded);
  bool IsLocked();
  void SetLocked(bool locked);
  CD3DTexture* GetTextures();
  D3D11_MAPPED_SUBRESOURCE* GetRects();
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* GetPlanes();
  bool UseExtendedView() const;
  bool UseExtendedBuffer() const;

  bool m_locked;
  bool m_loaded;
  bool m_bPending;
  bool m_soft;
  bool m_isMultiView;

  // video buffer size
  unsigned int m_width;
  unsigned int m_height;
  // real render bufer size
  unsigned int m_widthTex;
  unsigned int m_heightTex;
  unsigned int m_activePlanes;
  D3D11_MAP m_mapType;
  CD3D11_TEXTURE2D_DESC m_sDesc;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_planes[2];

  D3D11_MAPPED_SUBRESOURCE m_rects[YuvImage::MAX_PLANES];
  CD3DTexture m_textures[YuvImage::MAX_PLANES];

  // multi view
  bool m_loadedEx = false;
  bool m_lockedEx = false;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_planesEx[2];
  D3D11_MAPPED_SUBRESOURCE m_rectsEx[YuvImage::MAX_PLANES];
  CD3DTexture m_texturesEx[YuvImage::MAX_PLANES];
  std::string m_stereoMode;
  IVideoSettingsHolder* m_videoSettings = nullptr;
};
