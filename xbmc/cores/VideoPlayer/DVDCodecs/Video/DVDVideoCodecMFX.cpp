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

#include "DVDVideoCodecMFX.h"
#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/DVDCodecs.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "utils/Log.h"
#include "windowing/WindowingFactory.h"
#include <iterator>

#include "mfx/BaseFrameAllocator.h"
#include "mfx/GeneralAllocator.h"
#include "mfx/D3D11FrameAllocator.h"

extern "C" {
#include "libavutil/intreadwrite.h"
}

#define MSDK_IGNORE_RESULT(p, x) {if ((x) == (p)) {p = MFX_ERR_NONE;}}
#define MSDK_CHECK_RESULT(p, x)   {if ((x) > (p)) { CLog::Log(LOGERROR, "%s: error code %d (%d)", __FUNCTION__, p, __LINE__); return false;}}

//-----------------------------------------------------------------------------
// static methods
//-----------------------------------------------------------------------------
static bool alloc_and_copy(uint8_t **poutbuf, int *poutbuf_size, const uint8_t *in, uint32_t in_size)
{
  uint32_t offset = *poutbuf_size;
  uint8_t nal_header_size = offset ? 3 : 4;
  void *tmp;

  *poutbuf_size += in_size + nal_header_size;
  tmp = av_realloc(*poutbuf, *poutbuf_size);
  if (!tmp)
    return false;
  *poutbuf = (uint8_t *)tmp;
  memcpy(*poutbuf + nal_header_size + offset, in, in_size);
  if (!offset) 
  {
    AV_WB32(*poutbuf, 1);
  }
  else 
  {
    (*poutbuf + offset)[0] = (*poutbuf + offset)[1] = 0;
    (*poutbuf + offset)[2] = 1;
  }

  return true;
}

static uint32_t avc_quant(uint8_t *src, uint8_t *dst, int extralen)
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
      unsigned len = (((unsigned)src[0] << 8) | src[1]) + 2;
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
// AnnexB Converter
//-----------------------------------------------------------------------------
bool CAnnexBConverter::Convert(uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size)
{
  int32_t nal_size;
  const uint8_t *buf_end = buf + buf_size;

  *poutbuf_size = 0;

  do 
  {
    if (buf + m_NaluSize > buf_end)
      goto fail;

    if (m_NaluSize == 1) 
      nal_size = buf[0];
    else if (m_NaluSize == 2) 
      nal_size = AV_RB16(buf);
    else 
    {
      nal_size = AV_RB32(buf);
      if (m_NaluSize == 3)
        nal_size >>= 8;
    }

    buf += m_NaluSize;

    if (buf + nal_size > buf_end || nal_size < 0)
      goto fail;

    if (!alloc_and_copy(poutbuf, poutbuf_size, buf, nal_size))
      goto fail;

    buf += nal_size;
    buf_size -= (nal_size + m_NaluSize);
  } 
  while (buf_size > 0);

  return true;
fail:
  av_freep(poutbuf);
  return false;
}

//-----------------------------------------------------------------------------
// MVC Context
//-----------------------------------------------------------------------------
CMVCContext::CMVCContext()
{
  m_BufferQueue.clear();
}

CMVCContext::~CMVCContext()
{
  for (auto it = m_BufferQueue.begin(); it != m_BufferQueue.end(); ++it)
    delete (*it);
}

void CMVCContext::AllocateBuffers(mfxFrameInfo &frameInfo, uint8_t numBuffers, mfxMemId* mids)
{
  for (size_t i = 0; i < numBuffers; ++i)
  {
    MVCBuffer *pBuffer = new MVCBuffer;
    pBuffer->surface.Info = frameInfo;
    pBuffer->surface.Data.MemId = mids[i];
    m_BufferQueue.push_back(pBuffer);
  }
}

MVCBuffer* CMVCContext::GetFree()
{
  CSingleLock lock(m_BufferCritSec);
  MVCBuffer *pBuffer = nullptr;

  auto it = std::find_if(m_BufferQueue.begin(), m_BufferQueue.end(),
                         [](MVCBuffer *item){
                           return !item->surface.Data.Locked && !item->queued && !item->render;
                         });
  if (it != m_BufferQueue.end())
    pBuffer = *it;

  if (!pBuffer)
    CLog::Log(LOGERROR, "No free buffers (%d total)", m_BufferQueue.size());

  return pBuffer;
}

