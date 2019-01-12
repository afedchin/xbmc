/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "BaseFrameAllocator.h"

extern "C" {
#include <mfxvideo.h>
}

namespace MFX
{
struct sFrame
{
  mfxU32 nbytes;
  mfxU16 type;
  mfxU32 FourCC;
  mfxU16 Width;
  mfxU16 Height;
};

class CSysMemFrameAllocator : public CBaseFrameAllocator
{
public:
  CSysMemFrameAllocator();
  virtual ~CSysMemFrameAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override;
  mfxStatus Close() override;
  mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) override;

protected:
  mfxStatus CheckRequestType(mfxFrameAllocRequest *request) override;
  mfxStatus ReleaseResponse(mfxFrameAllocResponse *response) override;
  mfxStatus AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;
};
}; // namespace MFX