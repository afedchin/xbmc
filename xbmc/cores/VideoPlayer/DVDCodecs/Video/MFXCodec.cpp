/*
 *      Copyright (C) 2010-2016 Hendrik Leppkes
 *      http://www.1f0.de
 *      Copyright (C) 2005-2016 Team Kodi
 *      http://kodi.tv
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "MFXCodec.h"
#include "DVDClock.h"
#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "rendering/dx/RenderContext.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include "mfx/BaseFrameAllocator.h"
#include "mfx/D3D11FrameAllocator.h"
#include "mfx/GeneralAllocator.h"
#include "mfx/MfxDecoderUtils.h"

#define MSDK_IGNORE_RESULT(p, x)                                                                   \
  {                                                                                                \
    if ((x) == (p))                                                                                \
    {                                                                                              \
      (p) = MFX_ERR_NONE;                                                                          \
    }                                                                                              \
  }
#define MSDK_CHECK_RESULT(p, x)                                                                    \
  {                                                                                                \
    if ((x) > (p))                                                                                 \
    {                                                                                              \
      CLog::Log(LOGERROR, "%s: error code %d (%d)", __FUNCTION__, p, __LINE__);                    \
      return false;                                                                                \
    }                                                                                              \
  }

static uint32_t avc_quant(uint8_t* src, uint8_t* dst, int extralen)
{
  uint32_t cb = 0;
  uint8_t* src_end = src + extralen;
  uint8_t* dst_end = dst + extralen;
  src += 5;
  // Two runs, for sps and pps
  for (int i = 0; i < 2; i++)
  {
    for (int n = *(src++) & 0x1f; n > 0; n--)
    {
      const unsigned len = (static_cast<unsigned>(src[0]) << 8 | src[1]) + 2;
      if (src + len > src_end || dst + len > dst_end)
      {
        assert(0);
        break;
      }
      memcpy(dst, src, len);
      src += len;
      dst += len;
      cb += len;
    }
  }
  return cb;
}

//-----------------------------------------------------------------------------
// MVC Picture
//-----------------------------------------------------------------------------
CMVCPicture::CMVCPicture(size_t idx)
    : CVideoBuffer(static_cast<int>(idx))
{
  m_pixFormat = AV_PIX_FMT_NV12;
}

void CMVCPicture::ApplyBuffers(MVCBuffer* pBaseView, MVCBuffer* pExtraBuffer)
{
  baseView = pBaseView;
  extraView = pExtraBuffer;
  width = pBaseView->surface.Info.Width;
  height = pBaseView->surface.Info.Height;
}

void CMVCPicture::SetHW(bool isHW)
{
  m_pixFormat = isHW ? AV_PIX_FMT_D3D11VA_VLD : AV_PIX_FMT_NV12;
}

void CMVCPicture::GetPlanes(uint8_t* (&planes)[YuvImage::MAX_PLANES])
{
  if (baseView)
  {
    planes[0] = baseView->surface.Data.Y;
    planes[1] = baseView->surface.Data.UV;
  }
}

void CMVCPicture::GetStrides(int (&strides)[YuvImage::MAX_PLANES])
{
  if (baseView)
  {
    strides[0] = strides[1] = baseView->surface.Data.Pitch;
  }
}

void CMVCPicture::GetPlanesExt(uint8_t* (&planes)[YuvImage::MAX_PLANES]) const
{
  if (extraView)
  {
    planes[0] = extraView->surface.Data.Y;
    planes[1] = extraView->surface.Data.UV;
  }
}

void CMVCPicture::GetStridesExt(int (&strides)[YuvImage::MAX_PLANES]) const
{
  if (extraView)
  {
    strides[0] = strides[1] = extraView->surface.Data.Pitch;
  }
}

HRESULT CMVCPicture::GetHWResource(bool extended, ID3D11Resource** ppResource, unsigned* index)
{
  mfxHDLPair hndl = extended ? extHNDL : baseHNDL;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture = reinterpret_cast<ID3D11Texture2D*>(hndl.first);
  if (texture)
  {
    *ppResource = texture.Detach();
    *index = reinterpret_cast<unsigned>(hndl.second);
    return S_OK;
  }
  return E_POINTER;
}

//-----------------------------------------------------------------------------
// MVC Context
//-----------------------------------------------------------------------------
CMVCContext::CMVCContext(mfxSession session)
{
  m_BufferQueue.clear();
  m_mfxSession = session;
}

CMVCContext::~CMVCContext()
{
  for (auto it = m_BufferQueue.begin(); it != m_BufferQueue.end(); ++it)
    delete (*it);

  // this needed because closing decoder causes freeing frames
  // so we need to close it only if all frames are free
  if (m_closeDecoder)
  {
    MFXVideoDECODE_Close(m_mfxSession);
    m_closeDecoder = false;
  }
  ResetSession(nullptr);

  // delete frames and allocator
  if (m_frameAllocator)
  {
    m_frameAllocator->Free(m_frameAllocator->pthis, &m_mfxResponse);
    m_frameAllocator.reset();
  }
}

mfxSession CMVCContext::ResetSession(mfxSession session)
{
  MFXClose(m_mfxSession);
  m_mfxSession = session;
  return m_mfxSession;
}

MVCBuffer* CMVCContext::GetFree()
{
  CSingleLock lock(m_BufferCritSec);
  MVCBuffer* pBuffer = nullptr;

  const auto it = std::find_if(m_BufferQueue.begin(), m_BufferQueue.end(), [](MVCBuffer* item) {
    return !item->surface.Data.Locked && !item->queued;
  });
  if (it != m_BufferQueue.end())
    pBuffer = *it;

  if (!pBuffer)
    CLog::LogF(LOGERROR, "no more free buffers available (%d total)", m_BufferQueue.size());

  return pBuffer;
}

void CMVCContext::Return(MVCBuffer* pBuffer)
{
  if (!pBuffer)
    return;

  CSingleLock lock(m_BufferCritSec);
  if (pBuffer)
  {
    pBuffer->queued = false;
    pBuffer->sync = nullptr;
  }
}

MVCBuffer* CMVCContext::Queued(mfxFrameSurface1* pSurface, mfxSyncPoint sync)
{
  CSingleLock lock(m_BufferCritSec);

  const auto it = std::find_if(m_BufferQueue.begin(), m_BufferQueue.end(),
                               [pSurface](MVCBuffer* item) { return &item->surface == pSurface; });

  // skip not valid surface
  if (it == m_BufferQueue.end())
    return nullptr;

  MVCBuffer* pOutputBuffer = *it;
  pOutputBuffer->queued = true;
  pOutputBuffer->sync = sync;

  return pOutputBuffer;
}

CVideoBuffer* CMVCContext::Get()
{
  CSingleLock lock(m_section);

  CMVCPicture* retPic;
  if (!m_freeOut.empty())
  {
    const size_t idx = m_freeOut.front();
    m_freeOut.pop_front();
    retPic = m_out[idx];
  }
  else
  {
    const size_t idx = m_out.size();
    retPic = new CMVCPicture(idx);
    m_out.push_back(retPic);
  }

  retPic->Acquire(GetPtr());
  return retPic;
}

void CMVCContext::Return(int id)
{
  CSingleLock lock_ctx(m_section);
  CSingleLock lock_buff(m_BufferCritSec);

  auto buf = m_out[id];

  Unlock(buf);
  Return(buf->baseView);
  Return(buf->extraView);

  buf->baseView = nullptr;
  buf->extraView = nullptr;
  buf->baseHNDL = {nullptr, nullptr};
  buf->extHNDL = {nullptr, nullptr};

  m_freeOut.push_back(id);
}

bool CMVCContext::Init(MFX::mfxAllocatorParams* pParams)
{
  m_frameAllocator = std::make_unique<MFX::ÑGeneralAllocator>();
  mfxStatus sts = m_frameAllocator->Init(pParams);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  return true;
}

bool CMVCContext::AllocFrames(mfxVideoParam* mfxParams, mfxFrameAllocRequest* request)
{
  mfxStatus sts = m_frameAllocator->Alloc(m_frameAllocator->pthis, request, &m_mfxResponse);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  for (size_t i = 0; i < m_mfxResponse.NumFrameActual; ++i)
  {
    auto pBuffer = new MVCBuffer;
    pBuffer->surface.Info = mfxParams->mfx.FrameInfo;
    pBuffer->surface.Info.FourCC = MFX_FOURCC_NV12;
    pBuffer->surface.Data.MemId = m_mfxResponse.mids[i];

    m_BufferQueue.push_back(pBuffer);
  }

  m_useSysMem = mfxParams->IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
  return true;
}

void CMVCContext::Lock(CMVCPicture* pPicture) const
{
  if (m_useSysMem)
  {
    // get system memory pointers
    m_frameAllocator->Lock(m_frameAllocator->pthis, pPicture->baseView->surface.Data.MemId,
                           &pPicture->baseView->surface.Data);
    m_frameAllocator->Lock(m_frameAllocator->pthis, pPicture->extraView->surface.Data.MemId,
                           &pPicture->extraView->surface.Data);
    pPicture->SetHW(false);
  }
  else
  {
    // get HW references
    m_frameAllocator->GetHDL(m_frameAllocator->pthis, pPicture->baseView->surface.Data.MemId,
                             reinterpret_cast<mfxHDL*>(&pPicture->baseHNDL));
    m_frameAllocator->GetHDL(m_frameAllocator->pthis, pPicture->extraView->surface.Data.MemId,
                             reinterpret_cast<mfxHDL*>(&pPicture->extHNDL));
    pPicture->SetHW(true);
  }
}

void CMVCContext::Unlock(const CMVCPicture* pPicture) const
{
  if (pPicture->baseView->surface.Data.Y || pPicture->extraView->surface.Data.Y)
  {
    m_frameAllocator->Unlock(m_frameAllocator->pthis, pPicture->baseView->surface.Data.MemId,
                             &pPicture->baseView->surface.Data);
    m_frameAllocator->Unlock(m_frameAllocator->pthis, pPicture->extraView->surface.Data.MemId,
                             &pPicture->extraView->surface.Data);
  }
}

//-----------------------------------------------------------------------------
// MVC Decoder
//-----------------------------------------------------------------------------
CDVDVideoCodec* CMFXCodec::Create(CProcessInfo& processInfo)
{
  return new CMFXCodec(processInfo);
}

bool CMFXCodec::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("msdk mvc", &CMFXCodec::Create);
  return true;
}

CMFXCodec::CMFXCodec(CProcessInfo& processInfo)
    : CDVDVideoCodec(processInfo)
{
}

CMFXCodec::~CMFXCodec()
{
  FreeResources();
}

bool CMFXCodec::Init()
{
  mfxIMPL impl = MFX_IMPL_AUTO_ANY | MFX_IMPL_VIA_D3D11;
  mfxVersion version = {{8, 1}};

  mfxStatus sts = MFXInit(impl, &version, &m_mfxSession);
  if (sts != MFX_ERR_NONE)
  {
    // let's try with full auto
    impl = MFX_IMPL_AUTO_ANY | MFX_IMPL_VIA_ANY;
    sts = MFXInit(impl, &version, &m_mfxSession);
    if (sts != MFX_ERR_NONE)
    {
      CLog::LogF(LOGERROR, "MSDK not available");
      return false;
    }
  }

  // query actual API version
  MFXQueryVersion(m_mfxSession, &m_mfxVersion);
  MFXQueryIMPL(m_mfxSession, &m_impl);
  CLog::LogF(LOGNOTICE, "MSDK Initialized, version %d.%d", m_mfxVersion.Major, m_mfxVersion.Minor);
  if ((m_impl & 0xF00) == MFX_IMPL_VIA_D3D11)
    CLog::LogF(LOGDEBUG, "MSDK uses D3D11 API.");
  if ((m_impl & 0xF00) == MFX_IMPL_VIA_D3D9)
    CLog::LogF(LOGDEBUG, "MSDK uses D3D9 API.");
  if ((m_impl & 0xF) == MFX_IMPL_SOFTWARE)
    CLog::LogF(LOGDEBUG, "MSDK uses Pure Software Implementation.");
  if ((m_impl & 0xF) >= MFX_IMPL_HARDWARE)
    CLog::LogF(LOGDEBUG, "MSDK uses Hardware Accelerated Implementation (default device).");

  return true;
}

void CMFXCodec::FreeResources()
{
  while (!m_outputQueue.empty())
  {
    m_outputQueue.front()->Release();
    m_outputQueue.pop();
  }
  while (!m_baseViewQueue.empty())
  {
    m_context->Return(m_baseViewQueue.front());
    m_baseViewQueue.pop();
  }
  while (!m_extViewQueue.empty())
  {
    m_context->Return(m_extViewQueue.front());
    m_extViewQueue.pop();
  }
  m_context.reset();

  // delete MVC sequence buffers
  if (m_mfxExtMVCSeq.View)
  {
    delete m_mfxExtMVCSeq.View;
    m_mfxExtMVCSeq.View = nullptr;
  }
  if (m_mfxExtMVCSeq.ViewId)
  {
    delete m_mfxExtMVCSeq.ViewId;
    m_mfxExtMVCSeq.ViewId = nullptr;
  }
  if (m_mfxExtMVCSeq.OP)
  {
    delete m_mfxExtMVCSeq.OP;
    m_mfxExtMVCSeq.OP = nullptr;
  }
  // delete allocator
  m_pAnnexBConverter.reset();

  if (m_pBuff)
    av_freep(&m_pBuff);
}

bool CMFXCodec::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_VIDEOPLAYER_SUPPORTMVC))
    return false;

  if (hints.codec != AV_CODEC_ID_H264)
    return false;

  uint8_t* extradata;
  int extradata_size;

  const bool isMVC1 = CDVDCodecUtils::ProcessH264MVCExtradata(
      static_cast<uint8_t*>(hints.extradata), hints.extrasize, &extradata, &extradata_size);

  if (hints.codec_tag != MKTAG('A', 'M', 'V', 'C') && !isMVC1)
    return false;

  FreeResources();
  if (!Init())
    return false;

  // Init and reset video param arrays
  memset(&m_mfxVideoParams, 0, sizeof(m_mfxVideoParams));
  m_mfxVideoParams.mfx.CodecId = MFX_CODEC_AVC;
  m_mfxVideoParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  //m_mfxVideoParams.mfx.MaxDecFrameBuffering = 6;

  memset(&m_mfxExtMVCSeq, 0, sizeof(m_mfxExtMVCSeq));
  m_mfxExtMVCSeq.Header.BufferId = MFX_EXTBUFF_MVC_SEQ_DESC;
  m_mfxExtMVCSeq.Header.BufferSz = sizeof(m_mfxExtMVCSeq);
  m_mfxExtParam[0] = reinterpret_cast<mfxExtBuffer*>(&m_mfxExtMVCSeq);

  // Attach ext params to VideoParams
  m_mfxVideoParams.ExtParam = m_mfxExtParam;
  m_mfxVideoParams.NumExtParam = 1;

  m_pBuff = static_cast<uint8_t*>(av_malloc(1024 * 2048)); // reserve 2Mb buffer
  m_buffSize = 0;

  // annex h
  if (isMVC1)
  {
    m_context = std::make_shared<CMVCContext>(m_mfxSession);

    m_pAnnexBConverter = std::make_unique<MFX::CAnnexBConverter>();
    m_pAnnexBConverter->SetNALUSize(2);

    const uint8_t naluSize = (extradata[4] & 3) + 1;
    uint8_t* pSequenceHeader = static_cast<uint8_t*>(malloc(extradata_size));
    const uint32_t cbSequenceHeader = avc_quant(extradata, pSequenceHeader, extradata_size);

    DemuxPacket packet;
    packet.pData = pSequenceHeader;
    packet.iSize = cbSequenceHeader;
    packet.dts = DVD_NOPTS_VALUE;
    packet.pts = DVD_NOPTS_VALUE;

    const auto result = AddData(packet);
    free(pSequenceHeader);

    if (!result)
      goto fail;

    m_pAnnexBConverter->SetNALUSize(naluSize);
  }
  else if (hints.codec_tag == MKTAG('A', 'M', 'V', 'C'))
  {
    // annex b
    if (hints.extradata && hints.extrasize > 0)
    {
      m_context = std::make_shared<CMVCContext>(m_mfxSession);

      DemuxPacket packet;
      packet.pData = static_cast<uint8_t*>(hints.extradata);
      packet.iSize = hints.extrasize;
      packet.dts = DVD_NOPTS_VALUE;
      packet.pts = DVD_NOPTS_VALUE;

      if (!AddData(packet))
        goto fail;
    }
  }
  else
    goto fail;

  if (hints.stereo_mode != "block_lr" && hints.stereo_mode != "block_rl")
    hints.stereo_mode = "block_lr";
  m_stereoMode = hints.stereo_mode;

  m_processInfo.SetVideoDimensions(hints.width, hints.height);
  m_processInfo.SetVideoDAR(static_cast<float>(hints.aspect));
  m_processInfo.SetVideoDeintMethod("none");

  return true;

fail:
  // reset stereo mode if it was set
  hints.stereo_mode = "mono";
  av_freep(&m_pBuff);
  return false;
}

bool CMFXCodec::AllocateMVCExtBuffers()
{
  mfxU32 i;
  // delete MVC sequence buffers
  if (m_mfxExtMVCSeq.View)
  {
    delete m_mfxExtMVCSeq.View;
    m_mfxExtMVCSeq.View = nullptr;
  }
  if (m_mfxExtMVCSeq.ViewId)
  {
    delete m_mfxExtMVCSeq.ViewId;
    m_mfxExtMVCSeq.ViewId = nullptr;
  }
  if (m_mfxExtMVCSeq.OP)
  {
    delete m_mfxExtMVCSeq.OP;
    m_mfxExtMVCSeq.OP = nullptr;
  }

  m_mfxExtMVCSeq.View = new mfxMVCViewDependency[m_mfxExtMVCSeq.NumView];
  for (i = 0; i < m_mfxExtMVCSeq.NumView; ++i)
  {
    memset(&m_mfxExtMVCSeq.View[i], 0, sizeof(m_mfxExtMVCSeq.View[i]));
  }
  m_mfxExtMVCSeq.NumViewAlloc = m_mfxExtMVCSeq.NumView;

  m_mfxExtMVCSeq.ViewId = new mfxU16[m_mfxExtMVCSeq.NumViewId];
  for (i = 0; i < m_mfxExtMVCSeq.NumViewId; ++i)
  {
    memset(&m_mfxExtMVCSeq.ViewId[i], 0, sizeof(m_mfxExtMVCSeq.ViewId[i]));
  }
  m_mfxExtMVCSeq.NumViewIdAlloc = m_mfxExtMVCSeq.NumViewId;

  m_mfxExtMVCSeq.OP = new mfxMVCOperationPoint[m_mfxExtMVCSeq.NumOP];
  for (i = 0; i < m_mfxExtMVCSeq.NumOP; ++i)
  {
    memset(&m_mfxExtMVCSeq.OP[i], 0, sizeof(m_mfxExtMVCSeq.OP[i]));
  }
  m_mfxExtMVCSeq.NumOPAlloc = m_mfxExtMVCSeq.NumOP;

  return true;
}

bool CMFXCodec::AllocateFrames()
{
  mfxStatus sts;
  bool bDecOutSysmem = (m_impl & 0xF) < MFX_IMPL_HARDWARE;

  // clone session for posible reuse
  mfxSession clone;
  MFXCloneSession(m_mfxSession, &clone);

  m_mfxVideoParams.IOPattern =
      bDecOutSysmem ? MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  m_mfxVideoParams.AsyncDepth = ASYNC_DEPTH - 2;

  // need to set device before query
#ifdef TARGET_WINDOWS
  sts = MFXVideoCORE_SetHandle(m_mfxSession, MFX_HANDLE_D3D11_DEVICE,
                               DX::DeviceResources::Get()->GetD3DDevice());
#elif
  // TODO linux device handle
#endif
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  mfxFrameAllocRequest mfxRequest;
  memset(&mfxRequest, 0, sizeof(mfxFrameAllocRequest));

  sts = MFXVideoDECODE_Query(m_mfxSession, &m_mfxVideoParams, &m_mfxVideoParams);
  if (sts == MFX_WRN_PARTIAL_ACCELERATION)
  {
    CLog::LogF(LOGWARNING, "SW implementation will be used instead of the HW implementation (%d).",
               sts);

    // change video params to use system memory - most efficient for sw
    m_mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    bDecOutSysmem = true;
    MSDK_IGNORE_RESULT(sts, MFX_WRN_PARTIAL_ACCELERATION);
  }
  MSDK_IGNORE_RESULT(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  // calculate number of surfaces required for decoder
  sts = MFXVideoDECODE_QueryIOSurf(m_mfxSession, &m_mfxVideoParams, &mfxRequest);

  // it's possible that current app device isn't an Intel device,
  // if so, we need to close current session and use cloned session above
  // because there is no a way to reset device handle in the session
  if (sts == MFX_ERR_UNSUPPORTED && (m_impl & 0xF) > MFX_IMPL_HARDWARE)
  {
    // close current and use cloned
    m_mfxSession = m_context->ResetSession(clone);

    // use sysmem output, because we can't use current device in mfx decoder
    m_mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    bDecOutSysmem = true;

    sts = MFXVideoDECODE_Query(m_mfxSession, &m_mfxVideoParams, &m_mfxVideoParams);
    MSDK_IGNORE_RESULT(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);
    sts = MFXVideoDECODE_QueryIOSurf(m_mfxSession, &m_mfxVideoParams, &mfxRequest);
  }
  else
  {
    // session clone was not useful, close it
    MFXClose(clone);
    clone = nullptr;
  }
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  if ((mfxRequest.NumFrameSuggested < m_mfxVideoParams.AsyncDepth) &&
      (m_impl & MFX_IMPL_HARDWARE_ANY))
    return false;

  m_processInfo.SetVideoDecoderName(GetName(), !bDecOutSysmem);
  CLog::LogF(LOGNOTICE, "using %s %s decoder.", GetName(), bDecOutSysmem ? "SW" : "HW");

  MFX::mfxAllocatorParams* pParams = nullptr;
#ifdef TARGET_WINDOWS
  if (!bDecOutSysmem)
  {
    const auto pD3DParams = new MFX::CD3D11AllocatorParams;
    pD3DParams->pDevice = DX::DeviceResources::Get()->GetD3DDevice();
    pD3DParams->bUseSingleTexture = true;
    pParams = pD3DParams;
  }
#elif
  // TODO linux allocator
#endif
  if (!m_context->Init(pParams))
    return false;

  uint8_t shared = m_mfxVideoParams.AsyncDepth + 4; // queue + two extra pairs of frames for safety
  shared += GetAllowedReferences() * 2;             // add extra frames for sharing

  uint16_t toAllocate = mfxRequest.NumFrameSuggested + shared;
  CLog::LogF(LOGDEBUG, "decoder suggested (%d) frames to use. creating (%d) buffers.",
             mfxRequest.NumFrameSuggested, toAllocate);

  mfxRequest.NumFrameSuggested = toAllocate;
  if (!m_context->AllocFrames(&m_mfxVideoParams, &mfxRequest))
    return false;

  sts = MFXVideoCORE_SetFrameAllocator(m_mfxSession, m_context->GetAllocatorPtr());
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  return true;
}

bool CMFXCodec::AddData(const DemuxPacket& packet)
{
  if (!m_mfxSession)
    return false;

  mfxStatus sts = MFX_ERR_NONE;
  mfxBitstream bs = {};
  bool bFlush = packet.pData == nullptr;

  bs.DecodeTimeStamp = packet.dts == DVD_NOPTS_VALUE ? MFX_TIMESTAMP_UNKNOWN : static_cast<mfxI64>(packet.dts);
  bs.TimeStamp = packet.pts == DVD_NOPTS_VALUE ? MFX_TIMESTAMP_UNKNOWN : static_cast<mfxU64>(packet.pts);

  if (!bFlush)
  {
    if (m_pAnnexBConverter)
    {
      uint8_t* pOutBuffer = nullptr;
      int pOutSize = 0;
      if (!m_pAnnexBConverter->Convert(&pOutBuffer, &pOutSize, packet.pData, packet.iSize))
        return false;

      memmove(m_pBuff + m_buffSize, pOutBuffer, pOutSize);
      m_buffSize += pOutSize;
      av_freep(&pOutBuffer);
    }
    else
    {
      memmove(m_pBuff + m_buffSize, packet.pData, packet.iSize);
      m_buffSize += packet.iSize;
    }

    MFX::CH264Nalu nalu;
    nalu.SetBuffer(m_pBuff, m_buffSize, 0);
    while (nalu.ReadNext())
    {
      if (nalu.GetType() == MFX::NALU_TYPE::NALU_TYPE_EOSEQ)
      {
        // This is rather ugly, and relies on the bit-stream being AnnexB, 
        // so simply overwriting the EOS NAL with zero works.
        // In the future a more elaborate bit-stream filter might be advised
        memset(m_pBuff + nalu.GetNALPos(), 0, 4);
      }
    }
    bs.Data = m_pBuff;
    bs.DataLength = m_buffSize;
    bs.MaxLength = bs.DataLength;
  }

  // waits buffer to init
  if (!m_bDecodeReady && bFlush)
    return true;

  if (!m_bDecodeReady)
  {
    sts = MFXVideoDECODE_DecodeHeader(m_mfxSession, &bs, &m_mfxVideoParams);
    if (sts == MFX_ERR_NOT_ENOUGH_BUFFER)
    {
      if (!AllocateMVCExtBuffers())
        return false;

      sts = MFXVideoDECODE_DecodeHeader(m_mfxSession, &bs, &m_mfxVideoParams);
    }

    if (sts == MFX_ERR_MORE_DATA)
    {
      CLog::LogF(LOGDEBUG, "no enought data to init decoder (%d)", sts);
      m_buffSize = 0;
      return true;
    }
    if (sts == MFX_ERR_NONE)
    {
      if (!AllocateFrames())
        return false;

      sts = MFXVideoDECODE_Init(m_mfxSession, &m_mfxVideoParams);
      if (sts < 0)
      {
        CLog::LogF(LOGERROR, "error initializing the MSDK decoder (%d)", sts);
        return false;
      }
      if (sts == MFX_WRN_PARTIAL_ACCELERATION)
        CLog::LogF(LOGWARNING,
                   "SW implementation will be used instead of the HW implementation (%d).", sts);

      if (m_mfxExtMVCSeq.NumView != 2)
      {
        CLog::LogF(LOGERROR, "only MVC with two views is supported");
        return false;
      }

      CLog::LogF(LOGDEBUG, "initialized MVC with view ids %d, %d", m_mfxExtMVCSeq.View[0].ViewId,
                 m_mfxExtMVCSeq.View[1].ViewId);

      m_context->SetDecoderReady();
      m_bDecodeReady = true;
      m_processInfo.SetVideoPixelFormat(
          m_mfxVideoParams.IOPattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY ? "d3d11_nv12" : "nv12");
      m_processInfo.SetVideoFps(static_cast<float>(m_mfxVideoParams.mfx.FrameInfo.FrameRateExtN) /
                                static_cast<float>(m_mfxVideoParams.mfx.FrameInfo.FrameRateExtD));
    }
  }

  if (!m_bDecodeReady)
    return false;

  mfxSyncPoint sync = nullptr;
  int resetCount = 0;

  // Loop over the decoder to ensure all data is being consumed
  XbmcThreads::EndTime timeout(25); // timeout for DEVICE_BUSY state.
  while (true)
  {
    MVCBuffer* pInputBuffer = m_context->GetFree();
    mfxFrameSurface1* outsurf = nullptr;
    sts = MFXVideoDECODE_DecodeFrameAsync(m_mfxSession, bFlush ? nullptr : &bs,
                                          &pInputBuffer->surface, &outsurf, &sync);

    if (sts == MFX_WRN_DEVICE_BUSY)
    {
      if (timeout.IsTimePast())
      {
        if (resetCount >= 1)
        {
          CLog::LogF(LOGERROR, "decoder did not respond after reset, flushing decoder.");
          Flush();
          return false;
        }
        CLog::LogF(LOGWARNING, "decoder did not respond within possible time, resetting decoder.");

        MFXVideoDECODE_Reset(m_mfxSession, &m_mfxVideoParams);
        resetCount++;
      }
      Sleep(5);
      continue;
    }
    // reset timeout timer
    timeout.Set(25);
    if (sts == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM)
    {
      m_buffSize = 0;
      bFlush = true;
      m_bDecodeReady = false;
      continue;
    }

    if (sync)
    {
      HandleOutput(m_context->Queued(outsurf, sync));
      continue;
    }

    if (sts != MFX_ERR_MORE_SURFACE && sts < 0)
      break;
  }

  if (!bs.DataOffset && !sync && !bFlush)
  {
    CLog::LogF(LOGERROR, "decoder did not consume any data, discarding");
    bs.DataOffset = m_buffSize;
  }

  if (bs.DataOffset < m_buffSize)
  {
    memmove(m_pBuff, m_pBuff + bs.DataOffset, m_buffSize - bs.DataOffset);
    m_buffSize -= bs.DataOffset;
  }
  else
    m_buffSize = 0;

  bool result = true;

  if (sts != MFX_ERR_MORE_DATA && sts < 0)
  {
    CLog::LogF(LOGERROR, "error from mfx call (%d)", sts);
    result = m_codecControlFlags & DVD_CODEC_CTRL_DRAIN;
  }

  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
    FlushQueue();

  /*if (!m_renderQueue.empty())
    result |= VC_PICTURE;
  if (sts == MFX_ERR_MORE_DATA && !(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
    result |= VC_BUFFER;*/

  return result;
}