MVCBuffer* CMVCContext::FindBuffer(mfxFrameSurface1* pSurface)
{
  CSingleLock lock(m_BufferCritSec);
  bool bFound = false;
  auto it = std::find_if(m_BufferQueue.begin(), m_BufferQueue.end(),
                        [pSurface](MVCBuffer *item){
                          return &item->surface == pSurface;
                        });
  if (it != m_BufferQueue.end())
    return *it;

  return nullptr;
}

void CMVCContext::ReleaseBuffer(MVCBuffer * pBuffer)
{
  if (!pBuffer)
    return;

  CSingleLock lock(m_BufferCritSec);
  if (pBuffer)
  {
    pBuffer->render = false;
    pBuffer->queued = false;
    pBuffer->sync = nullptr;
  }
}

MVCBuffer* CMVCContext::MarkQueued(mfxFrameSurface1 *pSurface, mfxSyncPoint sync)
{
  CSingleLock lock(m_BufferCritSec);

  MVCBuffer * pOutputBuffer = FindBuffer(pSurface);
  pOutputBuffer->render = false;
  pOutputBuffer->queued = true;
  pOutputBuffer->sync = sync;

  return pOutputBuffer;
}

MVCBuffer* CMVCContext::MarkRender(MVCBuffer* pBuffer)
{
  CSingleLock lock(m_BufferCritSec);

  pBuffer->queued = false;
  pBuffer->render = true;

  return pBuffer;
}

void CMVCContext::ClearRender(CMVCPicture *picture)
{
  CSingleLock lock(m_BufferCritSec);

  ReleaseBuffer(picture->baseView);
  ReleaseBuffer(picture->extraView);
  picture->baseView->render = false;
  picture->extraView->render = false;
}

CMVCPicture * CMVCContext::GetPicture(MVCBuffer *base, MVCBuffer *extended)
{
  CMVCPicture *pRenderPicture = new CMVCPicture(base, extended);
  pRenderPicture->context = this->Acquire();

  return pRenderPicture;
}

//-----------------------------------------------------------------------------
// MVC Picture
//-----------------------------------------------------------------------------
CMVCPicture::~CMVCPicture()
{
  context->ClearRender(this);
  SAFE_RELEASE(context);
}

void CMVCPicture::MarkRender()
{
  context->MarkRender(baseView);
  context->MarkRender(extraView);
}

//-----------------------------------------------------------------------------
// MVC Decoder
//-----------------------------------------------------------------------------
CDVDVideoCodecMFX::CDVDVideoCodecMFX(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo)
{
  m_mfxSession = nullptr;
  memset(&m_mfxExtMVCSeq, 0, sizeof(m_mfxExtMVCSeq));
  Init();
}

CDVDVideoCodecMFX::~CDVDVideoCodecMFX()
{
  DestroyDecoder(true);
}

bool CDVDVideoCodecMFX::Init()
{
  mfxIMPL impl = MFX_IMPL_AUTO_ANY | MFX_IMPL_VIA_D3D11;
  mfxVersion version = { 8, 1 };

  mfxStatus sts = MFXInit(impl, &version, &m_mfxSession);
  if (sts != MFX_ERR_NONE) 
  {
    // let's try with full auto
    impl = MFX_IMPL_AUTO_ANY | MFX_IMPL_VIA_ANY;
    sts = MFXInit(impl, &version, &m_mfxSession);
    if (sts != MFX_ERR_NONE)
    {
      CLog::Log(LOGERROR, "%s: MSDK not available", __FUNCTION__);
      return false;
    }
  }

  // query actual API version
  MFXQueryVersion(m_mfxSession, &m_mfxVersion);
  MFXQueryIMPL(m_mfxSession, &m_impl);
  CLog::Log(LOGNOTICE, "%s: MSDK Initialized, version %d.%d", __FUNCTION__, m_mfxVersion.Major, m_mfxVersion.Minor);
  if ((m_impl & 0xF00) == MFX_IMPL_VIA_D3D11)
    CLog::Log(LOGDEBUG, "%s: MSDK uses D3D11 API.", __FUNCTION__);
  if ((m_impl & 0xF00) == MFX_IMPL_VIA_D3D9)
    CLog::Log(LOGDEBUG, "%s: MSDK uses D3D9 API.", __FUNCTION__);
  if ((m_impl & 0xF) == MFX_IMPL_SOFTWARE)
    CLog::Log(LOGDEBUG, "%s: MSDK uses Pure Software Implementation.", __FUNCTION__);
  if ((m_impl & 0xF) >= MFX_IMPL_HARDWARE)
    CLog::Log(LOGDEBUG, "%s: MSDK uses Hardware Accelerated Implementation (default device).", __FUNCTION__);

  return true;
}

