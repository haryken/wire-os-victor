/**
 * File: sdkAudioComponent.h
 *
 * Author: Bruce von Kugelgen
 *
 * Description: Component wrapper to generate, cache and use wave data from an SDK message.
 *
 * Copyright: Anki, Inc. 2016-2019
 *
 */

#ifndef __Anki_cozmo_sdkAudioComponent_H__
#define __Anki_cozmo_sdkAudioComponent_H__

#include "clad/types/sdkAudioTypes.h"
#include "audioEngine/audioTools/standardWaveDataContainer.h"
#include "audioEngine/audioTools/streamingWaveDataInstance.h"

#include <string>
#include <vector>
#include <cstddef>
#include <functional>
#include <memory>
#include <chrono>

#include <string>

// Forward declarations
namespace Anki {
  namespace Vector {
    namespace Anim {
      class AnimContext;
    }
    namespace Audio {
      class CozmoAudioController;
    }
    namespace RobotInterface {
      struct ExternalAudioPrepare;
      struct ExternalAudioChunk;
      struct ExternalAudioComplete;
      struct ExternalAudioCancel;
    }
  }
}


namespace Anki {
namespace Vector {

class SdkAudioComponent
{
public:
  //Constructor, destructor
  SdkAudioComponent(const Anim::AnimContext* context);
  ~SdkAudioComponent();

  //
  // CLAD message handlers are called on the main thread to handle incoming requests.
  //
  void HandleMessage(const RobotInterface::ExternalAudioPrepare& msg);
  void HandleMessage(const RobotInterface::ExternalAudioChunk& msg);
  void HandleMessage(const RobotInterface::ExternalAudioComplete& msg);
  void HandleMessage(const RobotInterface::ExternalAudioCancel& msg);

  // Xiaozhi: play a raw PCM s16le mono file via the ExternalAudio path (on-robot, no SDK host).
  // Returns false if prepare/playback could not start. Large files are fed across Update() ticks
  // to avoid StreamingWavePortal buffer overflow (playback must start before the buffer fills).
  bool PlayPCMFile(const std::string& path, uint16_t sampleRate, uint16_t volumePercent);

  // Xiaozhi ESP32-style stream: Opus FIFO → decode in-anim → ExternalAudio portal.
  bool StartStreamPCM(const std::string& path, uint16_t sampleRate, uint16_t volumePercent);
  // Same entry as StartStreamPCM but path is length-prefixed Opus FIFO.
  bool StartStreamOpus(const std::string& path, uint16_t pcmSampleRate, uint16_t volumePercent);

  // Producer finished writing PCM (TTS stop); call when xiaozhi-stream-end flag appears.
  void MarkStreamEnded();

  // Continue feeding a pending PlayPCMFile / stream across anim frames (call every Update).
  void Update();

  // Xiaozhi barge-in: stop ExternalAudio immediately.
  void CancelPlayback();

  // True after ExternalAudio PostAudioEvent (speaker actually started).
  bool IsStreamSpeakerPosted() const { return _audioPosted && _pcmStreamActive; }

  // TTS_PLAN_C: AnimEngine reports whether ListeningGetOut is still streaming.
  void SetStreamGetoutActive(bool active) {
    _streamGetoutActive = active;
    if (active) {
      _streamSawGetout = true;
    }
  }

private:
  // -------------------------------------------------------------------------------------------------------------------
  // Private types
  // -------------------------------------------------------------------------------------------------------------------
  using AudioController = Vector::Audio::CozmoAudioController;
  using StreamingWaveDataPtr = std::shared_ptr<AudioEngine::StreamingWaveDataInstance>;
  using AudioCallbackType = std::function<void()>;

  // -------------------------------------------------------------------------------------------------------------------
  // Private members
  // -------------------------------------------------------------------------------------------------------------------

  AudioController * _audioController = nullptr;
  StreamingWaveDataPtr _waveData = nullptr;
  uint16_t  _audioRate;
  uint32_t  _totalAudioFramesReceived;
  bool      _audioPrepared;
  bool      _audioPosted;
  // Bumped on CancelPlayback / StartStreamPCM so a late Wwise Complete from the
  // stopped turn cannot ClearOperationData / delete PCM of the next TTS (silent
  // speaker after button barge-in).
  uint32_t  _playbackEpoch = 0;
  uint32_t  _postedEpoch = 0;
  std::shared_ptr<AudioCallbackType> _audioPlaybackFinishedPtr;

  // Pending PCM file playback (fed across Update ticks).
  std::vector<uint8_t> _pendingPcm;
  size_t _pendingPcmOffset = 0;
  bool _pcmFilePlayActive = false;

  // Growing-file / Opus-FIFO stream (ESP32-style).
  bool _pcmStreamActive = false;
  bool _pcmStreamEnded = false;
  bool _pcmStreamDoneProducing = false;
  bool _pcmStreamIsOpus = false;
  bool _pcmStreamIsFifo = false;
  std::string _pcmStreamPath;
  size_t _pcmStreamOffset = 0;
  int _pcmStreamFd = -1;
  void* _opusDecoder = nullptr; // OpusDecoder*
  std::vector<uint8_t> _opusHdrBuf;
  std::vector<uint8_t> _opusBodyBuf;
  uint16_t _opusNeed = 0;
  bool _opusHaveLen = false;

  // Early-arm silence prime (wirepod-style): count only real speech toward Post.
  bool _pcmSawSpeech = false;
  uint32_t _pcmSpeechFramesReceived = 0;

  // TTS_PLAN_C_2026-07-13: hold PostAudioEvent until wakeword getout can finish
  // (or timeout). Portal still fills from file offset 0 — play starts at head.
  bool _streamHoldForGetout = false;
  bool _streamGetoutActive = false;
  bool _streamSawGetout = false;
  // TTS_PLAN_B: continuous Animation/Behavior SFX mute until protect window ends.
  bool _streamSilencedAnim = false;
  std::chrono::steady_clock::time_point _streamHoldStart{};
  std::chrono::steady_clock::time_point _streamPostStart{};

  // -------------------------------------------------------------------------------------------------------------------
  // Private methods
  // -------------------------------------------------------------------------------------------------------------------

  // Set up Audio Engine to play text's audio data
  bool PrepareAudioEngine(const RobotInterface::ExternalAudioPrepare& msg );
  bool AddAudioChunk(const RobotInterface::ExternalAudioChunk& msg );
  bool PostAudioEvent();
  bool FeedPendingPcmChunks();
  bool FeedStreamPcm();
  void OnAudioCompleted();
  void StopActiveAudio();
  void ClearActiveAudio();
  void CleanupAudioEngine();
  void ClearOperationData();
  bool SetPlayerVolume(float volume) const;
  bool SendAnimToEngine(SDKAudioStreamingState audioState, uint32_t audioSent=0, uint32_t audioPlayed=0);
};

}   // end namespace Anki
}   // end namespace Vector

#endif // __Anki_cozmo_sdkAudioComponent_H__
