/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "DVDInputStreams/DVDInputStream.h"
#include <queue>

struct DemuxPacket;

class CH264MVCCombiner final
{
public:
  CH264MVCCombiner() = default;
  ~CH264MVCCombiner() { Flush(); }

  DemuxPacket* AddData(DemuxPacket*& srcPkt);
  void Flush();

  void SetH264StreamId(int id) { m_h264StreamId = id; };
  void SetMVCStreamId(int id) { m_mvcStreamId = id; };
  int GetH264StreamId() const { return m_h264StreamId; };
  int GetMVCStreamId() const { return m_mvcStreamId; };

  void SetExtensionInput(const std::shared_ptr<CDVDInputStream::IExtensionStream>& stream) { m_extStream = stream; };
  bool HasExtensionInput() const { return m_extStream != nullptr; };

private:
  DemuxPacket* GetPacket();
  DemuxPacket* MergePacket(DemuxPacket* &srcPkt, DemuxPacket* &appendPkt) const;
  bool FillMVCQueue(double dtsBase);

  std::shared_ptr<CDVDInputStream::IExtensionStream> m_extStream = nullptr;
  std::queue<DemuxPacket*> m_H264queue;
  std::queue<DemuxPacket*> m_MVCqueue;
  int m_h264StreamId = -1;
  int m_mvcStreamId = -1;
};
