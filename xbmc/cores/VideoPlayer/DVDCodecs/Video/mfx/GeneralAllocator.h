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

#include "BaseFrameallocator.h"

#include <memory>
#include <map>

namespace MFX
{
class SysMemFrameAllocator;

// Wrapper on standard allocator for concurrent allocation of HW and system surfaces
class GeneralAllocator : public BaseFrameAllocator
{
public:
  GeneralAllocator();
  virtual ~GeneralAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override;
  mfxStatus Close() override;

protected:
  mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) override;
  mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) override;

  mfxStatus ReleaseResponse(mfxFrameAllocResponse *response) override;
  mfxStatus AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;

  void    StoreFrameMids(bool isD3DFrames, mfxFrameAllocResponse *response);
  bool    isHWMid(mfxHDL mid);

  std::map<mfxHDL, bool>               m_Mids;
  std::auto_ptr<BaseFrameAllocator>    m_HWAllocator;
  std::auto_ptr<SysMemFrameAllocator>  m_SYSAllocator;
private:
  GeneralAllocator(const GeneralAllocator&);
  void operator=(const GeneralAllocator&);
};
}; // namespace MFX