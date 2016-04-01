#pragma once
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

#include <list>

extern "C" {
#include <libmfx/mfxvideo.h>
}

namespace MFX
{

struct mfxAllocatorParams
{
  virtual ~mfxAllocatorParams(){};
};

class MFXFrameAllocator : public mfxFrameAllocator
{
public:
  MFXFrameAllocator();
  virtual ~MFXFrameAllocator();

  // optional method, override if need to pass some parameters to allocator from application
  virtual mfxStatus Init(mfxAllocatorParams *pParams) = 0;
  virtual mfxStatus Close() = 0;

  virtual mfxStatus AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) = 0;
  virtual mfxStatus LockFrame(mfxMemId mid, mfxFrameData *ptr) = 0;
  virtual mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData *ptr) = 0;
  virtual mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL *handle) = 0;
  virtual mfxStatus FreeFrames(mfxFrameAllocResponse *response) = 0;

private:
  static mfxStatus MFX_CDECL  Alloc_(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response);
  static mfxStatus MFX_CDECL  Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr);
  static mfxStatus MFX_CDECL  Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr);
  static mfxStatus MFX_CDECL  GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL *handle);
  static mfxStatus MFX_CDECL  Free_(mfxHDL pthis, mfxFrameAllocResponse *response);
};

// This class does not allocate any actual memory
class BaseFrameAllocator : public MFXFrameAllocator
{
public:
  BaseFrameAllocator();
  virtual ~BaseFrameAllocator();

  virtual mfxStatus Init(mfxAllocatorParams *pParams) = 0;
  virtual mfxStatus Close() override;
  virtual mfxStatus AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;
  virtual mfxStatus FreeFrames(mfxFrameAllocResponse *response) override;

protected:
  // we support only decoder
  static const mfxU32 MEMTYPE_FROM_MASK = MFX_MEMTYPE_FROM_DECODE;

  std::list<mfxFrameAllocResponse> m_responses;
  std::list<mfxFrameAllocResponse> m_ExtResponses;

  struct IsSame
    : public std::binary_function<mfxFrameAllocResponse, mfxFrameAllocResponse, bool>
  {
    bool operator () (const mfxFrameAllocResponse & l, const mfxFrameAllocResponse &r)const
    {
      return r.mids != 0 && l.mids != 0 &&
        r.mids[0] == l.mids[0] &&
        r.NumFrameActual == l.NumFrameActual;
    }
  };

  // checks if request is supported
  virtual mfxStatus CheckRequestType(mfxFrameAllocRequest *request);
  // frees memory attached to response
  virtual mfxStatus ReleaseResponse(mfxFrameAllocResponse *response) = 0;
  // allocates memory
  virtual mfxStatus AllocImpl(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) = 0;
};

}; // namespace MFX