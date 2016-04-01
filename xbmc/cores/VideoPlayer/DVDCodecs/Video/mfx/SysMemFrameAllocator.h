#pragma once
/*
 *      Copyright (C) 2005-2016 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "BaseFrameAllocator.h"

extern "C" {
#include <libmfx/mfxvideo.h>
}

namespace MFX
{
struct sFrame
{
  mfxU32       nbytes;
  mfxU16       type;
  mfxFrameInfo info;
};

class SysMemFrameAllocator : public BaseFrameAllocator
{
public:
  SysMemFrameAllocator();
  virtual ~SysMemFrameAllocator();

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