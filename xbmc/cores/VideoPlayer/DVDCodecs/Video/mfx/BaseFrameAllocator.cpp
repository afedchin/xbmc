/*
*      Copyright (C) 2005-2013 Team XBMC
*      http://xbmc.org
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
#include <algorithm>
#include <functional>

namespace MFX
{

MFXFrameAllocator::MFXFrameAllocator()
{
  pthis = this;
  Alloc = Alloc_;
  Lock = Lock_;
  Free = Free_;
  Unlock = Unlock_;
  GetHDL = GetHDL_;
}

MFXFrameAllocator::~MFXFrameAllocator()
{
}

mfxStatus MFXFrameAllocator::Alloc_(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  if (0 == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  MFXFrameAllocator& self = *(MFXFrameAllocator *)pthis;

  return self.AllocFrames(request, response);
}

mfxStatus MFXFrameAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
  if (0 == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  MFXFrameAllocator& self = *(MFXFrameAllocator *)pthis;

  return self.LockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
  if (0 == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  MFXFrameAllocator& self = *(MFXFrameAllocator *)pthis;

  return self.UnlockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Free_(mfxHDL pthis, mfxFrameAllocResponse *response)
{
  if (0 == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  MFXFrameAllocator& self = *(MFXFrameAllocator *)pthis;

  return self.FreeFrames(response);
}

mfxStatus MFXFrameAllocator::GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
  if (0 == pthis)
    return MFX_ERR_MEMORY_ALLOC;

  MFXFrameAllocator& self = *(MFXFrameAllocator *)pthis;

  return self.GetFrameHDL(mid, handle);
}

BaseFrameAllocator::BaseFrameAllocator()
{
}

BaseFrameAllocator::~BaseFrameAllocator()
{
}

mfxStatus BaseFrameAllocator::CheckRequestType(mfxFrameAllocRequest *request)
{
  if (0 == request)
    return MFX_ERR_NULL_PTR;

  // check that Media SDK component is specified in request
  if ((request->Type & MEMTYPE_FROM_MASK) != 0)
    return MFX_ERR_NONE;
  else
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus BaseFrameAllocator::AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  if (0 == request || 0 == response || 0 == request->NumFrameSuggested)
    return MFX_ERR_MEMORY_ALLOC;

  if (MFX_ERR_NONE != CheckRequestType(request))
    return MFX_ERR_UNSUPPORTED;

  mfxStatus sts = MFX_ERR_NONE;

  if ((request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) && (request->Type & MFX_MEMTYPE_FROM_DECODE))
  {
    // external decoder allocations
    bool foundInCache = false;
    for (auto it = m_ExtResponses.begin(); it != m_ExtResponses.end(); ++it)
    {
      // same decoder and same size
      if (request->AllocId == it->AllocId)
      {
        // check if enough frames were allocated
        if (request->NumFrameSuggested > it->NumFrameActual)
          return MFX_ERR_MEMORY_ALLOC;

        // return existing response
        *response = (mfxFrameAllocResponse&)*it;
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
    m_responses.push_back(mfxFrameAllocResponse());

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

mfxStatus BaseFrameAllocator::FreeFrames(mfxFrameAllocResponse *response)
{
  if (response == 0)
    return MFX_ERR_INVALID_HANDLE;

  mfxStatus sts = MFX_ERR_NONE;

  // check whether response is an external decoder response
  auto i = std::find_if(m_ExtResponses.begin(), m_ExtResponses.end(), std::bind1st(IsSame(), *response));

  if (i != m_ExtResponses.end())
  {
    sts = ReleaseResponse(response);
    m_ExtResponses.erase(i);
    return sts;
  }

  // if not found so far, then search in internal responses
  auto i2 = std::find_if(m_responses.begin(), m_responses.end(), std::bind1st(IsSame(), *response));

  if (i2 != m_responses.end())
  {
    sts = ReleaseResponse(response);
    m_responses.erase(i2);
    return sts;
  }

  // not found anywhere, report an error
  return MFX_ERR_INVALID_HANDLE;
}

mfxStatus BaseFrameAllocator::Close()
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
