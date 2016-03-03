/*
*      Copyright (C) 2010-2016 Hendrik Leppkes
*      http://www.1f0.de
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
#include "../DVDCodecUtils.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "utils/Log.h"

extern "C" {
#include "libavutil/intreadwrite.h"
}

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

CDVDVideoCodecMFX::CDVDVideoCodecMFX(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo)
{
  m_mfxSession = nullptr;
  memset(m_pOutputQueue, 0, sizeof(m_pOutputQueue));
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
  MFXQueryIMPL(m_mfxSession, &impl);
  CLog::Log(LOGNOTICE, "%s: MSDK Initialized, version %d.%d", __FUNCTION__, m_mfxVersion.Major, m_mfxVersion.Minor);
  if ((impl & 0x0F00) == MFX_IMPL_VIA_D3D11)
    CLog::Log(LOGDEBUG, "%s: MSDK uses D3D11 API.", __FUNCTION__);
  if ((impl & 0x0F00) == MFX_IMPL_VIA_D3D9)
    CLog::Log(LOGDEBUG, "%s: MSDK uses D3D9 API.", __FUNCTION__);
  if ((impl & 0x0F) == MFX_IMPL_SOFTWARE)
    CLog::Log(LOGDEBUG, "%s: MSDK uses Pure Software Implementation.", __FUNCTION__);
  if ((impl & 0x0F) == MFX_IMPL_HARDWARE)
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

  {
    CSingleLock lock(m_BufferCritSec);
    for (int i = 0; i < ASYNC_DEPTH; i++)
      if (m_pOutputQueue[i])
        ReleaseBuffer(&m_pOutputQueue[i]->surface);

    memset(m_pOutputQueue, 0, sizeof(m_pOutputQueue));

    for (auto it = m_BufferQueue.begin(); it != m_BufferQueue.end(); it++) 
    {
      if (!(*it)->queued) 
      {
        av_freep(&(*it)->surface.Data.Y);
        delete (*it);
      }
    }
    m_BufferQueue.clear();
  }

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

  memset(&m_mfxExtMVCSeq, 0, sizeof(m_mfxExtMVCSeq));
  m_mfxExtMVCSeq.Header.BufferId = MFX_EXTBUFF_MVC_SEQ_DESC;
  m_mfxExtMVCSeq.Header.BufferSz = sizeof(m_mfxExtMVCSeq);
  m_mfxExtParam[0] = (mfxExtBuffer *)&m_mfxExtMVCSeq;

  // Attach ext params to VideoParams
  m_mfxVideoParams.ExtParam = m_mfxExtParam;
  m_mfxVideoParams.NumExtParam = 1;

  uint8_t* extradata;
  int extradata_size;

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

MVCBuffer * CDVDVideoCodecMFX::GetBuffer()
{
  CSingleLock lock(m_BufferCritSec);
  MVCBuffer *pBuffer = nullptr;
  for (auto it = m_BufferQueue.begin(); it != m_BufferQueue.end(); it++) 
  {
    if (!(*it)->surface.Data.Locked && !(*it)->queued) 
    {
      pBuffer = *it;
      break;
    }
  }

  if (!pBuffer) 
  {
    pBuffer = new MVCBuffer();

    pBuffer->surface.Info = m_mfxVideoParams.mfx.FrameInfo;
    pBuffer->surface.Info.FourCC = MFX_FOURCC_NV12;

    pBuffer->surface.Data.PitchLow = FFALIGN(m_mfxVideoParams.mfx.FrameInfo.Width, 64);
    pBuffer->surface.Data.Y = (mfxU8 *)av_malloc(pBuffer->surface.Data.PitchLow * FFALIGN(m_mfxVideoParams.mfx.FrameInfo.Height, 64) * 3 / 2);
    pBuffer->surface.Data.UV = pBuffer->surface.Data.Y + (pBuffer->surface.Data.PitchLow * FFALIGN(m_mfxVideoParams.mfx.FrameInfo.Height, 64));

    m_BufferQueue.push_back(pBuffer);
    CLog::Log(LOGDEBUG, "Allocated new MSDK MVC buffer (%d total)", m_BufferQueue.size());
  }

  return pBuffer;
}

MVCBuffer * CDVDVideoCodecMFX::FindBuffer(mfxFrameSurface1 * pSurface)
{
  CSingleLock lock(m_BufferCritSec);
  bool bFound = false;
  for (auto it = m_BufferQueue.begin(); it != m_BufferQueue.end(); it++) 
    if (&(*it)->surface == pSurface)
      return *it;

  return nullptr;
}

void CDVDVideoCodecMFX::ReleaseBuffer(mfxFrameSurface1 * pSurface)
{
  if (!pSurface)
    return;

  CSingleLock lock(m_BufferCritSec);
  MVCBuffer * pBuffer = FindBuffer(pSurface);

  if (pBuffer) 
  {
    pBuffer->queued = 0;
    pBuffer->sync = nullptr;
  }
}

int CDVDVideoCodecMFX::Decode(uint8_t* buffer, int buflen, double dts, double pts)
{
  if (!m_mfxSession)
    return VC_ERROR;

  mfxStatus sts = MFX_ERR_NONE;
  mfxBitstream bs = { 0 };
  bool bBuffered = false, bFlush = (buffer == nullptr);
  int result = 0; 

  double ts = pts != DVD_NOPTS_VALUE ? pts : dts;
  if (ts >= 0 && ts != DVD_NOPTS_VALUE)
    bs.TimeStamp = static_cast<mfxU64>(round(ts));
  else
    bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN;
  bs.DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN;

  if (!bFlush) 
  {
    if (m_pAnnexBConverter) 
    {
      BYTE *pOutBuffer = nullptr;
      int pOutSize = 0;
      if (!m_pAnnexBConverter->Convert(&pOutBuffer, &pOutSize, buffer, buflen))
        return VC_ERROR;

      m_buff.reserve(m_buff.size() + pOutSize);
      std::copy(pOutBuffer, pOutBuffer + pOutSize, std::back_inserter(m_buff));
      av_freep(&pOutBuffer);
    }
    else
    {
      m_buff.reserve(m_buff.size() + buflen);
      std::copy(buffer, buffer + buflen, std::back_inserter(m_buff));
    }

    bs.Data = m_buff.data();
    bs.DataLength = m_buff.size();
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
      m_buff.clear();
      return VC_BUFFER;
    }
    if (sts == MFX_ERR_NONE) 
    {
      m_mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
      m_mfxVideoParams.AsyncDepth = ASYNC_DEPTH - 2;

      sts = MFXVideoDECODE_Init(m_mfxSession, &m_mfxVideoParams);
      if (sts != MFX_ERR_NONE) 
      {
        CLog::Log(LOGERROR, "%s: Error initializing the MSDK decoder (%d)", __FUNCTION__, sts);
        return VC_ERROR;
      }

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

  // Loop over the decoder to ensure all data is being consumed
  while (1) 
  {
    MVCBuffer *pInputBuffer = GetBuffer();
    mfxFrameSurface1 *outsurf = nullptr;
    sts = MFXVideoDECODE_DecodeFrameAsync(m_mfxSession, bFlush ? nullptr : &bs, &pInputBuffer->surface, &outsurf, &sync);

    if (sts == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) 
    {
      m_buff.clear();
      bFlush = true;
      m_bDecodeReady = false;
      continue;
    }

    if (sync) 
    {
      MVCBuffer * pOutputBuffer = FindBuffer(outsurf);
      pOutputBuffer->queued = 1;
      pOutputBuffer->sync = sync;
      result |= HandleOutput(pOutputBuffer);
      continue;
    }

    if (sts != MFX_ERR_MORE_SURFACE && sts < 0)
      break;
  }

  if (!bs.DataOffset && !sync && !bFlush) 
  {
    CLog::Log(LOGERROR, "%s: Decoder did not consume any data, discarding", __FUNCTION__);
    bs.DataOffset = m_buff.size();
  }

  if (bs.DataOffset < m_buff.size()) 
  {
    BYTE *p = m_buff.data();
    memmove(p, p + bs.DataOffset, m_buff.size() - bs.DataOffset);
    m_buff.resize(m_buff.size() - bs.DataOffset);
  }
  else 
  {
    m_buff.clear();
  }

  if (sts != MFX_ERR_MORE_DATA && sts < 0)
  {
    CLog::Log(LOGERROR, "%s: Error from Decode call (%d)", __FUNCTION__, sts);
    result = VC_ERROR;
  }
  else if (sts == MFX_ERR_MORE_DATA)
    result |= VC_BUFFER;

  return result;
}

int CDVDVideoCodecMFX::HandleOutput(MVCBuffer * pOutputBuffer)
{
  int result = VC_BUFFER;
  int nCur = m_nOutputQueuePosition, nNext = (m_nOutputQueuePosition + 1) % ASYNC_DEPTH;

  if (m_pOutputQueue[nCur] && m_pOutputQueue[nNext]) 
  {
    SyncOutput(m_pOutputQueue[nCur], m_pOutputQueue[nNext]);
    m_pOutputQueue[nCur] = nullptr;
    m_pOutputQueue[nNext] = nullptr;
    result |= VC_PICTURE;
  }
  else if (m_pOutputQueue[nCur]) 
  {
    CLog::Log(LOGDEBUG, "%s: Dropping unpaired frame", __FUNCTION__);

    ReleaseBuffer(&m_pOutputQueue[nCur]->surface);
    m_pOutputQueue[nCur]->sync = nullptr;
    m_pOutputQueue[nCur] = nullptr;
  }

  m_pOutputQueue[nCur] = pOutputBuffer;
  m_nOutputQueuePosition = nNext;

  return result;
}

#define RINT(x) ((x) >= 0 ? ((int)((x) + 0.5)) : ((int)((x) - 0.5)))

bool CDVDVideoCodecMFX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  mfxStatus sts = MFX_ERR_NONE;

  if (m_pRenderPicture)
  {
    MVCBuffer* pBaseView = m_pRenderPicture->baseView, *pExtraView = m_pRenderPicture->extraView;

    DVDVideoPicture* pFrame = pDvdVideoPicture;
    pFrame->iWidth = pBaseView->surface.Info.Width;
    pFrame->iHeight = pBaseView->surface.Info.Height;
    pFrame->format = RENDER_FMT_MSDK_MVC;

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
    pFrame->iFlags = DVP_FLAG_ALLOCATED;
    pFrame->dts = DVD_NOPTS_VALUE;
    if (!(pBaseView->surface.Data.DataFlag & MFX_FRAMEDATA_ORIGINAL_TIMESTAMP))
      pBaseView->surface.Data.TimeStamp = MFX_TIMESTAMP_UNKNOWN;
    if (pBaseView->surface.Data.TimeStamp != MFX_TIMESTAMP_UNKNOWN) 
      pFrame->pts = static_cast<double>(pBaseView->surface.Data.TimeStamp);
    else 
      pFrame->pts = DVD_NOPTS_VALUE;
    pFrame->mvc = m_pRenderPicture;

    return true;
  }
  return false;
}

void CDVDVideoCodecMFX::ReleasePicture(CMVCPicture* pMVCPicture)
{
  CSingleLock lock(m_BufferCritSec);

  MVCBuffer * pBaseBuffer = pMVCPicture->baseView;
  MVCBuffer * pStoredBuffer = FindBuffer(&pBaseBuffer->surface);
  if (pStoredBuffer)
  {
    ReleaseBuffer(&pBaseBuffer->surface);
  }
  else
  {
    av_free(pBaseBuffer->surface.Data.Y);
    SAFE_DELETE(pBaseBuffer);
  }

  MVCBuffer * pExtraBuffer = pMVCPicture->extraView;
  pStoredBuffer = FindBuffer(&pExtraBuffer->surface);
  if (pStoredBuffer)
  {
    ReleaseBuffer(&pExtraBuffer->surface);
  }
  else
  {
    av_free(pExtraBuffer->surface.Data.Y);
    SAFE_DELETE(pExtraBuffer);
  }
}

void free_buffers(CMVCPicture* pPicture)
{
  if (pPicture->dec)
    pPicture->dec->ReleasePicture(pPicture);
}

void CDVDVideoCodecMFX::SyncOutput(MVCBuffer * pBaseView, MVCBuffer * pExtraView)
{
  mfxStatus sts = MFX_ERR_NONE;

  assert(pBaseView->surface.Info.FrameId.ViewId == 0 && pExtraView->surface.Info.FrameId.ViewId > 0);
  assert(pBaseView->surface.Data.FrameOrder == pExtraView->surface.Data.FrameOrder);

  // Sync base view
  do 
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pBaseView->sync, 1000);
  } 
  while (sts == MFX_WRN_IN_EXECUTION);
  pBaseView->sync = nullptr;

  // Sync extra view
  do 
  {
    sts = MFXVideoCORE_SyncOperation(m_mfxSession, pExtraView->sync, 1000);
  } 
  while (sts == MFX_WRN_IN_EXECUTION);
  pExtraView->sync = nullptr;

  SAFE_RELEASE(m_pRenderPicture);
  m_pRenderPicture = new CMVCPicture(pBaseView, pExtraView);
  m_pRenderPicture->dec = this;
  m_pRenderPicture->free_buffers = free_buffers;
}

bool CDVDVideoCodecMFX::Flush()
{
  m_buff.clear();

  if (m_mfxSession) 
  {
    if (m_bDecodeReady)
      MFXVideoDECODE_Reset(m_mfxSession, &m_mfxVideoParams);
    // TODO: decode sequence data
    for (int i = 0; i < ASYNC_DEPTH; i++) 
      ReleaseBuffer(&m_pOutputQueue[i]->surface);

    memset(m_pOutputQueue, 0, sizeof(m_pOutputQueue));
    m_nOutputQueuePosition = 0;
  }

  return true;
}

bool CDVDVideoCodecMFX::EndOfStream()
{
  if (!m_bDecodeReady)
    return false;

  // Flush frames out of the decoder
  Decode(nullptr, 0, 0, 0);

  // Process all remaining frames in the queue
  for (int i = 0; i < ASYNC_DEPTH; i++) 
  {
    int nCur = (m_nOutputQueuePosition + i) % ASYNC_DEPTH, nNext = (m_nOutputQueuePosition + i + 1) % ASYNC_DEPTH;
    if (m_pOutputQueue[nCur] && m_pOutputQueue[nNext]) 
    {
      SyncOutput(m_pOutputQueue[nCur], m_pOutputQueue[nNext]);
      m_pOutputQueue[nCur] = nullptr;
      m_pOutputQueue[nNext] = nullptr;
      i++;
    }
    else if (m_pOutputQueue[nCur]) 
    {
      CLog::Log(LOGDEBUG, "%s: Dropping unpaired frame", __FUNCTION__);

      ReleaseBuffer(&m_pOutputQueue[nCur]->surface);
      m_pOutputQueue[nCur] = nullptr;
    }
  }
  m_nOutputQueuePosition = 0;

  return true;
}

void CDVDVideoCodecMFX::SetStereoMode(CDVDStreamInfo &hints)
{
  if (hints.stereo_mode.empty() || hints.stereo_mode.compare("mono") == 0)
    hints.stereo_mode = "mvc_lr";
  m_stereoMode = hints.stereo_mode;
}