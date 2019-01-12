/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GeneralAllocator.h"
#include "SysMemFrameAllocator.h"
#include "D3D11FrameAllocator.h"

namespace MFX
{
// Wrapper on standard allocator for concurrent allocation of HW and system surfaces
ÑGeneralAllocator::ÑGeneralAllocator() = default;

ÑGeneralAllocator::~ÑGeneralAllocator() = default;

mfxStatus ÑGeneralAllocator::Init(mfxAllocatorParams *pParams)
{
  mfxStatus sts;

#ifdef TARGET_WINDOWS
  auto* d3d11AllocParams = dynamic_cast<CD3D11AllocatorParams*>(pParams);
  if (d3d11AllocParams)
    m_HWAllocator.reset(new CD3D11FrameAllocator);
#elif
  // TODO linux implementation
#endif

  if (m_HWAllocator.get())
  {
    sts = m_HWAllocator->Init(pParams);
    if (sts != MFX_ERR_NONE)
      return sts;
  }

  m_SYSAllocator.reset(new CSysMemFrameAllocator);
  sts = m_SYSAllocator->Init(nullptr);

  return sts;
}
mfxStatus ÑGeneralAllocator::Close()
{
  mfxStatus sts;
  if (m_HWAllocator.get())
  {
    sts = m_HWAllocator->Close();
    if (sts != MFX_ERR_NONE)
      return sts;
  }

  sts = m_SYSAllocator->Close();
  return sts;
}

mfxStatus ÑGeneralAllocator::LockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator->Lock(m_HWAllocator.get(), mid, ptr);
  else
    return m_SYSAllocator->Lock(m_SYSAllocator.get(), mid, ptr);
}
mfxStatus ÑGeneralAllocator::UnlockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator->Unlock(m_HWAllocator.get(), mid, ptr);
  else
    return m_SYSAllocator->Unlock(m_SYSAllocator.get(), mid, ptr);
}

mfxStatus ÑGeneralAllocator::GetFrameHDL(mfxMemId mid, mfxHDL *handle)
{
  if (isHWMid(mid) && m_HWAllocator.get())
    return m_HWAllocator->GetHDL(m_HWAllocator.get(), mid, handle);
  else
    return m_SYSAllocator->GetHDL(m_SYSAllocator.get(), mid, handle);
}

mfxStatus ÑGeneralAllocator::ReleaseResponse(mfxFrameAllocResponse *response)
{
  // try to ReleaseResponse via D3D allocator
  if (isHWMid(response->mids[0]) && m_HWAllocator.get())
    return m_HWAllocator->Free(m_HWAllocator.get(), response);
  else
    return m_SYSAllocator->Free(m_SYSAllocator.get(), response);
}
mfxStatus ÑGeneralAllocator::AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  mfxStatus sts;
  if ((request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET || request->Type & MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET) && m_HWAllocator.get())
  {
    sts = m_HWAllocator->Alloc(m_HWAllocator.get(), request, response);
    if (sts != MFX_ERR_NONE)
      return sts;
    StoreFrameMids(true, response);
  }
  else
  {
    sts = m_SYSAllocator->Alloc(m_SYSAllocator.get(), request, response);
    if (sts != MFX_ERR_NONE)
      return sts;
    StoreFrameMids(false, response);
  }
  return sts;
}

void ÑGeneralAllocator::StoreFrameMids(bool isD3DFrames, mfxFrameAllocResponse *response)
{
  for (mfxU32 i = 0; i < response->NumFrameActual; i++)
    m_Mids.insert(std::pair<mfxHDL, bool>(response->mids[i], isD3DFrames));
}

bool ÑGeneralAllocator::isHWMid(mfxHDL mid)
{
  const auto it = m_Mids.find(mid);
  if (it == m_Mids.end())
    return false; // sys mem allocator will check validity of mid further

  return it->second;
}
} // namespace MFX