void CDVDVideoCodecMFX::DestroyDecoder(bool bFull)
{
  if (!m_mfxSession)
    return;

  if (m_bDecodeReady) 
  {
    MFXVideoDECODE_Close(m_mfxSession);
    m_bDecodeReady = false;
  }

  while (!m_renderQueue.empty())
  {
    SAFE_RELEASE(m_renderQueue.front());
    m_renderQueue.pop();
  }
  while (!m_baseViewQueue.empty())
  {
    m_context->ReleaseBuffer(m_baseViewQueue.front());
    m_baseViewQueue.pop();
  }
  while (!m_extViewQueue.empty())
  {
    m_context->ReleaseBuffer(m_extViewQueue.front());
    m_extViewQueue.pop();
  }
  SAFE_RELEASE(m_context);

  // delete frames
  if (m_frameAllocator)
    m_frameAllocator->Free(m_frameAllocator->pthis, &m_mfxResponse);

  SAFE_DELETE(m_frameAllocator);

  // delete MVC sequence buffers
  SAFE_DELETE(m_mfxExtMVCSeq.View);
  SAFE_DELETE(m_mfxExtMVCSeq.ViewId);
  SAFE_DELETE(m_mfxExtMVCSeq.OP);
  SAFE_DELETE(m_pAnnexBConverter);

  if (bFull && m_mfxSession)
  {
    MFXClose(m_mfxSession);
    m_mfxSession = nullptr;
  }
  if (m_pBuff)
    av_freep(&m_pBuff);
}

