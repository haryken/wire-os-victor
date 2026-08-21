/**
 * File: faceDisplay.cpp
 *
 * Author: Kevin Yoon
 * Created: 12/12/2017
 *
 * Description:
 *               Defines interface to face display
 *
 * Copyright: Anki, Inc. 2017
 *
 **/

#include "cozmoAnim/faceDisplay/faceDisplay.h"
#include "cozmoAnim/faceDisplay/faceDisplayImpl.h"
#include "cozmoAnim/faceDisplay/faceInfoScreenManager.h"
#include "coretech/common/shared/array2d.h"
#include "coretech/vision/engine/image.h"
#include "coretech/common/engine/colorRGBA.h"
#include "util/console/consoleInterface.h"
#include "util/cpuProfiler/cpuProfiler.h"
#include "util/threading/threadPriority.h"
#include "cozmoAnim/execCommand/exec_command.h"
#include "util/fileUtils/fileUtils.h"
#include "anki/cozmo/shared/cozmoConfig.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <string>

#define LOG_CHANNEL "FaceDisplay"

// Whether or not we need to manually stop the boot animation process, vic-bootAnim
#define MANUALLY_STOP_BOOT_ANIM 0

namespace Anki {
namespace Vector {

#if ANKI_CPU_PROFILER_ENABLED
  CONSOLE_VAR_RANGED(float, maxDrawTime_ms,      ANKI_CPU_CONSOLEVARGROUP, 5, 5, 32);
  CONSOLE_VAR_ENUM(u8,      kDrawFace_Logging,   ANKI_CPU_CONSOLEVARGROUP, 0, Util::CpuProfiler::CpuProfilerLogging());
#endif

namespace {
#if REMOTE_CONSOLE_ENABLED
  FaceDisplayImpl* sDisplayImpl = nullptr;

  void SetFaceBrightness(ConsoleFunctionContextRef context) {
    if( nullptr == sDisplayImpl ) {
      return;
    }

    const int val = ConsoleArg_GetOptional_Int(context, "val", 1);

    if( val >= 0 && val <= 20 ) {
      sDisplayImpl->SetFaceBrightness(val);
    }
    else {
      LOG_WARNING("FaceDisplay.SetFaceBrightness.Invalid",
                  "Brightness value %d is invalid, refusing to set",
                  val);
    }
  }

  CONSOLE_FUNC(SetFaceBrightness, "FaceDisplay", int val);
#endif

  enum class WireosWifiFace : uint8_t { None, Trying, Ok, Fail };

  static uint64_t WireosNowMs()
  {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }

  static WireosWifiFace ReadWireosWifiFace()
  {
    static WireosWifiFace cached = WireosWifiFace::None;
    static uint64_t lastRead = 0;
    const uint64_t now = WireosNowMs();
    if (lastRead != 0 && (now - lastRead) < 400) {
      return cached;
    }
    lastRead = now;
    const char* kPath = "/run/wireos-wifi-face";
    if (!Util::FileUtils::FileExists(kPath)) {
      cached = WireosWifiFace::None;
      return cached;
    }
    std::string s = Util::FileUtils::ReadFile(kPath);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
      s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
      s.pop_back();
    }
    if (s == "trying" || s == "accepted") {
      cached = WireosWifiFace::Trying;
    } else if (s == "ok") {
      cached = WireosWifiFace::Ok;
    } else if (s == "fail") {
      cached = WireosWifiFace::Fail;
    } else {
      cached = WireosWifiFace::None;
    }
    return cached;
  }

  static void DrawWifiArc(Vision::ImageRGB565& img, const Point2f& c, f32 r, const ColorRGBA& color, s32 thick)
  {
    Point2f prev;
    bool have = false;
    for (int deg = 200; deg <= 340; deg += 10) {
      const f32 rad = static_cast<f32>(deg) * 3.14159265f / 180.f;
      const Point2f p(c.x() + r * std::cos(rad), c.y() + r * std::sin(rad));
      if (have) {
        img.DrawLine(prev, p, color, thick);
      }
      prev = p;
      have = true;
    }
  }

