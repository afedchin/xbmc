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

#include "utils/StdString.h"
#include "Archive.h"
#include "ISerializable.h"
#include <vector>

class CStreamDetails;

class CStreamDetail : public IArchivable, public ISerializable
{
public:
  enum StreamType {
    VIDEO,
    AUDIO,
    SUBTITLE
#ifdef HAS_DS_PLAYER
	,
	EDITION	= 18,	
	BD_TITLE,
	PROGRAMM
#endif
  };

  CStreamDetail(StreamType type) : m_eType(type), m_pParent(NULL) {};
  virtual void Archive(CArchive& ar);
  virtual void Serialize(CVariant& value) const;
  virtual bool IsWorseThan(CStreamDetail *that) { return true; };

  const StreamType m_eType;

protected:
  CStreamDetails *m_pParent;
  friend class CStreamDetails;
};

class CStreamDetailVideo : public CStreamDetail
{
public:
  CStreamDetailVideo();
  virtual void Archive(CArchive& ar);
  virtual void Serialize(CVariant& value) const;
  virtual bool IsWorseThan(CStreamDetail *that);

  int m_iWidth;
  int m_iHeight;
  float m_fAspect;
  int m_iDuration;
  CStdString m_strCodec;
  std::string m_strStereoMode;
#ifdef HAS_DS_PLAYER
  unsigned long m_iFourcc;
#endif
};

class CStreamDetailAudio : public CStreamDetail
{
public:
  CStreamDetailAudio();
  virtual void Archive(CArchive& ar);
  virtual void Serialize(CVariant& value) const;
  virtual bool IsWorseThan(CStreamDetail *that);

  int m_iChannels;
  CStdString m_strCodec;
  CStdString m_strLanguage;
};

class CStreamDetailSubtitle : public CStreamDetail
{
public:
  CStreamDetailSubtitle();
  CStreamDetailSubtitle& operator=(const CStreamDetailSubtitle &that);
  virtual void Archive(CArchive& ar);
  virtual void Serialize(CVariant& value) const;
  virtual bool IsWorseThan(CStreamDetail *that);

  CStdString m_strLanguage;
};

#ifdef HAS_DS_PLAYER
class CStreamDetailEditon : public CStreamDetail
{
public:
	CStreamDetailEditon();
	virtual void Archive(CArchive& ar);
	virtual void Serialize(CVariant& value);
	CStdString m_strName;
};
#endif

class CStreamDetails : public IArchivable, public ISerializable
{
public:
  CStreamDetails() { Reset(); };
  CStreamDetails(const CStreamDetails &that);
  ~CStreamDetails() { Reset(); };
  CStreamDetails& operator=(const CStreamDetails &that);
  bool operator ==(const CStreamDetails &that) const;
  bool operator !=(const CStreamDetails &that) const;

  static CStdString VideoDimsToResolutionDescription(int iWidth, int iHeight);
  static CStdString VideoAspectToAspectDescription(float fAspect);

  bool HasItems(void) const { return m_vecItems.size() > 0; };
  int GetStreamCount(CStreamDetail::StreamType type) const;
  int GetVideoStreamCount(void) const;
  int GetAudioStreamCount(void) const;
  int GetSubtitleStreamCount(void) const;
  const CStreamDetail* GetNthStream(CStreamDetail::StreamType type, int idx) const;

  CStdString GetVideoCodec(int idx = 0) const;
  float GetVideoAspect(int idx = 0) const;
  int GetVideoWidth(int idx = 0) const;
  int GetVideoHeight(int idx = 0) const;
  int GetVideoDuration(int idx = 0) const;
  void SetVideoDuration(int idx, const int duration);
  std::string GetStereoMode(int idx = 0) const;
#ifdef HAS_DS_PLAYER
  CStdString GetVideoFourcc(int idx = 0) const;
#endif
  CStdString GetAudioCodec(int idx = 0) const;
  CStdString GetAudioLanguage(int idx = 0) const;
  int GetAudioChannels(int idx = 0) const;

  CStdString GetSubtitleLanguage(int idx = 0) const;

  void AddStream(CStreamDetail *item);
  void Reset(void);
  void DetermineBestStreams(void);

  virtual void Archive(CArchive& ar);
  virtual void Serialize(CVariant& value) const;

private:
  CStreamDetail *NewStream(CStreamDetail::StreamType type);
  std::vector<CStreamDetail *> m_vecItems;
  CStreamDetailVideo *m_pBestVideo;
  CStreamDetailAudio *m_pBestAudio;
  CStreamDetailSubtitle *m_pBestSubtitle;
};