bool CDVDVideoCodecMFX::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (!m_mfxSession)
    goto fail;

  if (hints.codec != AV_CODEC_ID_H264_MVC && hints.codec != AV_CODEC_ID_H264)
    goto fail;
  if (hints.codec_tag != MKTAG('M', 'V', 'C', '1') && hints.codec_tag != MKTAG('A', 'M', 'V', 'C'))
    goto fail;

  DestroyDecoder(false);

  // Init and reset video param arrays
  memset(&m_mfxVideoParams, 0, sizeof(m_mfxVideoParams));
  m_mfxVideoParams.mfx.CodecId = MFX_CODEC_AVC;
  m_mfxVideoParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  //m_mfxVideoParams.mfx.MaxDecFrameBuffering = 6;

  memset(&m_mfxExtMVCSeq, 0, sizeof(m_mfxExtMVCSeq));
  m_mfxExtMVCSeq.Header.BufferId = MFX_EXTBUFF_MVC_SEQ_DESC;
  m_mfxExtMVCSeq.Header.BufferSz = sizeof(m_mfxExtMVCSeq);
  m_mfxExtParam[0] = (mfxExtBuffer *)&m_mfxExtMVCSeq;

  // Attach ext params to VideoParams
  m_mfxVideoParams.ExtParam = m_mfxExtParam;
  m_mfxVideoParams.NumExtParam = 1;

  uint8_t* extradata;
  int extradata_size;

  m_pBuff = (uint8_t*)av_malloc(1024 * 2048); // reserve 2Mb buffer
  m_buffSize = 0;

  for (auto it = options.m_keys.begin(); it != options.m_keys.end(); ++it)
  {
    if (it->m_name == "surfaces")
      m_shared = atoi(it->m_value.c_str());
  }

  // annex h
  if (hints.codec_tag == MKTAG('M', 'V', 'C', '1') &&
      CDVDCodecUtils::ProcessH264MVCExtradata((uint8_t*)hints.extradata, hints.extrasize, 
                                              &extradata, &extradata_size))
  {
    uint8_t naluSize = (extradata[4] & 3) + 1;
    uint8_t *pSequenceHeader = (uint8_t*)malloc(extradata_size);
    uint32_t cbSequenceHeader = avc_quant(extradata, pSequenceHeader, extradata_size);

    m_pAnnexBConverter = new CAnnexBConverter();
    m_pAnnexBConverter->SetNALUSize(2);

    m_context = new CMVCContext();

    int result = Decode(pSequenceHeader, cbSequenceHeader, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

    free(pSequenceHeader);
    if (result == VC_ERROR)
      goto fail;

    m_pAnnexBConverter->SetNALUSize(naluSize);
    SetStereoMode(hints);
    return true;
  }
  else if (hints.codec_tag == MKTAG('A', 'M', 'V', 'C'))
  {
    // annex b
    if (hints.extradata && hints.extrasize > 0)
    {
      m_context = new CMVCContext();

      int result = Decode((uint8_t*)hints.extradata, hints.extrasize, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
      if (result == VC_ERROR)
        goto fail;
    }
    SetStereoMode(hints);
    return true;
  }

fail:
  // reset stereo mode if it was set
  hints.stereo_mode = "mono";
  av_freep(&m_pBuff);
  return false;
}

bool CDVDVideoCodecMFX::AllocateMVCExtBuffers()
{
  mfxU32 i;
  SAFE_DELETE(m_mfxExtMVCSeq.View);
  SAFE_DELETE(m_mfxExtMVCSeq.ViewId);
  SAFE_DELETE(m_mfxExtMVCSeq.OP);

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

bool CDVDVideoCodecMFX::AllocateFrames()
{
  mfxStatus sts = MFX_ERR_NONE;
  bool bDecOutSysmem = (m_impl & MFX_IMPL_SOFTWARE);

  m_mfxVideoParams.IOPattern = bDecOutSysmem ? MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  m_mfxVideoParams.AsyncDepth = ASYNC_DEPTH - 2;

  // need to set device before query
#ifdef TARGET_WINDOWS
  sts = MFXVideoCORE_SetHandle(m_mfxSession, MFX_HANDLE_D3D11_DEVICE, g_Windowing.Get3D11Device());
#elif
  // TODO linux device handle
#endif
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  mfxFrameAllocRequest  mfxRequest;
  memset(&mfxRequest, 0, sizeof(mfxFrameAllocRequest));
  memset(&m_mfxResponse, 0, sizeof(mfxFrameAllocResponse));

  sts = MFXVideoDECODE_Query(m_mfxSession, &m_mfxVideoParams, &m_mfxVideoParams);
  MSDK_IGNORE_RESULT(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  // calculate number of surfaces required for decoder
  sts = MFXVideoDECODE_QueryIOSurf(m_mfxSession, &m_mfxVideoParams, &mfxRequest);
  if (sts == MFX_WRN_PARTIAL_ACCELERATION)
  {
    CLog::Log(LOGWARNING, "%s: SW implementation will be used instead of the HW implementation (%d).", __FUNCTION__, sts);
    bDecOutSysmem = true;
    MSDK_IGNORE_RESULT(sts, MFX_WRN_PARTIAL_ACCELERATION);
  }
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  if ((mfxRequest.NumFrameSuggested < m_mfxVideoParams.AsyncDepth) &&
    (m_impl & MFX_IMPL_HARDWARE_ANY))
    return false;

  // re-calculate number of surfaces required for decoder in case MFX_WRN_PARTIAL_ACCELERATION
  if (bDecOutSysmem && m_mfxVideoParams.IOPattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY)
  {
    m_mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    sts = MFXVideoDECODE_QueryIOSurf(m_mfxSession, &m_mfxVideoParams, &mfxRequest);
  }

  MFX::mfxAllocatorParams *pParams = nullptr;
  m_frameAllocator = new MFX::GeneralAllocator();
#ifdef TARGET_WINDOWS
  if (!bDecOutSysmem)
  {
    MFX::D3D11AllocatorParams *pD3DParams = new MFX::D3D11AllocatorParams;
    pD3DParams->pDevice = g_Windowing.Get3D11Device();
    pD3DParams->bUseSingleTexture = true;
    pParams = pD3DParams;
  }
#elif
  // TODO linux allocator
#endif
  sts = m_frameAllocator->Init(pParams);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  uint8_t shared = ASYNC_DEPTH + 2; // +2 for safety
  if (!bDecOutSysmem)
    shared += m_shared * 2; // m_shared * 2 buffers for sharing

  uint16_t toAllocate = mfxRequest.NumFrameSuggested + shared;
  CLog::Log(LOGDEBUG, "%s: Decoder suggested (%d) frames to use. creating (%d) buffers.", __FUNCTION__, mfxRequest.NumFrameSuggested, toAllocate);

  mfxRequest.NumFrameSuggested = toAllocate;
  sts = m_frameAllocator->Alloc(m_frameAllocator->pthis, &mfxRequest, &m_mfxResponse);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  m_context->AllocateBuffers(m_mfxVideoParams.mfx.FrameInfo, m_mfxResponse.NumFrameActual, m_mfxResponse.mids);

  sts = MFXVideoCORE_SetFrameAllocator(m_mfxSession, m_frameAllocator);
  MSDK_CHECK_RESULT(sts, MFX_ERR_NONE);

  return true;
}

int CDVDVideoCodecMFX::Decode(uint8_t* buffer, int buflen, double dts, double pts)
{
  if (!m_mfxSession)
    return VC_ERROR;

  mfxStatus sts = MFX_ERR_NONE;
  mfxBitstream bs = { { { 0 } } };;
  bool bBuffered = false, bFlush = (buffer == nullptr);

  bs.DecodeTimeStamp = (dts == DVD_NOPTS_VALUE) ? MFX_TIMESTAMP_UNKNOWN : (mfxI64)dts;
  bs.TimeStamp       = (pts == DVD_NOPTS_VALUE) ? MFX_TIMESTAMP_UNKNOWN : (mfxU64)pts;

  if (!bFlush)
  {
    if (m_pAnnexBConverter)
    {
      uint8_t *pOutBuffer = nullptr;
      int pOutSize = 0;
      if (!m_pAnnexBConverter->Convert(&pOutBuffer, &pOutSize, buffer, buflen))
        return VC_ERROR;

      memmove(m_pBuff + m_buffSize, pOutBuffer, pOutSize);
      m_buffSize += pOutSize;
      av_freep(&pOutBuffer);
    }
    else
    {
      memmove(m_pBuff + m_buffSize, buffer, buflen);
      m_buffSize += buflen;
    }

    CH264Nalu nalu;
    nalu.SetBuffer(m_pBuff, m_buffSize, 0);
    while (nalu.ReadNext())
    {
      if (nalu.GetType() == NALU_TYPE_EOSEQ)
      {
        // This is rather ugly, and relies on the bitstream being AnnexB, so simply overwriting the EOS NAL with zero works.
        // In the future a more elaborate bitstream filter might be advised
        memset(m_pBuff + nalu.GetNALPos(), 0, 4);
      }
    }
    bs.Data = m_pBuff;
    bs.DataLength = m_buffSize;
    bs.MaxLength = bs.DataLength;
  }

  // waits buffer to init
  if (!m_bDecodeReady && bFlush)
    return VC_BUFFER;

  if (!m_bDecodeReady) 
  {
    sts = MFXVideoDECODE_DecodeHeader(m_mfxSession, &bs, &m_mfxVideoParams);
    if (sts == MFX_ERR_NOT_ENOUGH_BUFFER) 
    {
      if (!AllocateMVCExtBuffers())
        return VC_ERROR;

      sts = MFXVideoDECODE_DecodeHeader(m_mfxSession, &bs, &m_mfxVideoParams);
    }

    if (sts == MFX_ERR_MORE_DATA)
    {
      CLog::Log(LOGDEBUG, "%s: No enought data to init decoder (%d)", __FUNCTION__, sts);
      m_buffSize = 0;
      return VC_BUFFER;
    }
    if (sts == MFX_ERR_NONE) 
    {
      if (!AllocateFrames())
        return VC_ERROR;

      sts = MFXVideoDECODE_Init(m_mfxSession, &m_mfxVideoParams);
      if (sts < 0)
      {
        CLog::Log(LOGERROR, "%s: Error initializing the MSDK decoder (%d)", __FUNCTION__, sts);
        return VC_ERROR;
      }
      if (sts == MFX_WRN_PARTIAL_ACCELERATION)
        CLog::Log(LOGWARNING, "%s: SW implementation will be used instead of the HW implementation (%d).", __FUNCTION__, sts);

      if (m_mfxExtMVCSeq.NumView != 2) 
      {
        CLog::Log(LOGERROR, "%s: Only MVC with two views is supported", __FUNCTION__);
        return VC_ERROR;
      }

      CLog::Log(LOGDEBUG, "%s: Initialized MVC with View Ids %d, %d", __FUNCTION__, m_mfxExtMVCSeq.View[0].ViewId, m_mfxExtMVCSeq.View[1].ViewId);
      m_bDecodeReady = true;
    }
  }

  if (!m_bDecodeReady)
    return VC_ERROR;

  mfxSyncPoint sync = nullptr;
  int resetCount = 0;

  // Loop over the decoder to ensure all data is being consumed
  XbmcThreads::EndTime timeout(25); // timeout for DEVICE_BUSY state.
  while (1) 
  {
    MVCBuffer *pInputBuffer = m_context->GetFree();
    mfxFrameSurface1 *outsurf = nullptr;
    sts = MFXVideoDECODE_DecodeFrameAsync(m_mfxSession, bFlush ? nullptr : &bs, &pInputBuffer->surface, &outsurf, &sync);

    if (sts == MFX_WRN_DEVICE_BUSY)
    {
      if (timeout.IsTimePast())
      {
        if (resetCount >= 1)
        {
          CLog::Log(LOGERROR, "%s: Decoder did not respond after reset, flushing decoder.", __FUNCTION__);
          return VC_FLUSHED;
        }
        CLog::Log(LOGWARNING, "%s: Decoder did not respond within possible time, resetting decoder.", __FUNCTION__);

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
      HandleOutput(m_context->MarkQueued(outsurf, sync));
      continue;
    }

    if (sts != MFX_ERR_MORE_SURFACE && sts < 0)
      break;
  }

  if (!bs.DataOffset && !sync && !bFlush) 
  {
    CLog::Log(LOGERROR, "%s: Decoder did not consume any data, discarding", __FUNCTION__);
    bs.DataOffset = m_buffSize;
  }

  if (bs.DataOffset < m_buffSize)
  {
    BYTE *p = m_pBuff;
    memmove(p, p + bs.DataOffset, m_buffSize - bs.DataOffset);
    m_buffSize -= bs.DataOffset;
  }
  else
    m_buffSize = 0;

  int result = 0;

  if (sts != MFX_ERR_MORE_DATA && sts < 0)
  {
    CLog::Log(LOGERROR, "%s: Error from Decode call (%d)", __FUNCTION__, sts);
    result = VC_ERROR;
  }

  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN) 
    FlushQueue();
  if (!m_renderQueue.empty())
    result |= VC_PICTURE;
  if (sts == MFX_ERR_MORE_DATA && !(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
    result |= VC_BUFFER;
  else if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN && !result)
    result |= VC_BUFFER;

  return result;
}

int CDVDVideoCodecMFX::HandleOutput(MVCBuffer * pOutputBuffer)
{
  if (pOutputBuffer->surface.Info.FrameId.ViewId == 0)
    m_baseViewQueue.push(pOutputBuffer);
  else if (pOutputBuffer->surface.Info.FrameId.ViewId > 0)
    m_extViewQueue.push(pOutputBuffer);

  // process output if queue is full
  while (m_baseViewQueue.size() >= ASYNC_DEPTH >> 1 
      && m_extViewQueue.size()  >= ASYNC_DEPTH >> 1 )
      ProcessOutput();

  return 0;
}

void CDVDVideoCodecMFX::ProcessOutput()
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
    m_context->ReleaseBuffer(pBaseView);
    m_baseViewQueue.pop();
  }
  else if (pBaseView->surface.Data.FrameOrder > pExtraView->surface.Data.FrameOrder)
  {
    m_context->ReleaseBuffer(pExtraView);
    m_extViewQueue.pop();
  }
}

#define RINT(x) ((x) >= 0 ? ((int)((x) + 0.5)) : ((int)((x) - 0.5)))

bool CDVDVideoCodecMFX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  mfxStatus sts = MFX_ERR_NONE;

  if (!m_renderQueue.empty())
  {
    bool useSysMem = m_mfxVideoParams.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    CMVCPicture* pRenderPicture = m_renderQueue.front();
    MVCBuffer* pBaseView = pRenderPicture->baseView, *pExtraView = pRenderPicture->extraView;
    mfxHDL pthis = m_frameAllocator->pthis;

    if (useSysMem)
    {
      // get sysmem pointers
      m_frameAllocator->Lock(pthis, pBaseView->surface.Data.MemId, &pBaseView->surface.Data);
      m_frameAllocator->Lock(pthis, pExtraView->surface.Data.MemId, &pExtraView->surface.Data);
    }
    else 
    {
      // get HW references
      m_frameAllocator->GetHDL(pthis, pBaseView->surface.Data.MemId, reinterpret_cast<mfxHDL*>(&pRenderPicture->baseHNDL));
      m_frameAllocator->GetHDL(pthis, pExtraView->surface.Data.MemId, reinterpret_cast<mfxHDL*>(&pRenderPicture->extHNDL));
    }

    DVDVideoPicture* pFrame = pDvdVideoPicture;
    pFrame->iWidth = pBaseView->surface.Info.Width;
    pFrame->iHeight = pBaseView->surface.Info.Height;
    pFrame->format = RENDER_FMT_MSDK_MVC;
    pFrame->extended_format = !useSysMem ? RENDER_FMT_DXVA : RENDER_FMT_NONE;

    double aspect_ratio;
    if (pBaseView->surface.Info.AspectRatioH == 0)
      aspect_ratio = 0;
    else
      aspect_ratio = pBaseView->surface.Info.AspectRatioH / (double)pBaseView->surface.Info.AspectRatioW
      * pBaseView->surface.Info.CropW / pBaseView->surface.Info.CropH;

    if (aspect_ratio <= 0.0)
      aspect_ratio = (float)pBaseView->surface.Info.CropW / (float)pBaseView->surface.Info.CropH;

    pFrame->iDisplayHeight = pBaseView->surface.Info.CropH;
    pFrame->iDisplayWidth = ((int)RINT(pBaseView->surface.Info.CropH * aspect_ratio)) & -3;
    if (pFrame->iDisplayWidth > pFrame->iWidth)
    {
      pFrame->iDisplayWidth = pFrame->iWidth;
      pFrame->iDisplayHeight = ((int)RINT(pFrame->iWidth / aspect_ratio)) & -3;
    }
    strncpy(pFrame->stereo_mode, m_stereoMode.c_str(), sizeof(pFrame->stereo_mode));
    pFrame->stereo_mode[sizeof(pFrame->stereo_mode) - 1] = '\0';
    pFrame->color_range = 0;
    pFrame->iFlags = DVP_FLAG_ALLOCATED | m_codecControlFlags;
    pFrame->dts = DVD_NOPTS_VALUE;
    if (!(pBaseView->surface.Data.DataFlag & MFX_FRAMEDATA_ORIGINAL_TIMESTAMP))
      pBaseView->surface.Data.TimeStamp = MFX_TIMESTAMP_UNKNOWN;
    if (pBaseView->surface.Data.TimeStamp != MFX_TIMESTAMP_UNKNOWN) 
      pFrame->pts = (double)pBaseView->surface.Data.TimeStamp;
    else 
      pFrame->pts = DVD_NOPTS_VALUE;
    pFrame->mvc = pRenderPicture;

    m_renderQueue.pop();
    return true;
  }
  return false;
}

bool CDVDVideoCodecMFX::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (pDvdVideoPicture->mvc)
  {
    MVCBuffer* pBaseView = pDvdVideoPicture->mvc->baseView, *pExtraView = pDvdVideoPicture->mvc->extraView;

    if (pBaseView->surface.Data.Y || pExtraView->surface.Data.Y)
    {
      m_frameAllocator->Unlock(m_frameAllocator->pthis, pBaseView->surface.Data.MemId, &pBaseView->surface.Data);
      m_frameAllocator->Unlock(m_frameAllocator->pthis, pExtraView->surface.Data.MemId, &pExtraView->surface.Data);
    }

    SAFE_RELEASE(pDvdVideoPicture->mvc);
  }
  return CDVDVideoCodec::ClearPicture(pDvdVideoPicture);
}

