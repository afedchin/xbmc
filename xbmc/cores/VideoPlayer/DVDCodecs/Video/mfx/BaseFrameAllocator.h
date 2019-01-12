/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <list>

extern "C" {
#include <mfxvideo.h>
}

namespace MFX
{

struct mfxAllocatorParams
{
  virtual ~mfxAllocatorParams() = default;
};

class CMFXFrameAllocator : public mfxFrameAllocator
{
public:
  CMFXFrameAllocator();
  virtual ~CMFXFrameAllocator() = default;

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
class CBaseFrameAllocator : public CMFXFrameAllocator
{
public:
  CBaseFrameAllocator();
  virtual ~CBaseFrameAllocator();

  mfxStatus Init(mfxAllocatorParams *pParams) override = 0;
  mfxStatus Close() override;
  mfxStatus AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;
  mfxStatus FreeFrames(mfxFrameAllocResponse *response) override;

protected:
  // we support only decoder
  static const mfxU32 MEMTYPE_FROM_MASK = MFX_MEMTYPE_FROM_DECODE;

  std::list<mfxFrameAllocResponse> m_responses;
  std::list<mfxFrameAllocResponse> m_ExtResponses;

  struct IsSame : std::binary_function<mfxFrameAllocResponse, mfxFrameAllocResponse, bool>
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