int CMFXCodec::HandleOutput(MVCBuffer* pOutputBuffer)
{
  if (pOutputBuffer->surface.Info.FrameId.ViewId == 0)
    m_baseViewQueue.push(pOutputBuffer);
  else if (pOutputBuffer->surface.Info.FrameId.ViewId > 0)
    m_extViewQueue.push(pOutputBuffer);

  const uint16_t max = (m_mfxVideoParams.AsyncDepth >> 1) + 1;
  // process output if queue is full
  while (m_baseViewQueue.size() >= max && m_extViewQueue.size() >= max)
  {
    ProcessOutput();
  }
  return 0;
}

void CMFXCodec::ProcessOutput()
{
  MVCBuffer* pBaseView = m_baseViewQueue.front();
  MVCBuffer* pExtraView = m_extViewQueue.front();
  if (pBaseView->surface.Data.FrameOrder == pExtraView->surface.Data.FrameOrder)
  {
    SyncOutput(pBaseView, pExtraView);
    m_baseViewQueue.pop();
    m_extViewQueue.pop();
  }
  // drop unpaired frames
  else if (pBaseView->surface.Data.FrameOrder < pExtraView->surface.Data.FrameOrder)
  {
    m_context->Return(pBaseView);
    m_baseViewQueue.pop();
  }
  else if (pBaseView->surface.Data.FrameOrder > pExtraView->surface.Data.FrameOrder)
  {
    m_context->Return(pExtraView);
    m_extViewQueue.pop();
  }
}

