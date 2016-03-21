/*
 *      Copyright (C) 2005-2016 Team Kodi
 *      http://kodi.tv
 *
 *   This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "GeneralAllocator.h"
#include "SysMemFrameAllocator.h"
#include "D3D11FrameAllocator.h"

namespace MFX
{
// Wrapper on standard allocator for concurrent allocation of HW and system surfaces
GeneralAllocator::GeneralAllocator()
{
};

GeneralAllocator::~GeneralAllocator()
{
};

mfxStatus GeneralAllocator::Init(mfxAllocatorParams *pParams)
{
  mfxStatus sts = MFX_ERR_NONE;

#ifdef TARGET_WINDOWS
  D3D11AllocatorParams *d3d11AllocParams = dynamic_cast<D3D11AllocatorParams*>(pParams);
  if (d3d11AllocParams)
    m_HWAllocator.reset(new D3D11FrameAllocator);
#elif
  // TODO linux implementation
#endif

  if (m_HWAllocator.get())
  {
    sts = m_HWAllocator.get()->Init(pParams);
    if (sts != MFX_ERR_NONE)
      return sts;
  }

  m_SYSAllocator.reset(new SysMemFrameAllocator);
  sts = m_SYSAllocator.get()->Init(0);

  return sts;
}
mfxStatus GeneralAllocator::Close()
{
  mfxStatus sts = MFX_ERR_NONE;
  if (m_HWAllocator.get())
  {
    sts = m_HWAllocator.get()->Close();
    if (sts != MFX_ERR_NONE)
      return sts;
  }

  sts = m_SYSAllocator.get()->Close();
  return sts;
}

mfxStatus GeneralAllocator::LockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator.get()->Lock(m_HWAllocator.get(), mid, ptr);
  else
    return m_SYSAllocator.get()->Lock(m_SYSAllocator.get(), mid, ptr);
}
mfxStatus GeneralAllocator::UnlockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator.get()->Unlock(m_HWAllocator.get(), mid, ptr);
  else
    return m_SYSAllocator.get()->Unlock(m_SYSAllocator.get(), mid, ptr);
}

mfxStatus GeneralAllocator::GetFrameHDL(mfxMemId mid, mfxHDL *handle)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator.get()->GetHDL(m_HWAllocator.get(), mid, handle);
  else
    return m_SYSAllocator.get()->GetHDL(m_SYSAllocator.get(), mid, handle);
}

mfxStatus GeneralAllocator::ReleaseResponse(mfxFrameAllocResponse *response)
{
  // try to ReleaseResponse via D3D allocator
  if (isHWMid(response->mids[0]) && m_HWAllocator.get())
    return m_HWAllocator.get()->Free(m_HWAllocator.get(), response);
  else
    return m_SYSAllocator.get()->Free(m_SYSAllocator.get(), response);
}
mfxStatus GeneralAllocator::AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  mfxStatus sts;
  if ((request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET || request->Type & MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET) && m_HWAllocator.get())
  {
    sts = m_HWAllocator.get()->Alloc(m_HWAllocator.get(), request, response);
    if (sts != MFX_ERR_NONE)
      return sts;
    StoreFrameMids(true, response);
  }
  else
  {
    sts = m_SYSAllocator.get()->Alloc(m_SYSAllocator.get(), request, response);
    if (sts != MFX_ERR_NONE)
      return sts;
    StoreFrameMids(false, response);
  }
  return sts;
}

void GeneralAllocator::StoreFrameMids(bool isD3DFrames, mfxFrameAllocResponse *response)
{
  for (mfxU32 i = 0; i < response->NumFrameActual; i++)
    m_Mids.insert(std::pair<mfxHDL, bool>(response->mids[i], isD3DFrames));
}

bool GeneralAllocator::isHWMid(mfxHDL mid)
{
  std::map<mfxHDL, bool>::iterator it;
  it = m_Mids.find(mid);
  if (it == m_Mids.end())
    return false; // sys mem allocator will check validity of mid further
  else
    return it->second;
}
} // namespace MFX