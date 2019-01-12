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

namespace MFX 
{

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

class CAnnexBConverter
{
public:
  CAnnexBConverter(void){};
  ~CAnnexBConverter(void){};

  void SetNALUSize(int nalusize)
  {
    m_NaluSize = nalusize;
  }
  bool Convert(uint8_t** poutbuf, int* poutbuf_size, const uint8_t* buf, int buf_size) const;

private:
  int m_NaluSize = 0;
};

class CH264Nalu
{
protected:
  int forbidden_bit = 0;                       //! should be always FALSE
  int nal_reference_idc = 0;                   //! NALU_PRIORITY_xxxx
  NALU_TYPE nal_unit_type = NALU_TYPE_UNKNOWN; //! NALU_TYPE_xxxx

  size_t m_nNALStartPos = 0; //! NALU start (including startcode / size)
  size_t m_nNALDataPos = 0;  //! Useful part

  const BYTE* m_pBuffer = nullptr;
  size_t m_nCurPos = 0;
  size_t m_nNextRTP = 0;
  size_t m_nSize = 0;
  int m_nNALSize = 0;

  bool MoveToNextAnnexBStartÑode();
  bool MoveToNextRTPStartÑode();

public:
  CH264Nalu()
  {
    SetBuffer(nullptr, 0, 0);
  }
  NALU_TYPE GetType() const
  {
    return nal_unit_type;
  }
  bool IsRefFrame() const
  {
    return (nal_reference_idc != 0);
  }

  size_t GetDataLength() const
  {
    return m_nCurPos - m_nNALDataPos;
  }
  const uint8_t* GetDataBuffer()
  {
    return m_pBuffer + m_nNALDataPos;
  }
  size_t GetRoundedDataLength() const
  {
    size_t nSize = m_nCurPos - m_nNALDataPos;
    return nSize + 128 - (nSize % 128);
  }

  size_t GetLength() const
  {
    return m_nCurPos - m_nNALStartPos;
  }
  const uint8_t* GetNALBuffer()
  {
    return m_pBuffer + m_nNALStartPos;
  }
  size_t GetNALPos()
  {
    return m_nNALStartPos;
  }
  bool IsEOF() const
  {
    return m_nCurPos >= m_nSize;
  }

  void SetBuffer(const uint8_t* pBuffer, size_t nSize, int nNALSize);
  bool ReadNext();
};

}; // namespace MFX
