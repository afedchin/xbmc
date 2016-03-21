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

#include "SysMemFrameAllocator.h"

extern "C" {
#include <libavutil/macros.h>
#include <libavutil/mem.h>
}

namespace MFX
{

#define MSDK_ALIGN32(X) FFALIGN(X,32)

SysMemFrameAllocator::SysMemFrameAllocator()
{
}

SysMemFrameAllocator::~SysMemFrameAllocator()
{
  Close();
}

mfxStatus SysMemFrameAllocator::Init(mfxAllocatorParams *pParams)
{
  return MFX_ERR_NONE;
}

mfxStatus SysMemFrameAllocator::Close()
{
  return BaseFrameAllocator::Close();
}

mfxStatus SysMemFrameAllocator::LockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (!ptr)
    return MFX_ERR_NULL_PTR;

  sFrame *fs = (sFrame *)mid;
  if (!fs)
    return MFX_ERR_INVALID_HANDLE;

  mfxU16 Width2 = (mfxU16)MSDK_ALIGN32(fs->info.Width);
  mfxU16 Height2 = (mfxU16)MSDK_ALIGN32(fs->info.Height);
  ptr->B = ptr->Y = (mfxU8 *)fs + MSDK_ALIGN32(sizeof(sFrame));

  switch (fs->info.FourCC)
  {
  case MFX_FOURCC_NV12:
    ptr->U = ptr->Y + Width2 * Height2;
    ptr->V = ptr->U + 1;
    ptr->Pitch = Width2;
    break;
  default:
    return MFX_ERR_UNSUPPORTED;
  }

  return MFX_ERR_NONE;
}

mfxStatus SysMemFrameAllocator::UnlockFrame(mfxMemId mid, mfxFrameData *ptr)
{
  if (NULL != ptr)
  {
    ptr->Pitch = 0;
    ptr->Y = 0;
    ptr->U = 0;
    ptr->V = 0;
  }
  return MFX_ERR_NONE;
}

mfxStatus SysMemFrameAllocator::GetFrameHDL(mfxMemId mid, mfxHDL *handle)
{
  return MFX_ERR_UNSUPPORTED;
}

mfxStatus SysMemFrameAllocator::CheckRequestType(mfxFrameAllocRequest *request)
{
  mfxStatus sts = BaseFrameAllocator::CheckRequestType(request);
  if (MFX_ERR_NONE != sts)
    return sts;

  if ((request->Type & MFX_MEMTYPE_SYSTEM_MEMORY) != 0)
    return MFX_ERR_NONE;
  else
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus SysMemFrameAllocator::AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
  mfxU32 numAllocated = 0;

  mfxU32 width = MSDK_ALIGN32(request->Info.Width);
  mfxU32 height = MSDK_ALIGN32(request->Info.Height);
  mfxU32 nbytes;

  switch (request->Info.FourCC)
  {
  case MFX_FOURCC_NV12:
    nbytes = width*height + (width >> 1)*(height >> 1) + (width >> 1)*(height >> 1);
    break;
  default:
    return MFX_ERR_UNSUPPORTED;
  }

  mfxMemId *mids = new mfxMemId[request->NumFrameSuggested];
  if (!mids)
    return MFX_ERR_MEMORY_ALLOC;

  // allocate frames
  for (numAllocated = 0; numAllocated < request->NumFrameSuggested; numAllocated++)
  {
    mfxU8 *buffer_ptr = (mfxU8 *)av_malloc(nbytes + MSDK_ALIGN32(sizeof(sFrame)));
    if (!buffer_ptr)
      return MFX_ERR_MEMORY_ALLOC;

    sFrame *fs = (sFrame *)buffer_ptr;
    fs->type = request->Type;
    fs->nbytes = nbytes;
    fs->info = request->Info;
    mids[numAllocated] = (mfxHDL)fs;
  }

  // check the number of allocated frames
  if (numAllocated < request->NumFrameSuggested)
  {
    return MFX_ERR_MEMORY_ALLOC;
  }

  response->NumFrameActual = (mfxU16)numAllocated;
  response->mids = mids;

  return MFX_ERR_NONE;
}

mfxStatus SysMemFrameAllocator::ReleaseResponse(mfxFrameAllocResponse *response)
{
  if (!response)
    return MFX_ERR_NULL_PTR;

  if (response->mids)
  {
    for (mfxU32 i = 0; i < response->NumFrameActual; i++)
    {
      if (response->mids[i])
        av_freep(&response->mids[i]);
    }
  }

  delete[] response->mids;
  response->mids = 0;

  return MFX_ERR_NONE;
}
} // namespace MFX
