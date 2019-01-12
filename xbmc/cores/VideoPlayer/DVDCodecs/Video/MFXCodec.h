/*
 *  Copyright (C) 2010-2016 Hendrik Leppkes
 *  http://www.1f0.de
 *  
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "DVDVideoCodec.h"
#include "threads/Event.h"
#include <d3d11.h>
#include <queue>
#include <vector>

extern "C"
{
#include <mfxcommon.h>
#include <mfxmvc.h>
#include <mfxvideo.h>
}

#define ASYNC_DEPTH 10

// forward declaration of utility classes
namespace MFX
{
class CAnnexBConverter;
class CMFXFrameAllocator;
struct mfxAllocatorParams;
} // namespace MFX

struct MVCBuffer
{
  bool queued = false;
  mfxSyncPoint sync = nullptr;
  mfxFrameSurface1 surface = {};
};

class CMVCPicture;

class CMVCContext : public IVideoBufferPool
{
public:
  CMVCContext(mfxSession session);
  virtual ~CMVCContext();

  // IVideoBufferPool overrides
  CVideoBuffer* Get() override;
  void Return(int id) override;
  // allocator
  bool Init(MFX::mfxAllocatorParams* params);
  bool AllocFrames(mfxVideoParam* mfxParams, mfxFrameAllocRequest* request);
  void Lock(CMVCPicture* pPicture) const;
  void Unlock(const CMVCPicture* pPicture) const;
  MFX::CMFXFrameAllocator* GetAllocatorPtr() const
  {
    return &(*m_frameAllocator);
  }
  // session management
  void SetDecoderReady()
  {
    m_closeDecoder = true;
  }
  mfxSession ResetSession(mfxSession session);
  // MVC buffers management
  MVCBuffer* GetFree();
  MVCBuffer* Queued(mfxFrameSurface1* pSurface, mfxSyncPoint sync);
  void Return(MVCBuffer* pBuffer);

private:
  bool m_useSysMem = false;
  mfxFrameAllocResponse m_mfxResponse = {};
  std::unique_ptr<MFX::CMFXFrameAllocator> m_frameAllocator;

  CCriticalSection m_BufferCritSec;
  std::vector<MVCBuffer*> m_BufferQueue;

  CCriticalSection m_section;
  std::vector<CMVCPicture*> m_out;
  std::deque<size_t> m_freeOut;

  bool m_closeDecoder = false;
  mfxSession m_mfxSession = nullptr;
};

class CMVCPicture : public CVideoBuffer
{
  friend CMVCContext;
public:
  virtual ~CMVCPicture() = default;

  void ApplyBuffers(MVCBuffer* pBaseView, MVCBuffer* pExtraBuffer);
  void SetHW(bool isHW);
  // main view
  void GetPlanes(uint8_t* (&planes)[YuvImage::MAX_PLANES]) override;
  void GetStrides(int (&strides)[YuvImage::MAX_PLANES]) override;
  // extended view
  void GetPlanesExt(uint8_t* (&planes)[YuvImage::MAX_PLANES]) const;
  void GetStridesExt(int (&strides)[YuvImage::MAX_PLANES]) const;
  HRESULT GetHWResource(bool extended, ID3D11Resource** ppResource, unsigned* index);

  MVCBuffer* baseView = nullptr;
  MVCBuffer* extraView = nullptr;
  mfxHDLPair baseHNDL = {nullptr, nullptr};
  mfxHDLPair extHNDL = {nullptr, nullptr};
  unsigned width = 0;
  unsigned height = 0;

private:
  CMVCPicture(size_t idx);
};

class CMFXCodec : public CDVDVideoCodec
{
public:
  CMFXCodec(CProcessInfo& processInfo);
  virtual ~CMFXCodec();

  static CDVDVideoCodec* Create(CProcessInfo& processInfo);
  static bool Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  VCReturn GetPicture(VideoPicture* pFrame) override;
  void SetSpeed(int iSpeed) override;
  unsigned GetAllowedReferences() override;
  void Reset() override
  {
    Flush();
  };
  const char* GetName() override
  {
    return "msdk mvc";
  };
  void SetCodecControl(int flags) override
  {
    m_codecControlFlags = flags;
  }
  bool SupportsExtension() override
  {
    return true;
  }

private:
  bool Init();
  bool Flush();
  bool FlushQueue();
  void FreeResources();
  bool AllocateMVCExtBuffers();
  bool AllocateFrames();
  int HandleOutput(MVCBuffer* pOutputBuffer);
  void ProcessOutput();
  void SyncOutput(MVCBuffer* pBaseView, MVCBuffer* pExtraView);

  mfxIMPL m_impl = 0;
  mfxSession m_mfxSession = nullptr;
  mfxVersion m_mfxVersion = {};

  bool m_bDecodeReady = false;
  mfxVideoParam m_mfxVideoParams = {};
  mfxExtBuffer* m_mfxExtParam[1] = {nullptr};
  mfxExtMVCSeqDesc m_mfxExtMVCSeq = {};

  uint8_t* m_pBuff = nullptr;
  uint32_t m_buffSize = 0;

  std::shared_ptr<CMVCContext> m_context;
  std::unique_ptr<MFX::CAnnexBConverter> m_pAnnexBConverter;

  std::string m_stereoMode;
  int m_codecControlFlags = 0;
  int m_speed = DVD_PLAYSPEED_NORMAL;

  std::queue<MVCBuffer*> m_baseViewQueue;
  std::queue<MVCBuffer*> m_extViewQueue;
  std::queue<CMVCPicture*> m_outputQueue;
};
