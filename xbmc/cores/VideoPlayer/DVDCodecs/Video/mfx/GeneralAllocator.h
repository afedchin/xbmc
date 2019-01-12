/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "BaseFrameAllocator.h"

#include <memory>
#include <map>

namespace MFX
{
class CSysMemFrameAllocator;

// Wrapper on standard allocator for concurrent allocation of HW and system surfaces
class ÑGeneralAllocator : public CBaseFrameAllocator
{
public:
  ÑGeneralAllocator();
  virtual ~ÑGeneralAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override;
  mfxStatus Close() override;

  ÑGeneralAllocator(const ÑGeneralAllocator&) = delete;
  void operator=(const ÑGeneralAllocator&) = delete;

protected:
  mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) override;

  mfxStatus ReleaseResponse(mfxFrameAllocResponse *response) override;
  mfxStatus AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;

  void    StoreFrameMids(bool isD3DFrames, mfxFrameAllocResponse *response);
  bool    isHWMid(mfxHDL mid);

  std::map<mfxHDL, bool> m_Mids;
  std::auto_ptr<CBaseFrameAllocator> m_HWAllocator;
  std::auto_ptr<CSysMemFrameAllocator> m_SYSAllocator;
};
}; // namespace MFX