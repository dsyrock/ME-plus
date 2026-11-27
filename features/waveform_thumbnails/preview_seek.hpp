#pragma once
// Include after reaper_plugin.h (Win32 or host-provided SWELL).
#include "waveform_file.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mwt {
inline std::string previewText(HWND w) {
#ifdef _WIN32
  std::array<wchar_t,32768> wide{};
  int length=GetWindowTextW(w,wide.data(),int(wide.size()));
  int size=WideCharToMultiByte(CP_UTF8,0,wide.data(),length,nullptr,0,nullptr,nullptr);
  std::string result(size,'\0');
  if(size)WideCharToMultiByte(CP_UTF8,0,wide.data(),length,result.data(),size,nullptr,nullptr);
  return result;
#else
  std::array<char,32768> value{};GetWindowText(w,value.data(),int(value.size()));return value.data();
#endif
}
inline double thumbnailRatio(double x,double left,double right) {
  return std::clamp((x-left-6)/std::max(1.0,right-left-13),0.0,1.0);
}
inline bool previewModifiers() {
  return (GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_MENU))&0x8000;
}
#ifdef _WIN32
// REAPERhfader uses the trackbar position messages. The scroll notification
// applies that position to native preview gain. Position zero is -inf dB.
class ScopedPreviewMute {
  HWND root, fader;
  LRESULT position;
  void setPosition(LRESULT value) const {
    SendMessage(fader,WM_USER+5,TRUE,value); // TBM_SETPOS
    SendMessage(root,WM_HSCROLL,SB_THUMBTRACK,reinterpret_cast<LPARAM>(fader));
  }
public:
  explicit ScopedPreviewMute(HWND window)
    : root(window), fader(GetDlgItem(window,1045)), position(0) {
    wchar_t className[32]{};
    if(!GetClassNameW(fader,className,32) || std::wstring_view(className)!=L"REAPERhfader") {
      fader=nullptr;
      return;
    }
    position=SendMessage(fader,WM_USER,0,0); // TBM_GETPOS
    setPosition(0);
  }
  ~ScopedPreviewMute() {
    if(fader && IsWindow(fader) && IsWindow(root)) setPosition(position);
  }
  bool available() const {return fader!=nullptr;}
  void stop() const {
    if(IsWindow(root)) SendMessage(root,WM_COMMAND,1009,0);
  }
  ScopedPreviewMute(const ScopedPreviewMute&) = delete;
  ScopedPreviewMute& operator=(const ScopedPreviewMute&) = delete;
};
#endif
class PreviewSeek {
  std::string pendingPath;
  double ratio=0;
  uint64_t requestedAt=0,releasedAt=0,openedAt=0;
  bool released=false,opened=false;
#ifdef _WIN32
  std::optional<ScopedPreviewMute> mute;
  uint64_t gestureAt=0;
#endif
public:
#ifdef _WIN32
  using PreviewInfo=bool (*)(char*,int,int*,double*,double*,double*,double*,double*,double*,char*,int);
  PreviewInfo getPreviewInfo=nullptr;
#endif
  uint64_t requests=0,completed=0,cancelled=0;
  double lastRatio=0;
  std::string lastPath,error;
  bool pending()const{return !pendingPath.empty();}
  const std::string& path()const{return pendingPath;}
  bool previewMatches(HWND root) const {
    const auto requested=std::filesystem::u8path(pendingPath);
#ifdef _WIN32
    if(getPreviewInfo){
      std::array<char,32768> file{};
      int mode=0;
      double start=0,end=0,pitch=0,volume=0,rate=0,bpm=0;
      char extra[1]{};
      if(!getPreviewInfo(file.data(),int(file.size()),&mode,&start,&end,&pitch,&volume,&rate,&bpm,extra,int(sizeof(extra)))||!file[0])
        return false;
      const auto actualPath=std::filesystem::u8path(file.data()).lexically_normal().wstring();
      const auto wantedPath=requested.lexically_normal().wstring();
      return CompareStringOrdinal(actualPath.c_str(),-1,wantedPath.c_str(),-1,TRUE)==CSTR_EQUAL;
    }
#endif
    const auto label=previewText(GetDlgItem(root,1014));
    const auto name=requested.filename().u8string();
    if(label.find(name)!=std::string::npos)return true;
#ifdef _WIN32
    // Static controls escape ampersands that would otherwise be accelerators.
    std::string escaped;
    for(char ch:name){escaped+=ch;if(ch=='&')escaped+='&';}
    return escaped!=name&&label.find(escaped)!=std::string::npos;
#else
    return false;
#endif
  }
  bool request(const std::string& file,double fraction,uint64_t time) {
    if(!waveformPath(file)||!std::isfinite(fraction))return false;
    pendingPath=file;ratio=std::clamp(fraction,0.0,1.0);requestedAt=time;
    released=opened=false;error.clear();++requests;return true;
  }
#ifdef _WIN32
  bool muting() const {return mute.has_value();}
  bool beginGesture(HWND root,uint64_t time) {
    if(!mute)mute.emplace(root);
    if(!mute->available()){mute.reset();return false;}
    gestureAt=time;
    return true;
  }
  void expireGesture(uint64_t time) {
    if(mute&&!pending()&&time-gestureAt>=4000)cancel();
  }
#endif
  void cancel(){
    if(pending()){++cancelled;pendingPath.clear();}
#ifdef _WIN32
    if(mute){mute->stop();mute.reset();}
#endif
  }
  // Selected path is re-resolved by the backend: a queued click must not act
  // on an old row after sorting, filtering, changing directory or selection.
  bool tick(HWND root,const std::string& selectedPath,uint64_t time) {
    if(!pending())return false;
    if(!IsWindow(root)||selectedPath!=pendingPath){cancel();return true;}
    if(time-requestedAt>4000){error="Preview seek timed out";cancel();return true;}
    if(GetAsyncKeyState(VK_LBUTTON)&0x8000)return false;
#ifndef _WIN32
    if(!released){released=true;releasedAt=time;return false;}
    if(time-releasedAt<60)return false;
#endif
    if(!opened){
#ifdef _WIN32
      if(!beginGesture(root,time)){error="Native preview volume control unavailable";cancel();return true;}
#endif
      SendMessage(root,WM_COMMAND,1009,0); // native Preview: Stop
      // Load from the current list selection, never OpenMediaExplorer(path):
      // that API can navigate out of a database/search to the source folder.
      // Stop immediately in the same UI callback, before the deferred seek.
      SendMessage(root,WM_COMMAND,1008,0); // native Preview: Play/load selection
      SendMessage(root,WM_COMMAND,1009,0);
      opened=true;openedAt=time;return true;
    }
    // Allow the native preview pane to finish installing the new source.
    if(time-openedAt<100)return false;
    HWND wave=GetDlgItem(root,1046);
    if(!wave||!previewMatches(root)||previewText(GetDlgItem(root,1016)).empty())return false;
    RECT bounds{};GetClientRect(wave,&bounds);
    int width=int(bounds.right-bounds.left),height=int(bounds.bottom-bounds.top);
    if(width<16||height<1){error="Expand the native Media Explorer preview panel to seek";cancel();return true;}
    // The thumbnail represents the whole source, regardless of preview zoom.
    // Reset the native view rather than guessing its current scroll range.
    SendMessage(root,WM_COMMAND,42093,0); // native Preview: Zoom out full
    int x=std::clamp(int(std::lround(ratio*width)),0,width-1);
    int y=height/2;
    SendMessage(wave,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessage(wave,WM_LBUTTONUP,0,MAKELPARAM(x,y));
#ifdef _WIN32
    // Some native waveform gestures can start playback themselves. Stop that
    // preview while it is still muted, then play from the saved seek position.
    if(!mute||!pending())return false;
    mute->stop();
    mute.reset();
#endif
    SendMessage(root,WM_COMMAND,40027,0); // native Play from last seek position
    lastPath=pendingPath;lastRatio=ratio;++completed;pendingPath.clear();return true;
  }
};
}
