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

#include "MfxDecoderUtils.h"

extern "C"
{
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
}

namespace MFX
{

//-----------------------------------------------------------------------------
// static methods
//-----------------------------------------------------------------------------
static bool alloc_and_copy(uint8_t** poutbuf, int* poutbuf_size, const uint8_t* in, uint32_t in_size)
{
  const uint32_t offset = *poutbuf_size;
  const uint8_t nal_header_size = offset ? 3 : 4;
  void* tmp;

  *poutbuf_size += in_size + nal_header_size;
  tmp = av_realloc(*poutbuf, *poutbuf_size);
  if (!tmp)
    return false;
  *poutbuf = static_cast<uint8_t*>(tmp);
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

//-----------------------------------------------------------------------------
// AnnexB Converter
//-----------------------------------------------------------------------------
bool CAnnexBConverter::Convert(uint8_t** poutbuf, int* poutbuf_size, const uint8_t* buf, int buf_size) const
{
  int32_t nal_size;
  const uint8_t* buf_end = buf + buf_size;

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
  } while (buf_size > 0);

  return true;
fail:
  av_freep(poutbuf);
  return false;
}

//-----------------------------------------------------------------------------
// CH264Nalu parser
//-----------------------------------------------------------------------------
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
    MoveToNextAnnexBStart—ode();
}

bool CH264Nalu::MoveToNextAnnexBStart—ode()
{
  if (m_nSize < 4)
  {
    m_nCurPos = m_nSize;
    return false;
  }

  const size_t nBuffEnd = m_nSize - 4;

  for (size_t i = m_nCurPos; i <= nBuffEnd; i++)
  {
    if ((*(unsigned long*)(m_pBuffer + i) & 0x00FFFFFF) == 0x00010000)
    {
      // Found next AnnexB NAL
      m_nCurPos = i;
      return true;
    }
  }

  m_nCurPos = m_nSize;
  return false;
}

bool CH264Nalu::MoveToNextRTPStart—ode()
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
    MoveToNextRTPStart—ode();
  }
  else
  {
    // Remove trailing bits
    while (m_pBuffer[m_nCurPos] == 0x00 &&
          (*(unsigned long*)(m_pBuffer + m_nCurPos) & 0x00FFFFFF) != 0x00010000)
      m_nCurPos++;

    // AnnexB Nalu : 00 00 01 NAL...
    m_nNALStartPos = m_nCurPos;
    m_nCurPos += 3;
    m_nNALDataPos = m_nCurPos;
    MoveToNextAnnexBStart—ode();
  }

  forbidden_bit = (m_pBuffer[m_nNALDataPos] >> 7) & 1;
  nal_reference_idc = (m_pBuffer[m_nNALDataPos] >> 5) & 3;
  nal_unit_type = static_cast<NALU_TYPE>(m_pBuffer[m_nNALDataPos] & 0x1f);

  return true;
}

} // namespace MFX