#define RINT(x) ((x) >= 0 ? ((int)((x) + 0.5)) : ((int)((x)-0.5)))

CDVDVideoCodec::VCReturn CMFXCodec::GetPicture(VideoPicture* pFrame)
{
  if (!m_mfxSession)
    return VC_ERROR;

  // ask more data
  if (!m_bDecodeReady || m_outputQueue.empty() && !(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
    return VC_BUFFER;

  if (!m_outputQueue.empty())
  {
    if (pFrame->videoBuffer)
    {
      pFrame->videoBuffer->Release();
      pFrame->videoBuffer = nullptr;
    }

    CMVCPicture* pRenderPicture = m_outputQueue.front();
    m_outputQueue.pop();

    m_context->Lock(pRenderPicture);
    MVCBuffer *pBaseView = pRenderPicture->baseView;

    pFrame->iWidth = pBaseView->surface.Info.Width;
    pFrame->iHeight = pBaseView->surface.Info.Height;

    double aspect_ratio;
    if (pBaseView->surface.Info.AspectRatioH == 0)
      aspect_ratio = 0;
    else
      aspect_ratio = pBaseView->surface.Info.AspectRatioH /
                     static_cast<double>(pBaseView->surface.Info.AspectRatioW) *
                     pBaseView->surface.Info.CropW / pBaseView->surface.Info.CropH;

    if (aspect_ratio <= 0.0)
      aspect_ratio = static_cast<float>(pBaseView->surface.Info.CropW) /
                     static_cast<float>(pBaseView->surface.Info.CropH);

    pFrame->iDisplayHeight = pBaseView->surface.Info.CropH;
    pFrame->iDisplayWidth = static_cast<int>(RINT(pBaseView->surface.Info.CropH * aspect_ratio)) & -3;
    if (pFrame->iDisplayWidth > pFrame->iWidth)
    {
      pFrame->iDisplayWidth = pFrame->iWidth;
      pFrame->iDisplayHeight = static_cast<int>(RINT(pFrame->iWidth / aspect_ratio)) & -3;
    }
    pFrame->stereoMode = m_stereoMode;
    pFrame->color_range = 1;
    pFrame->colorBits = 8;

    pFrame->iFlags = 0;
    if (m_codecControlFlags & DVD_CODEC_CTRL_DROP)
      pFrame->iFlags |= DVP_FLAG_DROPPED;

    pFrame->iRepeatPicture = 0;
    pFrame->hasDisplayMetadata = false;
    pFrame->hasLightMetadata = false;

    pFrame->dts = DVD_NOPTS_VALUE;
    if (!(pBaseView->surface.Data.DataFlag & MFX_FRAMEDATA_ORIGINAL_TIMESTAMP))
      pBaseView->surface.Data.TimeStamp = MFX_TIMESTAMP_UNKNOWN;
    if (pBaseView->surface.Data.TimeStamp != MFX_TIMESTAMP_UNKNOWN)
      pFrame->pts = static_cast<double>(pBaseView->surface.Data.TimeStamp);
    else
      pFrame->pts = DVD_NOPTS_VALUE;

    pFrame->videoBuffer = pRenderPicture;

    int queued, discard, free;
    m_processInfo.GetRenderBuffers(queued, discard, free);
    if (free > 1)
      DX::Windowing()->RequestDecodingTime();
    else
      DX::Windowing()->ReleaseDecodingTime();

    return VC_PICTURE;
  }

  // if no more pictures send EOF
  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
    return VC_EOF;

  return VC_NONE;
}

void CMFXCodec::SetSpeed(int iSpeed)
{
  if (m_speed == iSpeed)
    return;

  m_speed = iSpeed;
  if (!m_bDecodeReady)
    return;

  switch (iSpeed)
  {
  case DVD_PLAYSPEED_PAUSE:
  case DVD_PLAYSPEED_NORMAL:
    MFXVideoDECODE_SetSkipMode(m_mfxSession, MFX_SKIPMODE_NOSKIP);
    break;
  default:
    MFXVideoDECODE_SetSkipMode(m_mfxSession, iSpeed < DVD_PLAYSPEED_NORMAL ? MFX_SKIPMODE_NOSKIP
                                                                           : MFX_SKIPMODE_MORE);
    break;
  }
}

unsigned CMFXCodec::GetAllowedReferences()
{
  return 5;
}

void CMFXCodec::SyncOutput(MVCBuffer* pBaseView, MVCBuffer* pExtraView)
{
  mfxStatus sts;

  assert(pBaseView->surface.Info.FrameId.ViewId == 0 &&
         pExtraView->surface.Info.FrameId.ViewId > 0);
  assert(pBaseView->surface.Data.FrameOrder == pExtraView->surface.Data.FrameOrder);

  // sync base view
  do
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pBaseView->sync, 1000);
  } while (sts == MFX_WRN_IN_EXECUTION);
  // sync extra view
  do
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pExtraView->sync, 1000);
  } while (sts == MFX_WRN_IN_EXECUTION);

  auto pMVCPicture = reinterpret_cast<CMVCPicture*>(m_context->Get());
  pMVCPicture->ApplyBuffers(pBaseView, pExtraView);

  m_outputQueue.push(pMVCPicture);
}

bool CMFXCodec::Flush()
{
  m_buffSize = 0;

  if (m_mfxSession)
  {
    if (m_bDecodeReady)
      MFXVideoDECODE_Reset(m_mfxSession, &m_mfxVideoParams);

    while (!m_outputQueue.empty())
    {
      m_outputQueue.front()->Release();
      m_outputQueue.pop();
    }
    while (!m_baseViewQueue.empty())
    {
      m_context->Return(m_baseViewQueue.front());
      m_baseViewQueue.pop();
    }
    while (!m_extViewQueue.empty())
    {
      m_context->Return(m_extViewQueue.front());
      m_extViewQueue.pop();
    }
  }

  return true;
}

bool CMFXCodec::FlushQueue()
{
  if (!m_bDecodeReady)
    return false;

  // Process all remaining frames in the queue
  while (!m_baseViewQueue.empty() && !m_extViewQueue.empty())
  {
    ProcessOutput();
  }
  return true;
}
