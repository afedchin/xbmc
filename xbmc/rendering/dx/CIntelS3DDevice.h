/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://www.xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#ifdef HAS_DX

#pragma once

#define MEMCPY_VAR(dstVarName, src, count) memcpy_s(&(dstVarName), sizeof(dstVarName), (src), (count))

#include <d3d9.h>
#include <dxva2api.h>
#include "IS3DDevice.h"
#include "guilib/D3DResource.h"
#include "settings/lib/ISettingCallback.h"
#include "win32/igfx_s3dcontrol/igfx_s3dcontrol.h"

class IS3DDevice;
class IGFXS3DControl;

class CIntelS3DDevice: public IS3DDevice
                     , public ID3DResource
                     , public ISettingCallback
{
public:
  CIntelS3DDevice(IDirect3D9Ex* pD3D);
 ~CIntelS3DDevice();

  // correct present params for correct stereo rendering some implementations need it
  bool CorrectPresentParams(D3DPRESENT_PARAMETERS *pD3DPP) override;

  // Returns true if S3D is supported by the platform and exposes supported display modes 
  // !! m.b. not needed
  bool GetS3DCaps(S3D_CAPS *pCaps) override;

  // Switch the monitor to 3D mode
  // Call with NULL to use current display mode
  bool SwitchTo3D(S3D_DISPLAY_MODE *pMode) override;

  // Switch the monitor back to 2D mode
  // Call with NULL to use current display mode
  bool SwitchTo2D(S3D_DISPLAY_MODE *pMode) override;
    
  // Activate left view, requires device to be set
  bool SelectLeftView(void) override;

  // Activates right view, requires device to be set
  bool SelectRightView(void) override;

  // 
  bool PresentFrame(void) override;

  void UnInit(void) override;

  void OnCreateDevice() override;
  void OnDestroyDevice() override;
  void OnLostDevice() override;
  void OnResetDevice() override;

  bool OnSettingChanging(const CSetting *setting) override;
  bool UseWindowedMode() override { return true; }

protected:
  bool PreInit(void) override;
  bool Less(const IGFX_DISPLAY_MODE &l, const IGFX_DISPLAY_MODE& r);
  bool CheckOverlaySupport(int iWidth, int iHeight, D3DFORMAT dFormat);

  // various structures for S3D and DXVA2 calls

  bool                            m_restoreFFScreen;
  unsigned int                    m_resetToken;

  IDirect3DDevice9Ex*             m_pD3DDevice;
  IGFX_S3DCAPS                    m_S3DCaps;
  IGFX_DISPLAY_MODE               m_S3DPrefMode;       // Prefered S3D display mode
  DXVA2_VideoDesc                 m_VideoDesc;
  DXVA2_VideoProcessBltParams     m_BltParams; 
  DXVA2_VideoSample               m_Sample;            // Simple sample :)
   
  IGFXS3DControl*                 m_pS3DControl;
  IDirect3DDeviceManager9*        m_pDeviceManager9;   
  IDirectXVideoProcessorService*  m_pProcessService;   // Service required to create video processors
  IDirectXVideoProcessor*         m_pProcessLeft;      // Left channel processor
  IDirectXVideoProcessor*         m_pProcessRight;     // Right channel processor
  IDirect3DSurface9*              m_pRenderSurface;    // The surface for L+R results

};

#endif // HAS_DX