  static void DrawWireosWifiOverlay(Vision::ImageRGB565& img)
  {
    const WireosWifiFace st = ReadWireosWifiFace();
    if (st == WireosWifiFace::None) {
      return;
    }

    const ColorRGBA cyan(0.40f, 0.91f, 0.97f);
    const ColorRGBA green(0.13f, 0.77f, 0.37f);
    const ColorRGBA red(0.94f, 0.27f, 0.27f);

    const f32 ox = static_cast<f32>(FACE_DISPLAY_WIDTH) - 34.f;
    const f32 oy = 2.f;
    const Point2f origin(ox + 13.f, oy + 20.f);

    bool drawOuter = true;
    if (st == WireosWifiFace::Trying) {
      drawOuter = ((WireosNowMs() / 400) % 2) == 0;
    }
    if (drawOuter) {
      DrawWifiArc(img, origin, 14.f, cyan, 2);
    }
    DrawWifiArc(img, origin, 9.f, cyan, 2);
    DrawWifiArc(img, origin, 5.f, cyan, 2);
    img.DrawFilledCircle(origin, cyan, 2);

    if (st == WireosWifiFace::Ok) {
      img.DrawLine({ox + 17.f, oy + 16.f}, {ox + 21.f, oy + 21.f}, green, 2);
      img.DrawLine({ox + 21.f, oy + 21.f}, {ox + 31.f, oy + 9.f}, green, 2);
    } else if (st == WireosWifiFace::Fail) {
      img.DrawLine({ox + 17.f, oy + 8.f}, {ox + 29.f, oy + 20.f}, red, 2);
      img.DrawLine({ox + 29.f, oy + 8.f}, {ox + 17.f, oy + 20.f}, red, 2);
    }
  }
}

  
FaceDisplay::FaceDisplay()
: _stopDrawFace(false)
, _readyFace(false)
, _stopBootAnim(false)
{
  // Don't try to stop the boot anim in sim
  // or if we are not supposed to manunually stop it
  // (systemd will stop it for us)
#if defined(SIMULATOR) || !MANUALLY_STOP_BOOT_ANIM
  _stopBootAnim = true;
#endif
	
  // The boot anim process may be using the display so don't actually create
  // a FaceDisplay until we know for sure that process is no longer running
  _displayImpl.reset(nullptr);
  
  // Set up our thread running data
  _faceDrawImg[0].reset(new Vision::ImageRGB565());
  _faceDrawImg[0]->Allocate(FACE_DISPLAY_HEIGHT, FACE_DISPLAY_WIDTH);
  _faceDrawImg[1].reset(new Vision::ImageRGB565());
  _faceDrawImg[1]->Allocate(FACE_DISPLAY_HEIGHT, FACE_DISPLAY_WIDTH);
  _faceDrawThread = std::thread(&FaceDisplay::DrawFaceLoop, this);
}

FaceDisplay::~FaceDisplay()
{
#if REMOTE_CONSOLE_ENABLED
  sDisplayImpl = nullptr;
#endif

  _stopDrawFace = true;
  {
    // Since the face-drawing thread is often waiting for a signal that there is
    // a new face to be drawn, send that signal now so that it will stop waiting
    // (and end execution soon thereafter.)
    std::unique_lock<std::mutex> lock{_readyMutex};
    _readyFace = true;
  }
  _readyCondition.notify_all();

  _faceDrawThread.join();
}

void FaceDisplay::UpdateNextImgPtr()
{
  if(_faceDrawNextImg == nullptr)
  {
    _faceDrawNextImg = _faceDrawImg[0].get();
    if(_faceDrawCurImg == _faceDrawNextImg)
    {
      _faceDrawNextImg = _faceDrawImg[1].get();
    }
  }
}

void FaceDisplay::DrawToFaceDebug(const Vision::ImageRGB565& img)
{
  // We want to allow FaceInfoScreenManager to draw in the None screen in particular
  // in order to clear since there are no eyes to clear it for us.
  if (!FACTORY_TEST && !FaceInfoScreenManager::getInstance()->IsActivelyDrawingToScreen())
  {
    return;
  }

  DrawToFaceInternal(img);
}

void FaceDisplay::SetFaceBrightness(LCDBrightness level)
{
  if(_displayImpl != nullptr)
  {
    _displayImpl->SetFaceBrightness(EnumToUnderlyingType(level));
  }
}

void FaceDisplay::DrawToFace(const Vision::ImageRGB565& img)
{
  if (FaceInfoScreenManager::getInstance()->IsActivelyDrawingToScreen())
  {
    return;
  }

  DrawToFaceInternal(img);
}

void FaceDisplay::DrawToFaceInternal(const Vision::ImageRGB565& img)
{
  // Don't update images and pointers while the boot animation is still playing
  if(!_stopBootAnim)
  {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(_faceDrawMutex);
    UpdateNextImgPtr();
    img.CopyTo(*_faceDrawNextImg);
    DrawWireosWifiOverlay(*_faceDrawNextImg);
  }

  // Notify the face-drawing thread that there is a face to draw
  {
    std::unique_lock<std::mutex> lock{_readyMutex};
    _readyFace = true;
  }
  _readyCondition.notify_all();
}

void FaceDisplay::DrawFaceLoop()
{
  Anki::Util::SetThreadName(pthread_self(), "DrawFaceLoop");

  while (!_stopDrawFace)
  {
    // Note that this CPU profiler tag is less useful now that we are waiting on a condition variable in this loop
    ANKI_CPU_TICK("FaceDisplay::DrawFaceLoop", maxDrawTime_ms, Util::CpuProfiler::CpuProfilerLoggingTime(kDrawFace_Logging));

    // Lock because we're about to check and change pointers
    _faceDrawMutex.lock();

    if(_displayImpl == nullptr && _stopBootAnim)
    {      
     // Actually create a FaceDisplay which will open a connection to the LCD
     // now that no other process is using it
     _displayImpl.reset(new FaceDisplayImpl());

#if REMOTE_CONSOLE_ENABLED
     sDisplayImpl = _displayImpl.get();
#endif
    }

    if (_faceDrawNextImg != nullptr)
    {
      _faceDrawCurImg = _faceDrawNextImg;
      _faceDrawNextImg = nullptr;

      // Grab a reference to the image we're going to draw so we can release the mutex
      const auto& drawImage = *_faceDrawCurImg;
      _faceDrawMutex.unlock();

      // Note: for VIC-1873 it's possible to take a copy of the face buffer here
      //       and then pass to screen capture for converting to .tga or .gif
      //       as per the original animationStreamer version.

      //       However, it should be noted having the code here is a significant
      //       impact to performance, both visually on the robot and dropped frames
      //       in the .gif file

      // Only draw to the face once the boot anim has been stopped
      if(_displayImpl != nullptr && _stopBootAnim)
      {
        _displayImpl->FaceDraw(drawImage.GetRawDataPointer());
      }

      // Done with this image, clear the pointer
      {
        std::lock_guard<std::mutex> lock(_faceDrawMutex);
        _faceDrawCurImg = nullptr;
      }
    }
    else
    {
      _faceDrawMutex.unlock();

      if (_displayImpl == nullptr || !_stopBootAnim)
      {
        // If we haven't created the display implementation instance, or we're still
        // waiting for the boot animation to complete, sleep for a bit and then check again
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      else
      {
        // Otherwise, we wait here for a signal that a face is ready to be drawn
        std::unique_lock<std::mutex> lock{_readyMutex};
        _readyCondition.wait(lock, [this]{ return _readyFace; });
        _readyFace = false;
      }
    }
  } // End while loop

  LOG_INFO("FaceDisplay.DrawFaceLoop", "DrawFaceLoop thread is exiting");
}

void FaceDisplay::StopBootAnim()
{
  if(!_stopBootAnim)
  {
    // Have systemd stop the boot animation process, vic-bootAnim
    // Will do nothing if it is not running
    ExecCommandInBackground({"systemctl", "stop", "vic-bootAnim"},
     [this](int rc)
      {
        if(rc != 0)
        {
          LOG_WARNING("FaceDisplay.StopBootAnim.StopFailed", "%d", rc);

          // Asking nicely didn't work so try something more aggressive
          ExecCommandInBackground({"systemctl", "kill", "-s", "9", "vic-bootAnim"},
            [this](int rc)
            {
              // Killing didn't work for some reason so error and show fault code
              if(rc != 0)
              {
                LOG_ERROR("FaceDisplay.StopBootAnim.KillFailed", "%d", rc);
                FaultCode::DisplayFaultCode(FaultCode::STOP_BOOT_ANIM_FAILED);
              }
              else
              {
                _stopBootAnim = true;
              }
            });
        }
        else
        {
          _stopBootAnim = true;
        }
      });
  }
}


} // namespace Vector
} // namespace Anki