void CDVDVideoCodecMFX::SyncOutput(MVCBuffer * pBaseView, MVCBuffer * pExtraView)
{
  mfxStatus sts = MFX_ERR_NONE;

  assert(pBaseView->surface.Info.FrameId.ViewId == 0 && pExtraView->surface.Info.FrameId.ViewId > 0);
  assert(pBaseView->surface.Data.FrameOrder == pExtraView->surface.Data.FrameOrder);

  // sync base view
  do 
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pBaseView->sync, 1000);
  }
  while (sts == MFX_WRN_IN_EXECUTION);
  pBaseView->sync = nullptr;

  // sync extra view
  do 
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pExtraView->sync, 1000);
  } 
  while (sts == MFX_WRN_IN_EXECUTION);
  pExtraView->sync = nullptr;

  m_renderQueue.push(m_context->GetPicture(pBaseView, pExtraView));
}

bool CDVDVideoCodecMFX::Flush()
{
  m_buffSize = 0;

  if (m_mfxSession) 
  {
    if (m_bDecodeReady)
      MFXVideoDECODE_Reset(m_mfxSession, &m_mfxVideoParams);

    while (!m_renderQueue.empty())
    {
      SAFE_RELEASE(m_renderQueue.front());
      m_renderQueue.pop();
    }
    while (!m_baseViewQueue.empty())
    {
      m_context->ReleaseBuffer(m_baseViewQueue.front());
      m_baseViewQueue.pop();
    }
    while (!m_extViewQueue.empty())
    {
      m_context->ReleaseBuffer(m_extViewQueue.front());
      m_extViewQueue.pop();
    }
  }

  return true;
}

