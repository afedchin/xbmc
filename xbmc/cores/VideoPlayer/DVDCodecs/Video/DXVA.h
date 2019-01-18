/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/Process/VideoBuffer.h"
#include "guilib/D3DResource.h"
#include "threads/Event.h"

#include <vector>
#include <wrl/client.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/d3d11va.h>
}

namespace DXVA
{
class CDecoder;

class COutputBuffer : public CVideoBuffer
{
  template<typename _TBuf>
  friend class CBufferPool;

public:
  virtual ~COutputBuffer();

  void SetRef(AVFrame* frame);
  void Unref();

  virtual void Initialize(CDecoder* decoder);
  virtual HRESULT GetResource(ID3D11Resource** ppResource);
  virtual unsigned GetIdx();

  ID3D11View* view = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  unsigned width = 0;
  unsigned height = 0;

protected:
  explicit COutputBuffer(int id);

private:
  AVFrame* m_pFrame{nullptr};
};

class COutputSharedBuffer : public COutputBuffer
{
  template<typename _TBuf>
  friend class CBufferPool;

public:
  HRESULT GetResource(ID3D11Resource** ppResource) override;
  void Initialize(CDecoder* decoder) override;

protected:
  explicit COutputSharedBuffer(int id)
      : COutputBuffer(id)
  {
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
  Microsoft::WRL::ComPtr<ID3D11Resource> m_sharedRes;
};
class COutputCopyBuffer : public COutputSharedBuffer
{
  template<typename _TBuf>
  friend class CBufferPool;

public:
  void Initialize(CDecoder* decoder) override;
  unsigned GetIdx() override { return 0; }

protected:
  explicit COutputCopyBuffer(int id)
      : COutputSharedBuffer(id)
  {
  }

  Microsoft::WRL::ComPtr<ID3D11Resource> m_copyRes;
};

class CContext
{
public:
  typedef std::shared_ptr<CContext> shared_ptr;
  typedef std::weak_ptr<CContext> weak_ptr;

  ~CContext();

  static shared_ptr EnsureContext(CDecoder* decoder);
  bool GetFormatAndConfig(AVCodecContext* avctx, D3D11_VIDEO_DECODER_DESC& format, D3D11_VIDEO_DECODER_CONFIG& config) const;
  bool CreateSurfaces(const D3D11_VIDEO_DECODER_DESC& format, uint32_t count, uint32_t alignment,
                      ID3D11VideoDecoderOutputView** surfaces, HANDLE* pHandle) const;
  bool CreateDecoder(const D3D11_VIDEO_DECODER_DESC& format, const D3D11_VIDEO_DECODER_CONFIG& config,
                     ID3D11VideoDecoder** decoder, ID3D11VideoContext** context);
  void Release(CDecoder* decoder);

  bool Check() const;
  bool Reset();
  bool IsContextShared() const
  {
    return m_sharingAllowed;
  }

private:
  explicit CContext() = default;

  void Close();
  bool CreateContext();
  void DestroyContext();
  void QueryCaps();
  bool IsValidDecoder(CDecoder* decoder);
  bool GetConfig(const D3D11_VIDEO_DECODER_DESC& format, D3D11_VIDEO_DECODER_CONFIG& config) const;

  static weak_ptr m_context;
  static CCriticalSection m_section;

  UINT m_input_count = 0;
  GUID* m_input_list = nullptr;
  bool m_atiWorkaround = false;
  bool m_sharingAllowed = false;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> m_pD3D11Context;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_pD3D11Device;
  std::vector<CDecoder*> m_decoders;
};

template<typename _TBuf>
class CBufferPool : public IVideoBufferPool
{
public:
  static_assert(std::is_base_of<COutputBuffer, _TBuf>::value, "_TBuf not derived from CDXVAOutputBuffer");
  typedef std::shared_ptr<CBufferPool<_TBuf>> shared_ptr;

  explicit CBufferPool();
  virtual ~CBufferPool();

  // IVideoBufferPool overrides
  CVideoBuffer* Get() override;
  void Return(int id) override;

  // views pool
  void AddView(ID3D11View* view);
  bool ReturnView(ID3D11View* view);
  ID3D11View* GetView();
  bool IsValid(ID3D11View* view);
  size_t Size();
  bool HasFree();
  bool HasRefs();

protected:
  void Reset();

  CCriticalSection m_section;

  std::vector<ID3D11View*> m_views;
  std::deque<size_t> m_freeViews;
  std::vector<COutputBuffer*> m_out;
  std::deque<size_t> m_freeOut;
};

class CDecoder : public IHardwareDecoder, public ID3DResource
{
public:
  ~CDecoder() override;

  static IHardwareDecoder* Create(CDVDStreamInfo& hint,
                                  CProcessInfo& processInfo,
                                  AVPixelFormat fmt);
  static bool Register();

  // IHardwareDecoder overrides
  bool Open(AVCodecContext* avctx, AVCodecContext* mainctx, const enum AVPixelFormat) override;
  CDVDVideoCodec::VCReturn Decode(AVCodecContext* avctx, AVFrame* frame) override;
  bool GetPicture(AVCodecContext* avctx, VideoPicture* picture) override;
  CDVDVideoCodec::VCReturn Check(AVCodecContext* avctx) override;
  const std::string Name() override { return "d3d11va"; }
  unsigned GetAllowedReferences() override;
  void Reset() override;

  // IDVDResourceCounted overrides
  long Release() override;

  bool OpenDecoder();
  int GetBuffer(AVCodecContext* avctx, AVFrame* pic);
  void ReleaseBuffer(uint8_t* data);
  void Close();
  void CloseDXVADecoder();

  //static members
  static bool Supports(enum AVPixelFormat fmt);
  static int FFGetBuffer(AVCodecContext* avctx, AVFrame* pic, int flags);
  static void FFReleaseBuffer(void* opaque, uint8_t* data);

protected:
  friend COutputBuffer;
  friend COutputSharedBuffer;
  friend COutputCopyBuffer;

  explicit CDecoder(CProcessInfo& processInfo);
  bool CheckInternal() const;

  enum EDeviceState
  {
    DXVA_OPEN,
    DXVA_RESET,
    DXVA_LOST
  } m_state = DXVA_OPEN;


  // ID3DResource overrides
  void OnCreateDevice() override
  {
    CSingleLock lock(m_section);
    m_state = DXVA_RESET;
    m_event.Set();
  }
  void OnDestroyDevice(bool fatal) override
  {
    CSingleLock lock(m_section);
    m_state = DXVA_LOST;
    m_event.Reset();
  }

  CEvent m_event;
  CCriticalSection m_section;
  CProcessInfo& m_processInfo;
  Microsoft::WRL::ComPtr<ID3D11VideoDecoder> m_pD3D11Decoder;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> m_pD3D11Context;
  CBufferPool<COutputBuffer>::shared_ptr m_bufferPool;
  CContext::shared_ptr m_dxvaContext;
  COutputBuffer* m_videoBuffer = nullptr;
  struct AVD3D11VAContext* m_avD3D11Context = nullptr;
  struct AVCodecContext* m_avCtx = nullptr;
  int m_refs = 0;
  unsigned int m_shared = 0;
  unsigned int m_surface_alignment = 0;
  HANDLE m_sharedHandle = INVALID_HANDLE_VALUE;
  D3D11_VIDEO_DECODER_DESC m_format = {};
};

} // namespace DXVA
