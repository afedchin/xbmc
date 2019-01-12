/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BaseFrameAllocator.h"
#include <algorithm>
#include <functional>

namespace MFX
{

CMFXFrameAllocator::CMFXFrameAllocator()
    : mfxFrameAllocator()
{
  pthis = this;
  Alloc = Alloc_;
  Lock = Lock_;
  Free = Free_;
  Unlock = Unlock_;
  GetHDL = GetHDL_;
}

mfxStatus CMFXFrameAllocator::Alloc_(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  if (nullptr == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  CMFXFrameAllocator& self = *static_cast<CMFXFrameAllocator*>(pthis);

  return self.AllocFrames(request, response);
}

mfxStatus CMFXFrameAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
  if (nullptr == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  CMFXFrameAllocator& self = *static_cast<CMFXFrameAllocator*>(pthis);

  return self.LockFrame(mid, ptr);
}

mfxStatus CMFXFrameAllocator::Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
  if (nullptr == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  CMFXFrameAllocator& self = *static_cast<CMFXFrameAllocator*>(pthis);

  return self.UnlockFrame(mid, ptr);
}

mfxStatus CMFXFrameAllocator::Free_(mfxHDL pthis, mfxFrameAllocResponse *response)
{
  if (nullptr == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  CMFXFrameAllocator& self = *static_cast<CMFXFrameAllocator*>(pthis);

  return self.FreeFrames(response);
}

mfxStatus CMFXFrameAllocator::GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
  if (nullptr == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  CMFXFrameAllocator& self = *static_cast<CMFXFrameAllocator*>(pthis);

  return self.GetFrameHDL(mid, handle);
}

CBaseFrameAllocator::CBaseFrameAllocator() = default;

CBaseFrameAllocator::~CBaseFrameAllocator() = default;

mfxStatus CBaseFrameAllocator::CheckRequestType(mfxFrameAllocRequest *request)
{
  if (nullptr == request)
    return MFX_ERR_NULL_PTR;

  // check that Media SDK component is specified in request
  if ((request->Type & MEMTYPE_FROM_MASK) != 0)
    return MFX_ERR_NONE;

  return MFX_ERR_UNSUPPORTED;
}

mfxStatus CBaseFrameAllocator::AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  if (nullptr == request || nullptr == response || 0 == request->NumFrameSuggested)
    return MFX_ERR_MEMORY_ALLOC;

  if (MFX_ERR_NONE != CheckRequestType(request))
    return MFX_ERR_UNSUPPORTED;

  mfxStatus sts = MFX_ERR_NONE;

  if ((request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) && (request->Type & MFX_MEMTYPE_FROM_DECODE))
  {
    // external decoder allocations
    bool foundInCache = false;
    for (auto& m_ExtResponse : m_ExtResponses)
    {
      // same decoder and same size
      if (request->AllocId == m_ExtResponse.AllocId)
      {
        // check if enough frames were allocated
        if (request->NumFrameSuggested > m_ExtResponse.NumFrameActual)
          return MFX_ERR_MEMORY_ALLOC;

        // return existing response
        *response = static_cast<mfxFrameAllocResponse&>(m_ExtResponse);
        foundInCache = true;
      }
    }

    if (!foundInCache)
    {
      sts = AllocImpl(request, response);
      if (sts == MFX_ERR_NONE)
      {
        response->AllocId = request->AllocId;
        m_ExtResponses.push_back(*response);
      }
    }
  }
  else
  {
    // internal allocations
    // reserve space before allocation to avoid memory leak
    m_responses.emplace_back();

    sts = AllocImpl(request, response);
    if (sts == MFX_ERR_NONE)
    {
      m_responses.back() = *response;
    }
    else
    {
      m_responses.pop_back();
    }
  }

  return sts;
}

mfxStatus CBaseFrameAllocator::FreeFrames(mfxFrameAllocResponse *response)
{
  if (response == nullptr)
    return MFX_ERR_INVALID_HANDLE;

  mfxStatus sts;

  // check whether response is an external decoder response
  const auto i = std::find_if(m_ExtResponses.begin(), m_ExtResponses.end(), std::bind1st(IsSame(), *response));

  if (i != m_ExtResponses.end())
  {
    sts = ReleaseResponse(response);
    m_ExtResponses.erase(i);
    return sts;
  }

  // if not found so far, then search in internal responses
  const auto i2 = std::find_if(m_responses.begin(), m_responses.end(), std::bind1st(IsSame(), *response));

  if (i2 != m_responses.end())
  {
    sts = ReleaseResponse(response);
    m_responses.erase(i2);
    return sts;
  }

  // not found anywhere, report an error
  return MFX_ERR_INVALID_HANDLE;
}

mfxStatus CBaseFrameAllocator::Close()
{
  for (auto i = m_ExtResponses.begin(); i != m_ExtResponses.end(); i++)
  {
    ReleaseResponse(&*i);
  }
  m_ExtResponses.clear();

  for (auto i2 = m_responses.begin(); i2 != m_responses.end(); i2++)
  {
    ReleaseResponse(&*i2);
  }
  m_responses.clear();

  return MFX_ERR_NONE;
}

} // namespace MFX
