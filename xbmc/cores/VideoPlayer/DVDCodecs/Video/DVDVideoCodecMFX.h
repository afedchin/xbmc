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

#pragma once
#include "DVDVideoCodec.h"
#include "DVDResource.h"
#include "threads/Event.h"
#include <vector>
#include <queue>

extern "C" {
#include <libmfx/mfxvideo.h>
#include <libmfx/mfxmvc.h>
}

#define ASYNC_DEPTH 10

struct MVCBuffer 
{
  mfxFrameSurface1 surface;
  bool queued = false;
  mfxSyncPoint sync = nullptr;
  MVCBuffer()
  { 
    memset(&surface, 0, sizeof(surface)); 
  };
};

class CAnnexBConverter
{
public:
  CAnnexBConverter(void) {};
  ~CAnnexBConverter(void) {};

  void SetNALUSize(int nalusize) { m_NaluSize = nalusize; }
  bool Convert(uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size);

private:
  int m_NaluSize = 0;
};

class CDVDVideoCodecMFX;

class CMVCPicture : 
  public IDVDResourceCounted<CMVCPicture>
{
public:
  CMVCPicture(MVCBuffer *pBaseView, MVCBuffer* pExtraBuffer)
    : baseView(pBaseView), extraView(pExtraBuffer)
  {};
  ~CMVCPicture() {}
  MVCBuffer *baseView = nullptr;
  MVCBuffer* extraView = nullptr;
};

typedef enum
{
  NALU_TYPE_UNKNOWN = 0,
  NALU_TYPE_SLICE = 1,
  NALU_TYPE_DPA = 2,
  NALU_TYPE_DPB = 3,
  NALU_TYPE_DPC = 4,
  NALU_TYPE_IDR = 5,
  NALU_TYPE_SEI = 6,
  NALU_TYPE_SPS = 7,
  NALU_TYPE_PPS = 8,
  NALU_TYPE_AUD = 9,
  NALU_TYPE_EOSEQ = 10,
  NALU_TYPE_EOSTREAM = 11,
  NALU_TYPE_FILL = 12
} NALU_TYPE;


class CH264Nalu
{
protected:
  int        forbidden_bit = 0;                 //! should be always FALSE
  int        nal_reference_idc = 0;                 //! NALU_PRIORITY_xxxx
  NALU_TYPE  nal_unit_type = NALU_TYPE_UNKNOWN; //! NALU_TYPE_xxxx

  size_t     m_nNALStartPos = 0;                 //! NALU start (including startcode / size)
  size_t     m_nNALDataPos = 0;                 //! Useful part

  const BYTE *m_pBuffer = nullptr;
  size_t     m_nCurPos = 0;
  size_t     m_nNextRTP = 0;
  size_t     m_nSize = 0;
  int        m_nNALSize = 0;

  bool      MoveToNextAnnexBStartcode();
  bool      MoveToNextRTPStartcode();

public:
  CH264Nalu() { SetBuffer(nullptr, 0, 0); }
  NALU_TYPE GetType() const { return nal_unit_type; }
  bool      IsRefFrame() const { return (nal_reference_idc != 0); }

  size_t    GetDataLength() const { return m_nCurPos - m_nNALDataPos; }
  const uint8_t *GetDataBuffer() { return m_pBuffer + m_nNALDataPos; }
  size_t    GetRoundedDataLength() const
  {
    size_t nSize = m_nCurPos - m_nNALDataPos;
    return nSize + 128 - (nSize % 128);
  }

  size_t    GetLength() const { return m_nCurPos - m_nNALStartPos; }
  const uint8_t *GetNALBuffer() { return m_pBuffer + m_nNALStartPos; }
  size_t    GetNALPos() { return m_nNALStartPos; }
  bool      IsEOF() const { return m_nCurPos >= m_nSize; }

  void      SetBuffer(const uint8_t *pBuffer, size_t nSize, int nNALSize);
  bool      ReadNext();
};

class CDVDVideoCodecMFX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecMFX(CProcessInfo &processInfo);
  virtual ~CDVDVideoCodecMFX();

  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose() { DestroyDecoder(true); }
  virtual int Decode(uint8_t* pData, int iSize, double dts, double pts);
  virtual void Reset() { Flush(); };
  virtual bool GetPicture(DVDVideoPicture* pDvdVideoPicture);
  virtual void SetDropState(bool bDrop) {};
  virtual const char* GetName() { return "msdk-mvc"; };

  virtual bool ClearPicture(DVDVideoPicture* pDvdVideoPicture) override;
  void ReleasePicture(CMVCPicture* pMVCPicture);
  virtual void SetCodecControl(int flags) override { m_codecControlFlags = flags; }

private:
  bool Init();
  bool Flush();
  bool EndOfStream();
  void DestroyDecoder(bool bFull);
  bool AllocateMVCExtBuffers();
  void SetStereoMode(CDVDStreamInfo &hints);

  MVCBuffer * GetBuffer();
  MVCBuffer * FindBuffer(mfxFrameSurface1 * pSurface);
  void ReleaseBuffer(mfxFrameSurface1 * pSurface);

  int HandleOutput(MVCBuffer * pOutputBuffer);
  void SyncOutput(MVCBuffer * pBaseView, MVCBuffer * pExtraView);
  MVCBuffer * AllocateBuffer();

private:

  mfxSession m_mfxSession = nullptr;
  mfxVersion m_mfxVersion;

  bool                 m_bDecodeReady = false;
  mfxVideoParam        m_mfxVideoParams;

  mfxExtBuffer        *m_mfxExtParam[1];
  mfxExtMVCSeqDesc     m_mfxExtMVCSeq;

  CCriticalSection     m_BufferCritSec;
  std::vector<MVCBuffer*> m_BufferQueue;

  std::vector<BYTE>    m_buff;
  CAnnexBConverter    *m_pAnnexBConverter = nullptr;

  MVCBuffer           *m_pOutputQueue[ASYNC_DEPTH];
  int                  m_nOutputQueuePosition = 0;
  std::queue<CMVCPicture*> m_renderQueue;
  std::string          m_stereoMode;
  int                  m_codecControlFlags = 0;
};
