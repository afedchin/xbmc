/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "H264MVCCombiner.h"
#include "DVDDemux.h"
#include "DVDDemuxUtils.h"

//#define DEBUG_VERBOSE
#define MVC_QUEUE_SIZE 100

#if defined(DEBUG_VERBOSE)
#include "utils/log.h"
#endif // DEBUG_VERBOSE

DemuxPacket* CH264MVCCombiner::AddData(DemuxPacket*& srcPkt)
{
  if (srcPkt->iStreamId != m_h264StreamId && 
      srcPkt->iStreamId != m_mvcStreamId)
    return srcPkt;

  if (srcPkt->iStreamId == m_h264StreamId)
  {
    if (m_extStream && !m_extStream->HasExtension())
      return srcPkt;
    m_H264queue.push(srcPkt);
  }
  else if (srcPkt->iStreamId == m_mvcStreamId)
  {
    m_MVCqueue.push(srcPkt);
  }

  return GetPacket();
}

void CH264MVCCombiner::Flush()
{
  while (!m_H264queue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_H264queue.front());
    m_H264queue.pop();
  }
  while (!m_MVCqueue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_MVCqueue.front());
    m_MVCqueue.pop();
  }
}

DemuxPacket* CH264MVCCombiner::MergePacket(DemuxPacket*& srcPkt, DemuxPacket*& appendPkt) const
{
  DemuxPacket* newPkt = CDVDDemuxUtils::AllocateDemuxPacket(srcPkt->iSize + appendPkt->iSize);
  newPkt->iSize = srcPkt->iSize + appendPkt->iSize;

  newPkt->pts = srcPkt->pts;
  newPkt->dts = srcPkt->dts;
  newPkt->duration = srcPkt->duration;
  newPkt->iGroupId = srcPkt->iGroupId;
  newPkt->iStreamId = srcPkt->iStreamId;
  memcpy(newPkt->pData, srcPkt->pData, srcPkt->iSize);
  memcpy(newPkt->pData + srcPkt->iSize, appendPkt->pData, appendPkt->iSize);

  CDVDDemuxUtils::FreeDemuxPacket(srcPkt);
  srcPkt = nullptr;
  CDVDDemuxUtils::FreeDemuxPacket(appendPkt);
  appendPkt = nullptr;

  return newPkt;
}

DemuxPacket* CH264MVCCombiner::GetPacket()
{
  // fill up mvc queue before processing from extension input 
  if (m_extStream && m_MVCqueue.empty() && !m_H264queue.empty())
    FillMVCQueue(m_H264queue.front()->dts);

  // Here, we recreate a h264 MVC packet from the base one + buffered MVC NALU's
  while (!m_H264queue.empty() && !m_MVCqueue.empty())
  {
    DemuxPacket* h264pkt = m_H264queue.front();
    const double tsH264 = h264pkt->dts != DVD_NOPTS_VALUE ? h264pkt->dts : h264pkt->pts;
    DemuxPacket* mvcPkt = m_MVCqueue.front();
    const double tsMVC = mvcPkt->dts != DVD_NOPTS_VALUE ? mvcPkt->dts : mvcPkt->pts;

    if (tsH264 == tsMVC)
    {
      m_H264queue.pop();
      m_MVCqueue.pop();

      while (!m_H264queue.empty())
      {
        DemuxPacket* pkt = m_H264queue.front();
        const double ts = (pkt->dts != DVD_NOPTS_VALUE ? pkt->dts : pkt->pts);
        if (ts == DVD_NOPTS_VALUE)
        {
#if defined(DEBUG_VERBOSE)
          CLog::Log(LOGDEBUG, ">>> MVC merge h264 fragment: %6d+%6d, pts(%.3f/%.3f) dts(%.3f/%.3f)", h264pkt->iSize, pkt->iSize, h264pkt->pts*1e-6, pkt->pts*1e-6, h264pkt->dts*1e-6, pkt->dts*1e-6);
#endif
          h264pkt = MergePacket(h264pkt, pkt);
          m_H264queue.pop();
        }
        else
          break;
      }
      while (!m_MVCqueue.empty())
      {
        DemuxPacket* pkt = m_MVCqueue.front();
        const double ts = (pkt->dts != DVD_NOPTS_VALUE ? pkt->dts : pkt->pts);
        if (ts == DVD_NOPTS_VALUE)
        {
#if defined(DEBUG_VERBOSE)
          CLog::Log(LOGDEBUG, ">>> MVC merge mvc fragment: %6d+%6d, pts(%.3f/%.3f) dts(%.3f/%.3f)", mvcPkt->iSize, pkt->iSize, mvcPkt->pts*1e-6, pkt->pts*1e-6, mvcPkt->dts*1e-6, pkt->dts*1e-6);
#endif
          mvcPkt = MergePacket(mvcPkt, pkt);
          m_MVCqueue.pop();
        }
        else
          break;
      }

#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC merge packet: %6d+%6d, pts(%.3f/%.3f) dts(%.3f/%.3f)", h264pkt->iSize, mvcPkt->iSize, h264pkt->pts*1e-6, mvcPkt->pts*1e-6, h264pkt->dts*1e-6, mvcPkt->dts*1e-6);
#endif
      return MergePacket(h264pkt, mvcPkt);
    }

    if (tsH264 > tsMVC)
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC discard mvc: %6d, pts(%.3f) dts(%.3f)", mvcPkt->iSize, mvcPkt->pts*1e-6, mvcPkt->dts*1e-6);
#endif
      CDVDDemuxUtils::FreeDemuxPacket(mvcPkt);
      m_MVCqueue.pop();
    }
    else
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC discard h264: %6d, pts(%.3f) dts(%.3f)", h264pkt->iSize, h264pkt->pts*1e-6, h264pkt->dts*1e-6);
#endif
      CDVDDemuxUtils::FreeDemuxPacket(h264pkt);
      m_H264queue.pop();
    }
  }

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, ">>> MVC waiting data. MVC(%d) H264(%d)", m_MVCqueue.size(), m_H264queue.size());
#endif
  return CDVDDemuxUtils::AllocateDemuxPacket(0);
}

bool CH264MVCCombiner::FillMVCQueue(double dtsBase)
{
  if (!m_extStream)
    return false;

  DemuxPacket* mvc;
  while (m_MVCqueue.size() < MVC_QUEUE_SIZE && ((mvc = m_extStream->ReadDemux())))
  {
    if (dtsBase == DVD_NOPTS_VALUE || mvc->dts == DVD_NOPTS_VALUE)
    {
      // do nothing, can't compare timestamps when they are not set
    }
    else if (mvc->dts < dtsBase)
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC drop mvc: %6d, pts(%.3f) dts(%.3f)", mvc->iSize, mvc->pts*1e-6, mvc->dts*1e-6);
#endif
      CDVDDemuxUtils::FreeDemuxPacket(mvc);
      continue;
    }
    m_MVCqueue.push(mvc);
  }

  if (m_MVCqueue.size() != MVC_QUEUE_SIZE)
    m_extStream->NeedMoreData();

  return true;
}