bool CDVDVideoCodecMFX::FlushQueue()
{
  if (!m_bDecodeReady)
    return false;

  // Process all remaining frames in the queue
  while(!m_baseViewQueue.empty() 
     && !m_extViewQueue.empty()) 
     ProcessOutput();

  return true;
}

void CDVDVideoCodecMFX::SetStereoMode(CDVDStreamInfo &hints)
{
  if (hints.stereo_mode != "mvc_lr" && hints.stereo_mode != "mvc_rl")
    hints.stereo_mode = "mvc_lr";
  m_stereoMode = hints.stereo_mode;
}

void CH264Nalu::SetBuffer(const uint8_t* pBuffer, size_t nSize, int nNALSize)
{
  m_pBuffer = pBuffer;
  m_nSize = nSize;
  m_nNALSize = nNALSize;
  m_nCurPos = 0;
  m_nNextRTP = 0;

  m_nNALStartPos = 0;
  m_nNALDataPos = 0;

  // In AnnexB, the buffer is not guaranteed to start on a NAL boundary
  if (nNALSize == 0 && nSize > 0)
    MoveToNextAnnexBStartcode();
}

bool CH264Nalu::MoveToNextAnnexBStartcode()
{
  if (m_nSize < 4)
  {
    m_nCurPos = m_nSize;
    return false;
  }

  size_t nBuffEnd = m_nSize - 4;

  for (size_t i = m_nCurPos; i <= nBuffEnd; i++) 
  {
    if ((*((DWORD*)(m_pBuffer + i)) & 0x00FFFFFF) == 0x00010000) 
    {
      // Found next AnnexB NAL
      m_nCurPos = i;
      return true;
    }
  }

  m_nCurPos = m_nSize;
  return false;
}

bool CH264Nalu::MoveToNextRTPStartcode()
{
  if (m_nNextRTP < m_nSize) 
  {
    m_nCurPos = m_nNextRTP;
    return true;
  }

  m_nCurPos = m_nSize;
  return false;
}

bool CH264Nalu::ReadNext()
{
  if (m_nCurPos >= m_nSize) 
    return false;

  if ((m_nNALSize != 0) && (m_nCurPos == m_nNextRTP)) 
  {
    if (m_nCurPos + m_nNALSize >= m_nSize) 
      return false;
    // RTP Nalu type : (XX XX) XX XX NAL..., with XX XX XX XX or XX XX equal to NAL size
    m_nNALStartPos = m_nCurPos;
    m_nNALDataPos = m_nCurPos + m_nNALSize;

    // Read Length code from the buffer
    unsigned nTemp = 0;
    for (int i = 0; i < m_nNALSize; i++)
      nTemp = (nTemp << 8) + m_pBuffer[m_nCurPos++];

    m_nNextRTP += nTemp + m_nNALSize;
    MoveToNextRTPStartcode();
  }
  else 
  {
    // Remove trailing bits
    while (m_pBuffer[m_nCurPos] == 0x00 && ((*((DWORD*)(m_pBuffer + m_nCurPos)) & 0x00FFFFFF) != 0x00010000))
      m_nCurPos++;

    // AnnexB Nalu : 00 00 01 NAL...
    m_nNALStartPos = m_nCurPos;
    m_nCurPos += 3;
    m_nNALDataPos = m_nCurPos;
    MoveToNextAnnexBStartcode();
  }

  forbidden_bit = (m_pBuffer[m_nNALDataPos] >> 7) & 1;
  nal_reference_idc = (m_pBuffer[m_nNALDataPos] >> 5) & 3;
  nal_unit_type = (NALU_TYPE)(m_pBuffer[m_nNALDataPos] & 0x1f);

  return true;
}
