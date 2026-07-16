/**
 * File: sdkAudioComponent.cpp
 *
 * Author: Bruce von Kugelgen
 *
 * Description: Component wrapper to generate, cache and use wave data from an SDK message.
 *
 * Copyright: Anki, Inc. 2016-2019
 *
 */

#include "sdkAudioComponent.h"

#include "audioEngine/audioCallback.h"
#include "audioEngine/audioTools/standardWaveDataContainer.h"
#include "audioEngine/audioTypeTranslator.h"
#include "audioEngine/plugins/ankiPluginInterface.h"
#include "audioEngine/plugins/streamingWavePortalPlugIn.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "clad/robotInterface/messageEngineToRobot.h"
#include "cozmoAnim/animProcessMessages.h"
#include "cozmoAnim/audio/cozmoAudioController.h"
#include "cozmoAnim/animContext.h"

#include "util/logging/logging.h"
#include "util/fileUtils/fileUtils.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__linux__) || defined(ANKI_PLATFORM_VICOS) || defined(ANKI_PLATFORM_LINUX)
#include <malloc.h>
#endif

// Opus decode-in-anim was experimental; current Xiaozhi cloud decodes to PCM.
// Do not pull libopus into victor_anim link — keep stubs only.
#define ANKI_HAS_OPUS 0

// Log options
#define LOG_CHANNEL "SDKAudio"

// Point at which we declare a potential buffer overrun
#define MAX_BUFFERED_AUDIO  (100000)
// One-shot PlayPCMFile: start after a short buffer.
#define AUDIO_TO_BEGIN_PLAYING_SEC (0.2)
// Growing-file Xiaozhi stream: larger jitter buffer before speaker start (reduces underrun/OOM).
#define STREAM_AUDIO_TO_BEGIN_PLAYING_SEC (0.55f)
#define STREAM_SILENCE_PEAK_ABS (200)
#define STREAM_GETOUT_SETTLE_SEC (1.50f)
#define STREAM_PROTECT_HEAD_SEC (2.0f)
// Prefer larger portal: 28000 starved after ALSA underrun (silent TTS).
// 40000 (~2.5s) was stable for dialogue; soft-cap + PCM window still bound RAM.
#define STREAM_MAX_PORTAL_FRAMES (40000u)
// After ExternalAudio completes, trim heap again once Wwise finishes teardown.
#define IDLE_MEMORY_TRIM_DELAY_SEC (1.5f)
#define IDLE_MEMORY_TRIM_SECOND_DELAY_SEC (4.0f)

namespace {
  constexpr Anki::AudioMetaData::GameObjectType kSdkGameObject = Anki::AudioMetaData::GameObjectType::TextToSpeech;
  constexpr Anki::AudioEngine::PlugIns::StreamingWavePortalPlugIn::PluginId_t kSdkPluginId = 100;

  bool PcmChunkIsNearSilence(const uint8_t* data, size_t nbytes)
  {
    if (data == nullptr || nbytes < 2) {
      return true;
    }
    const size_t nSamples = nbytes / 2;
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    int32_t peak = 0;
    for (size_t i = 0; i < nSamples; ++i) {
      const int32_t a = std::abs(static_cast<int32_t>(samples[i]));
      if (a > peak) {
        peak = a;
      }
    }
    return peak < STREAM_SILENCE_PEAK_ABS;
  }
}

namespace Anki {
namespace Vector {

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Constructor
//
SdkAudioComponent::SdkAudioComponent(const Anim::AnimContext* context) {
  DEV_ASSERT(nullptr != context, "SdkAudioComponent.InvalidContext");
  DEV_ASSERT(nullptr != context->GetAudioController(), "SdkAudioComponent.InvalidAudioController");

  _audioController = context->GetAudioController();
  _audioPrepared = false;
  _audioPosted = false;
  _audioPlaybackFinishedPtr = std::make_shared<AudioCallbackType>([this](){
    OnAudioCompleted();
  });
}

//
// Destructor
//
SdkAudioComponent::~SdkAudioComponent() {
}

//
// Called to handle incoming ExternalAudioComplete message when client signals end of streaming
//
void SdkAudioComponent::HandleMessage(const RobotInterface::ExternalAudioComplete& msg)
{
  if (ANKI_VERIFY(_audioPrepared, 
                    "SdkAudioComponent.HandleMessage.ExternalAudioComplete", 
                    "Audio stream complete message received without start")) {
    _waveData->DoneProducingData();      
  }
}

//
// Called to handle incoming ExternalAudioCancel message for user cancellation
//
void SdkAudioComponent::HandleMessage(const RobotInterface::ExternalAudioCancel& msg)
{
  if (ANKI_VERIFY(_audioPrepared, 
                    "SdkAudioComponent.HandleMessage.ExternalAudioCancel", 
                    "Audio stream cancel message received without start")) {
    CleanupAudioEngine();
    SendAnimToEngine(SDKAudioStreamingState::Cancelled);
  }
}

//
// Called to handle incoming ExternalAudioPrepare messages to start audio streaming process
//
void SdkAudioComponent::HandleMessage(const RobotInterface::ExternalAudioPrepare& msg)
{
  LOG_DEBUG("SdkAudioComponent.HandleMessage.ExternalAudioPrepare", 
           "Sample rate %d, volume %d", msg.audio_volume, msg.audio_rate );

  if (!PrepareAudioEngine(msg)) {
    LOG_DEBUG("SdkAudioComponent.HandleMessage.ExternalAudioPrepare", "Unable to prepare audio engine for streaming");
    SendAnimToEngine(SDKAudioStreamingState::PrepareFailed);
    ClearOperationData();
    return;
  }
}

//
// Called to handle incoming ExternalAudioChunk message for streaming audio data chunks
//
void SdkAudioComponent::HandleMessage(const RobotInterface::ExternalAudioChunk& msg)
{
  if (!_audioPrepared) {
    LOG_DEBUG("SdkAudioComponent.HandleMessage.ExternalAudioChunk", "Dropping chunks due to cancellation");
    return;
  }

  //add our wave data to the playing instance
  if (!AddAudioChunk(msg)) {
    CleanupAudioEngine(); 
    return;
  }

  const float totalAudioReceived_s = _totalAudioFramesReceived / (float)_audioRate;
  LOG_DEBUG("SdkAudioComponent.HandleMessage.ExternalAudioChunk", "Received (sec) %f", totalAudioReceived_s);
  if (!_audioPosted && totalAudioReceived_s > AUDIO_TO_BEGIN_PLAYING_SEC) {
      LOG_DEBUG("SdkAudioComponent.HandleMessage.ExternalAudioChunk", "Starting playback");

    // Post initial audio event for streaming
    if (!PostAudioEvent()) {
      LOG_ERROR("SdkAudioComponent.HandleMessage.ExternalAudioChunk", "Unable to post audio event Audio Streaming");
      SendAnimToEngine(SDKAudioStreamingState::PostFailed);
      CleanupAudioEngine();
      return;
    }
    _audioPosted = true;
  }
} // HandleMessage()

//
// Set volume for audio stream playback
// Value expected between 0.0 and 1.0 inclusive
//
bool SdkAudioComponent::SetPlayerVolume(float volume) const
{
  if (!ANKI_VERIFY(volume <= 1.0f && volume >= 0.0f, 
      "SdkAudioComponent.SetPlayerVolume","InvalidVolumeLevel should be between 0.0 and 1.0 inclusive")) {
    return false;
  }
  const auto parameterId = 
    AudioEngine::ToAudioParameterId( AudioMetaData::GameParameter::ParameterType::Robot_Vic_Sdk_Volume );
  const auto parameterValue = AudioEngine::ToAudioRTPCValue( volume );
  _audioController->SetParameter( parameterId, parameterValue, AudioEngine::kInvalidAudioGameObject );
  return true;
}

//
// Prepare for audio playback
//
bool SdkAudioComponent::PrepareAudioEngine(const RobotInterface::ExternalAudioPrepare& msg )
{
  if (_audioPrepared) {
    //no reentrance, only one playing at a time
    LOG_ERROR("SdkAudioComponent::PrepareAudioEngine", "Already playing audio");
    return false;
  }
  if (!SetPlayerVolume(msg.audio_volume / 100.0f)) {
    return false;
  }
  if (!ANKI_VERIFY(msg.audio_rate >= 8000 && msg.audio_rate <= 16025,
                   "SdkAudioComponent::PrepareAudioEngine", "Invalid audio rate %u", msg.audio_rate)) {
    return false;
  }

  // new waveData instance to hold data passed to wwise
  _waveData = AudioEngine::PlugIns::StreamingWavePortalPlugIn::CreateDataInstance();

  // Clear previously loaded data, setup plugin
  auto * pluginInterface = _audioController->GetPluginInterface();
  DEV_ASSERT(nullptr != pluginInterface, "SdkAudioComponent.PrepareAudioEngine.InvalidPluginInterface");
  auto * plugin = pluginInterface->GetStreamingWavePortalPlugIn();
  plugin->ClearAudioData(kSdkPluginId);
  bool result = plugin->AddDataInstance(_waveData, kSdkPluginId);
  if (!result) {
    LOG_ERROR("SdkAudioComponent::PrepareAudioEngine", "Failed to add data instance");
    return false;
  }

  _audioRate = msg.audio_rate;
  _totalAudioFramesReceived = 0;
  _audioPrepared = true;

  return true;
}

//
// Add a chunk of audio to the audio engine
//
bool SdkAudioComponent::AddAudioChunk(const RobotInterface::ExternalAudioChunk& msg) {
  //check for dangerous buffer expansion
  auto * pluginInterface = _audioController->GetPluginInterface();
  auto * plugin = pluginInterface->GetStreamingWavePortalPlugIn();
  const auto played = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesPlayed();
  const auto received = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesReceived();
  LOG_DEBUG("SdkAudioComponent::AddAudioChunk", "Played %u Received %u", played, received);
  SendAnimToEngine(SDKAudioStreamingState::ChunkAdded, received, played);

  if (received - played > MAX_BUFFERED_AUDIO) {
    LOG_ERROR("SdkAudioComponent::AddAudioChunk", "Buffer overflow %u played %u received", played, received);
    SendAnimToEngine(SDKAudioStreamingState::BufferOverflow, received, played);
    return false; 
  }

  //copy chunk into container, append to waveData
  const uint16_t input_chunk_size = msg.audio_chunk_size;
  const uint16_t wave_data_size = input_chunk_size / 2;  //size is in bytes, but buffer samples are 16 bit
  Anki::AudioEngine::StandardWaveDataContainer waveContainer(_audioRate, 1, wave_data_size); 
  waveContainer.CopyLittleEndianWaveData( (const unsigned char*)msg.audio_chunk_data, wave_data_size );
  const bool result = _waveData->AppendStandardWaveData( std::move(waveContainer ));

  if (!result) {
    LOG_ERROR("SdkAudioComponent::AddAudioChunk", "Failed to append audio data");
    SendAnimToEngine(SDKAudioStreamingState::AddAudioFailed);
    return false;
  } 

  _totalAudioFramesReceived = received + wave_data_size;

  return true;
}

//
// Send audio trigger event
//
bool SdkAudioComponent::PostAudioEvent()
{
  // Clear getout/SFX once more right before speaker start (after settle).
  {
    using GO = Anki::AudioMetaData::GameObjectType;
    _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Animation));
    _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Behavior));
  }

  auto * audioCallbackContext = new AudioEngine::AudioCallbackContext();

  // Set callback flags
  audioCallbackContext->SetCallbackFlags( AudioEngine::AudioCallbackFlag::Complete );
  // Execute callbacks synchronously (on main thread)
  audioCallbackContext->SetExecuteAsync( false );
  // Register callback for event
  using namespace AudioEngine;
  std::weak_ptr<AudioCallbackType> weakOnFinished = _audioPlaybackFinishedPtr;
  audioCallbackContext->SetEventCallbackFunc( [weakOnFinished]( const AudioCallbackContext* thisContext,
                                                                     const AudioCallbackInfo& callbackInfo ) {
    // if the user logs out, this callback could fire after sdkAudioComponent is destroyed. check that here.
    auto callback = weakOnFinished.lock();
    if( callback && (*callback) ) {
      (*callback)();
    }
  });

  using AudioEvent = AudioMetaData::GameEvent::GenericEvent;
  const auto eventID = AudioEngine::ToAudioEventId( AudioEvent::Play__Robot_Vic__External_Sdk_Playback_01 );
  const auto gameObject = static_cast<AudioEngine::AudioGameObject>( kSdkGameObject );
  // Arm epoch before Post — Complete can run synchronously on stop/underrun.
  _postedEpoch = _playbackEpoch;
  const auto playingID = _audioController->PostAudioEvent(eventID, gameObject, audioCallbackContext);

  if (AudioEngine::kInvalidAudioPlayingId == playingID) {
    LOG_ERROR("SdkAudioComponent.PostAudioEvent", "Failed to post eventID %u", eventID);
    Util::FileUtils::WriteFile("/run/vic-cloud/xiaozhi-dbg", "post_fail\n");
    return false;
  }
  _streamPostStart = std::chrono::steady_clock::now();
  // INFO so journalctl -u vic-anim shows when the speaker actually starts (Xiaozhi ExternalAudio).
  LOG_INFO("SdkAudioComponent.PostAudioEvent", "Speaker start: posted eventID %u playingID %d epoch %u (Plan B protect %.0fms)",
           eventID, playingID, _postedEpoch, STREAM_PROTECT_HEAD_SEC * 1000.f);
  Util::FileUtils::WriteFile("/run/vic-cloud/xiaozhi-dbg", "post_ok\n");
  return true;
}

//
// Cleanup the audio engine in cases of error or cancellation
//
void SdkAudioComponent::CleanupAudioEngine()
{
  LOG_DEBUG("SdkAudioComponent.CleanupAudioEngine", "Clean up Audio Engine");
  StopActiveAudio();
  ClearActiveAudio();
  ClearOperationData();
  // Return pages now; deferred trim is scheduled by callers that fully end playback
  // (OnAudioCompleted / CancelPlayback). Mid-stream replace via StartStreamPCM should
  // not arm idle trim — next Prepare is immediate.
#if defined(__linux__) || defined(ANKI_PLATFORM_VICOS) || defined(ANKI_PLATFORM_LINUX)
  malloc_trim(0);
#endif
  // Do NOT clear xiaozhi-busy here — StartStreamPCM calls Cleanup when replacing
  // a prior stream and would drop the busy gate while cloud still expects playback.
  // Busy is cleared only in OnAudioCompleted / CancelPlayback.
}

//
// Tells wave output we are done making data
//
void SdkAudioComponent::ClearOperationData()
{
  LOG_DEBUG("SdkAudioComponent.ClearOperationData", "Clear Sdk Audio");
  if (_waveData && _waveData->IsPlayingStream()) {
    //no more data coming
    _waveData->DoneProducingData();
  }

  _audioPrepared = false;
  _audioPosted = false;
  _waveData = nullptr;
  _pcmFilePlayActive = false;
  // clear() keeps capacity — swap so large oneshot PCM buffers return to the allocator.
  std::vector<uint8_t>().swap(_pendingPcm);
  _pendingPcmOffset = 0;
  _pcmStreamActive = false;
  _pcmStreamEnded = false;
  _pcmStreamDoneProducing = false;
  _pcmStreamIsOpus = false;
  _pcmStreamIsFifo = false;
  std::string().swap(_pcmStreamPath);
  _pcmStreamOffset = 0;
  if (_pcmStreamFd >= 0) {
    close(_pcmStreamFd);
    _pcmStreamFd = -1;
  }
  if (_opusDecoder != nullptr) {
#if ANKI_HAS_OPUS
    opus_decoder_destroy(static_cast<OpusDecoder*>(_opusDecoder));
#endif
    _opusDecoder = nullptr;
  }
  std::vector<uint8_t>().swap(_opusHdrBuf);
  std::vector<uint8_t>().swap(_opusBodyBuf);
  _opusNeed = 0;
  _opusHaveLen = false;
  _streamHoldForGetout = false;
  _streamGetoutActive = false;
  _streamSawGetout = false;
  _streamSilencedAnim = false;
  _pcmSawSpeech = false;
  _pcmSpeechFramesReceived = 0;
  // Do NOT unlink the Opus FIFO here — cloud may still be writing. Unlink only after
  // OnAudioCompleted / CancelPlayback once both ends are done.
}

void SdkAudioComponent::ReleaseIdleAudioMemory()
{
  // Drop any leftover portal shared_ptr (Complete path used to skip ClearActiveAudio).
  ClearActiveAudio();
#if defined(__linux__) || defined(ANKI_PLATFORM_VICOS) || defined(ANKI_PLATFORM_LINUX)
  // ExternalAudio / float portal chunks free into glibc but RSS stays at high-water
  // until trim — that sticky RSS is what LMK sees after Xiaozhi goes idle.
  malloc_trim(0);
#endif
  ScheduleIdleMemoryTrim();
}

void SdkAudioComponent::ScheduleIdleMemoryTrim()
{
  _idleTrimPending = true;
  _idleTrimPass = 0;
  _idleTrimAt = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<float>(IDLE_MEMORY_TRIM_DELAY_SEC));
}

void SdkAudioComponent::MaybeIdleMemoryTrim()
{
  if (!_idleTrimPending) {
    return;
  }
  // Do not trim while a new stream is already armed — would race Prepare.
  if (_audioPrepared || _pcmFilePlayActive || _pcmStreamActive) {
    _idleTrimPending = false;
    return;
  }
  if (std::chrono::steady_clock::now() < _idleTrimAt) {
    return;
  }
#if defined(__linux__) || defined(ANKI_PLATFORM_VICOS) || defined(ANKI_PLATFORM_LINUX)
  ClearActiveAudio();
  malloc_trim(0);
#endif
  ++_idleTrimPass;
  if (_idleTrimPass < 2) {
    _idleTrimAt = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(IDLE_MEMORY_TRIM_SECOND_DELAY_SEC));
    LOG_INFO("SdkAudioComponent.MaybeIdleMemoryTrim",
             "Deferred heap trim pass %u (Xiaozhi idle)", _idleTrimPass);
  } else {
    _idleTrimPending = false;
    LOG_INFO("SdkAudioComponent.MaybeIdleMemoryTrim",
             "Deferred heap trim done (Xiaozhi idle)");
  }
} 

//
// Cancel audio events
//
void SdkAudioComponent::StopActiveAudio()
{
  LOG_DEBUG("SdkAudioComponent.StopActiveAudio", "Stop active Sdk audio");
  using AudioEvent = AudioMetaData::GameEvent::GenericEvent;
  const auto eventID = AudioEngine::ToAudioEventId( AudioEvent::Stop__Robot_Vic__External_Sdk_Playback_01 );
  _audioController->StopAllAudioEvents(eventID);
}

//
// Clear data from currently playing audio
//
void SdkAudioComponent::ClearActiveAudio()
{
  LOG_DEBUG("SdkAudioComponent.ClearActiveAudio", "Clear active Sdk audio");
  auto * plugin = _audioController->GetPluginInterface()->GetStreamingWavePortalPlugIn();
  if (plugin != nullptr) {
    plugin->ClearAudioData(kSdkPluginId);
  }
}

//
// Handle a callback from the AudioEngine indicating that the SDK Audio has finished sending audio
//
void SdkAudioComponent::OnAudioCompleted()
{
  // Stop/Cancel bumps _playbackEpoch before Wwise may deliver Complete. A stale
  // callback must not tear down the next turn's ExternalAudio (barge-in → silent TTS).
  if (_postedEpoch != _playbackEpoch) {
    LOG_INFO("SdkAudioComponent.OnAudioCompleted",
             "Ignoring stale completion (posted=%u epoch=%u)",
             _postedEpoch, _playbackEpoch);
    return;
  }
  ++_playbackEpoch;
  LOG_DEBUG("SdkAudioComponent.OnAudioCompleted", "AudioStreaming completion callback received");
  // Full teardown (portal + vectors), then return pages to OS — peak during play is OK,
  // sticky RSS after idle was the LMK/fault-800 trigger.
  ClearActiveAudio();
  ClearOperationData();
  ReleaseIdleAudioMemory();
  SendAnimToEngine(SDKAudioStreamingState::Completed, 0, 0);

  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
  Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
  // Do NOT delete xiaozhi_play.pcm here — cloud may still be appending / next turn
  // may already own the file (stale Complete after barge-in caused silent TTS).
  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi_play.opus");

  // Xiaozhi continuous: if a pending relisten was requested, arm FakeTriggerWord via flag.
  static const char* kPending = "/run/vic-cloud/xiaozhi-relisten-pending";
  static const char* kPendingLegacy = "/run/xiaozhi-relisten-pending";
  static const char* kRelisten = "/run/vic-cloud/xiaozhi-relisten";
  static const char* kRelistenLegacy = "/run/xiaozhi-relisten";
  const char* pending = nullptr;
  if (access(kPending, F_OK) == 0) {
    pending = kPending;
  } else if (access(kPendingLegacy, F_OK) == 0) {
    pending = kPendingLegacy;
  }
  if (pending != nullptr) {
    unlink(pending);
    // Prefer /run/vic-cloud; also write legacy path for older micDataSystem builds.
    Util::FileUtils::WriteFile(kRelisten, "1");
    Util::FileUtils::WriteFile(kRelistenLegacy, "1");
    LOG_INFO("SdkAudioComponent.OnAudioCompleted.XiaozhiRelisten", "Armed continuous relisten after TTS");
  }
}

bool SdkAudioComponent::PlayPCMFile(const std::string& path, uint16_t sampleRate, uint16_t volumePercent)
{
  if (sampleRate < 8000 || sampleRate > 16025) {
    LOG_ERROR("SdkAudioComponent.PlayPCMFile", "Invalid sample rate %u", sampleRate);
    return false;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    LOG_ERROR("SdkAudioComponent.PlayPCMFile", "Unable to open %s", path.c_str());
    return false;
  }
  std::vector<uint8_t> pcm((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (pcm.empty() || (pcm.size() % 2) != 0) {
    LOG_ERROR("SdkAudioComponent.PlayPCMFile", "Invalid PCM size %zu", pcm.size());
    return false;
  }

  // Cancel any in-flight file play so we don't stack streams.
  if (_audioPrepared || _pcmFilePlayActive) {
    CleanupAudioEngine();
  }

  RobotInterface::ExternalAudioPrepare prep;
  prep.audio_volume = volumePercent;
  prep.audio_rate = sampleRate;
  if (!PrepareAudioEngine(prep)) {
    return false;
  }

  _pendingPcm = std::move(pcm);
  _pendingPcmOffset = 0;
  _pcmFilePlayActive = true;

  LOG_INFO("SdkAudioComponent.PlayPCMFile", "Queued %zu bytes @ %u Hz from %s",
           _pendingPcm.size(), sampleRate, path.c_str());

  // Feed as much as fits this frame; remainder continues in Update().
  FeedPendingPcmChunks();
  return true;
}

void SdkAudioComponent::Update()
{
  if (_pcmStreamActive) {
    // TTS_PLAN_B_MUTE_SFX_2026-07-13: mute Animation/Behavior SFX for the whole
    // hold + ~2s after Post so ListeningGetOut audio cannot duck TTS head.
    // Face/getout anim can still run; only audio events are stopped.
    if (_audioController != nullptr) {
      bool protectHead = !_audioPosted;
      if (_audioPosted) {
        const float sincePost = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - _streamPostStart).count();
        protectHead = sincePost < STREAM_PROTECT_HEAD_SEC;
      }
      if (protectHead) {
        using GO = Anki::AudioMetaData::GameObjectType;
        _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Animation));
        _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Behavior));
        if (!_streamSilencedAnim) {
          _streamSilencedAnim = true;
          LOG_INFO("SdkAudioComponent.Update",
                   "Plan B: muting Animation/Behavior SFX during TTS head protect");
        }
      }
    }
    FeedStreamPcm();
  } else if (_pcmFilePlayActive) {
    FeedPendingPcmChunks();
  } else {
    MaybeIdleMemoryTrim();
  }
}

void SdkAudioComponent::CancelPlayback()
{
  if (!_audioPrepared && !_pcmFilePlayActive && !_pcmStreamActive) {
    // Still clear busy marker if present.
    Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
    Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
    return;
  }
  LOG_INFO("SdkAudioComponent.CancelPlayback", "Stopping ExternalAudio (Xiaozhi barge-in)");
  CleanupAudioEngine();
  ReleaseIdleAudioMemory();
  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
  Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-relisten-pending");
  Util::FileUtils::DeleteFile("/run/xiaozhi-relisten-pending");
  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-stream");
  Util::FileUtils::DeleteFile("/run/xiaozhi-stream");
  Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-stream-end");
  Util::FileUtils::DeleteFile("/run/xiaozhi-stream-end");
}

bool SdkAudioComponent::StartStreamOpus(const std::string& path, uint16_t pcmSampleRate, uint16_t volumePercent)
{
  // Opus-in-anim disabled (no libopus on anim link). Cloud sends decoded PCM.
  LOG_WARNING("SdkAudioComponent.StartStreamOpus", "Opus stream not supported; use PCM path (%s)", path.c_str());
  (void)pcmSampleRate;
  (void)volumePercent;
  return false;
}

bool SdkAudioComponent::StartStreamPCM(const std::string& path, uint16_t sampleRate, uint16_t volumePercent)
{
  if (sampleRate < 8000 || sampleRate > 16025) {
    LOG_ERROR("SdkAudioComponent.StartStreamPCM", "Invalid sample rate %u", sampleRate);
    return false;
  }

  if (_audioPrepared || _pcmFilePlayActive || _pcmStreamActive) {
    CleanupAudioEngine();
  }

  // TTS_HEAD_FIX_2026-07-13 (anim): wakeword getout / anim SFX share the speaker with
  // ExternalAudio and duck/eat the first Xiaozhi TTS chunk. Stop competing voices first.
  {
    using GO = Anki::AudioMetaData::GameObjectType;
    _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Animation));
    _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::TextToSpeech));
    _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Behavior));
    LOG_INFO("SdkAudioComponent.StartStreamPCM", "Stopped Animation/TTS/Behavior audio before Xiaozhi stream");
  }

  RobotInterface::ExternalAudioPrepare prep;
  prep.audio_volume = volumePercent;
  prep.audio_rate = sampleRate;
  if (!PrepareAudioEngine(prep)) {
    return false;
  }

  _pcmStreamPath = path;
  _pcmStreamOffset = 0;
  _pcmStreamEnded = false;
  _pcmStreamDoneProducing = false;
  _pcmStreamActive = true;
  _pcmFilePlayActive = false;
  _pcmSawSpeech = false;
  _pcmSpeechFramesReceived = 0;
  _streamHoldForGetout = true;
  _streamHoldStart = std::chrono::steady_clock::now();
  _streamSawGetout = false;
  _streamGetoutActive = false;
  _streamSilencedAnim = false;
  _pendingPcm.clear();
  _pendingPcmOffset = 0;
  if (_pcmStreamFd >= 0) {
    close(_pcmStreamFd);
    _pcmStreamFd = -1;
  }

  struct stat st;
  _pcmStreamIsFifo = (stat(path.c_str(), &st) == 0) && S_ISFIFO(st.st_mode);
  // Open read end ASAP so cloud's O_WRONLY open can succeed (FIFO backpressure path).
  _pcmStreamFd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (_pcmStreamFd < 0) {
    LOG_ERROR("SdkAudioComponent.StartStreamPCM", "Unable to open %s errno=%d", path.c_str(), errno);
    CleanupAudioEngine();
    return false;
  }

  LOG_INFO("SdkAudioComponent.StartStreamPCM", "Streaming from %s @ %u Hz (fifo=%d)",
           path.c_str(), sampleRate, _pcmStreamIsFifo ? 1 : 0);
  return true;
}

void SdkAudioComponent::MarkStreamEnded()
{
  if (_pcmStreamActive) {
    _pcmStreamEnded = true;
    LOG_INFO("SdkAudioComponent.MarkStreamEnded", "Stream producer done at offset %zu", _pcmStreamOffset);
  }
}

bool SdkAudioComponent::FeedStreamPcm()
{
  if (!_pcmStreamActive || !_audioPrepared || !_waveData) {
    return false;
  }

  auto * pluginInterface = _audioController->GetPluginInterface();
  if (pluginInterface == nullptr) {
    return false;
  }
  auto * plugin = pluginInterface->GetStreamingWavePortalPlugIn();
  if (plugin == nullptr || plugin->GetDataInstance(kSdkPluginId) == nullptr) {
    return false;
  }

  constexpr size_t kMaxChunk = 1024;
  constexpr uint32_t kMinPortalFramesWhileStreaming = 3200;

  auto tryStartSpeaker = [this]() -> bool {
    if (_audioPosted) {
      return true;
    }
    // TTS_PLAN_C: buffer into portal while getout plays; Post after settle.
    // Cloud releases listen UI at STT, so getout often finished BEFORE first TTS frame.
    // In that case _streamSawGetout stays false — open after a short grace (not full 1.5s).
    // Still wait out a late getout if we see one start (intent→engine lag).
    if (_streamHoldForGetout) {
      const auto elapsed = std::chrono::duration<float>(
          std::chrono::steady_clock::now() - _streamHoldStart).count();
      const bool getoutFinished = _streamSawGetout && !_streamGetoutActive && elapsed >= 0.35f;
      // Getout already done before stream (common): do not wait the full settle timeout.
      const bool alreadyDoneBeforeStream = !_streamSawGetout && elapsed >= 0.45f;
      const bool timedOut = elapsed >= STREAM_GETOUT_SETTLE_SEC;
      if (!getoutFinished && !alreadyDoneBeforeStream && !timedOut) {
        return true;
      }
      {
        using GO = Anki::AudioMetaData::GameObjectType;
        _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Animation));
        _audioController->StopAllAudioEvents(static_cast<AudioEngine::AudioGameObject>(GO::Behavior));
      }
      _streamHoldForGetout = false;
      LOG_INFO("SdkAudioComponent.FeedStreamPcm",
               "Getout settle done (%.0fms, saw=%d active=%d early=%d) — arming ExternalAudio",
               elapsed * 1000.f, _streamSawGetout ? 1 : 0, _streamGetoutActive ? 1 : 0,
               alreadyDoneBeforeStream ? 1 : 0);
    }
    // Silence prime (zeros) warms the portal but must not count toward Post —
    // otherwise we open the speaker during getout and eat the first real syllables.
    // Exception: short replies (< ~0.55s speech) used to never Post → busy stuck + silence.
    // When the producer already ended, post as soon as we have any real speech (or a tiny clip).
    const float speechReceived_s = _pcmSpeechFramesReceived / (float)_audioRate;
    const bool enoughBufferedSpeech =
        _pcmSawSpeech && speechReceived_s > STREAM_AUDIO_TO_BEGIN_PLAYING_SEC;
    const bool shortReplyReady =
        _pcmStreamEnded && _pcmSawSpeech && _pcmSpeechFramesReceived > 0;
    // Producer ended with only a blip after silence-prime — still open speaker.
    const bool endedWithAnyPcm =
        _pcmStreamEnded && _totalAudioFramesReceived > (_audioRate / 10); // >100ms
    if (!enoughBufferedSpeech && !shortReplyReady && !endedWithAnyPcm) {
      return true;
    }
    if (!PostAudioEvent()) {
      LOG_ERROR("SdkAudioComponent.FeedStreamPcm", "Unable to post audio event");
      CleanupAudioEngine();
      return false;
    }
    _audioPosted = true;
    LOG_INFO("SdkAudioComponent.FeedStreamPcm",
             "Speaker started after settle + ~%.0fms real speech (speechFrames=%u totalFrames=%u ended=%d)",
             STREAM_AUDIO_TO_BEGIN_PLAYING_SEC * 1000.f,
             _pcmSpeechFramesReceived, _totalAudioFramesReceived, _pcmStreamEnded ? 1 : 0);
    return true;
  };

  // Prefer FIFO read (bounded kernel buffer). Fall back to growing-file seek for one-shot leftovers.
  if (_pcmStreamFd >= 0) {
    for (;;) {
      const auto played = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesPlayed();
      const auto received = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesReceived();
      if (received - played > STREAM_MAX_PORTAL_FRAMES) {
        if (!tryStartSpeaker()) {
          return false;
        }
        break; // backpressure: leave bytes in the FIFO
      }

      uint8_t buf[2048];
      const ssize_t nread = read(_pcmStreamFd, buf, sizeof(buf));
      if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        LOG_ERROR("SdkAudioComponent.FeedStreamPcm", "FIFO read errno=%d", errno);
        CleanupAudioEngine();
        return false;
      }
      if (nread == 0) {
        // EOF: writer closed after StreamEnd.
        if (_pcmStreamEnded && !_pcmStreamDoneProducing) {
          if (!_audioPosted) {
            if (_totalAudioFramesReceived == 0) {
              CleanupAudioEngine();
              Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
              Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
              return false;
            }
            if (!tryStartSpeaker()) {
              return false;
            }
          }
          _waveData->DoneProducingData();
          _pcmStreamDoneProducing = true;
          _pcmStreamActive = false;
          if (_pcmStreamFd >= 0) {
            close(_pcmStreamFd);
            _pcmStreamFd = -1;
          }
          LOG_INFO("SdkAudioComponent.FeedStreamPcm", "Finished FIFO stream %zu bytes PCM",
                   _pcmStreamOffset);
        }
        break;
      }

      size_t got = static_cast<size_t>(nread);
      if ((got % 2) != 0) {
        got -= 1;
      }
      size_t fed = 0;
      auto feedOne = [&](size_t n) -> bool {
        RobotInterface::ExternalAudioChunk chunkMsg;
        chunkMsg.audio_chunk_size = static_cast<uint16_t>(n);
        std::memcpy(chunkMsg.audio_chunk_data, buf + fed, n);
        // Growing regular PCM files also take this fd path (not only FIFOs).
        // Without speech detection, tryStartSpeaker never Posts → silent TTS + busy stuck.
        const bool nearSilence = PcmChunkIsNearSilence(buf + fed, n);
        if (!nearSilence) {
          _pcmSawSpeech = true;
          _pcmSpeechFramesReceived += static_cast<uint32_t>(n / 2);
        }
        if (!AddAudioChunk(chunkMsg)) {
          CleanupAudioEngine();
          return false;
        }
        fed += n;
        _pcmStreamOffset += n;
        return tryStartSpeaker();
      };
      while (fed < got) {
        const auto played2 = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesPlayed();
        const auto received2 = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesReceived();
        if (received2 - played2 > STREAM_MAX_PORTAL_FRAMES) {
          if (!tryStartSpeaker()) {
            return false;
          }
          // Portal full mid-buffer: we already pulled from FIFO — feed what we can next ticks
          // by keeping a small pending? Avoid complexity: feed remaining into portal anyway
          // up to one extra chunk then stop (portal check above prevents large overrun).
          break;
        }
        const size_t n = std::min(kMaxChunk, got - fed);
        if (!feedOne(n)) {
          return false;
        }
      }
      // If we couldn't feed the whole read, push remainder as chunks anyway (≤2KB).
      while (fed < got) {
        const size_t n = std::min(kMaxChunk, got - fed);
        if (!feedOne(n)) {
          return false;
        }
      }
      break; // one read per Update
    }
    return true;
  }

  // Legacy growing-file path (one-shot leftover / non-FIFO).
  const ssize_t fileSize = Util::FileUtils::GetFileSize(_pcmStreamPath);
  if (fileSize < 0) {
    if (_pcmStreamEnded && !_pcmStreamDoneProducing) {
      if (!_audioPosted) {
        CleanupAudioEngine();
        Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
        Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
        return false;
      }
      _waveData->DoneProducingData();
      _pcmStreamDoneProducing = true;
      _pcmStreamActive = false;
      LOG_INFO("SdkAudioComponent.FeedStreamPcm", "Done producing (no file)");
    }
    return false;
  }

  size_t available = static_cast<size_t>(fileSize);
  if (available < _pcmStreamOffset) {
    _pcmStreamOffset = 0;
  }
  size_t unread = available - _pcmStreamOffset;
  if ((unread % 2) != 0) {
    unread -= 1;
  }

  if (unread > 0) {
    std::ifstream in(_pcmStreamPath, std::ios::binary);
    if (!in) {
      LOG_ERROR("SdkAudioComponent.FeedStreamPcm", "Unable to open %s", _pcmStreamPath.c_str());
      CleanupAudioEngine();
      return false;
    }
    in.seekg(static_cast<std::streamoff>(_pcmStreamOffset));
    std::vector<uint8_t> buf(std::min(unread, size_t{4096}));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const size_t got = static_cast<size_t>(in.gcount());
    size_t fed = 0;
    while (fed < got) {
      const auto played = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesPlayed();
      const auto received = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesReceived();
      if (received - played > STREAM_MAX_PORTAL_FRAMES) {
        if (!tryStartSpeaker()) {
          return false;
        }
        break;
      }
      const size_t n = std::min(kMaxChunk, got - fed);
      RobotInterface::ExternalAudioChunk chunkMsg;
      chunkMsg.audio_chunk_size = static_cast<uint16_t>(n);
      std::memcpy(chunkMsg.audio_chunk_data, buf.data() + fed, n);
      const bool nearSilence = PcmChunkIsNearSilence(buf.data() + fed, n);
      if (!nearSilence) {
        _pcmSawSpeech = true;
        _pcmSpeechFramesReceived += static_cast<uint32_t>(n / 2);
      }
      if (!AddAudioChunk(chunkMsg)) {
        CleanupAudioEngine();
        return false;
      }
      fed += n;
      _pcmStreamOffset += n;
      if (!tryStartSpeaker()) {
        return false;
      }
    }
  } else if (!_audioPosted) {
    // No new bytes this tick (gap between cloud appends) — still advance hold / Post.
    if (!tryStartSpeaker()) {
      return false;
    }
  }

  if (!_pcmStreamEnded) {
    return true;
  }

  const size_t evenEnd = (static_cast<size_t>(fileSize) / 2) * 2;
  if (_pcmStreamOffset >= evenEnd && !_pcmStreamDoneProducing) {
    if (!_audioPosted) {
      if (_totalAudioFramesReceived == 0) {
        CleanupAudioEngine();
        Util::FileUtils::DeleteFile("/run/vic-cloud/xiaozhi-busy");
        Util::FileUtils::DeleteFile("/run/xiaozhi-busy");
        return false;
      }
      if (!tryStartSpeaker()) {
        return false;
      }
    }
    _waveData->DoneProducingData();
    _pcmStreamDoneProducing = true;
    _pcmStreamActive = false;
    LOG_INFO("SdkAudioComponent.FeedStreamPcm", "Finished streaming %zu bytes PCM (portal buffered)",
             _pcmStreamOffset);
    (void)kMinPortalFramesWhileStreaming;
  }

  return true;
}

bool SdkAudioComponent::FeedPendingPcmChunks()
{
  if (!_pcmFilePlayActive || !_audioPrepared || !_waveData) {
    return false;
  }

  auto * pluginInterface = _audioController->GetPluginInterface();
  if (pluginInterface == nullptr) {
    return false;
  }
  auto * plugin = pluginInterface->GetStreamingWavePortalPlugIn();
  if (plugin == nullptr || plugin->GetDataInstance(kSdkPluginId) == nullptr) {
    return false;
  }

  constexpr size_t kMaxChunk = 1024;
  // Leave headroom under MAX_BUFFERED_AUDIO so AddAudioChunk does not trip overflow.
  constexpr uint32_t kBufferHeadroomFrames = 8000;

  while (_pendingPcmOffset < _pendingPcm.size()) {
    const auto played = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesPlayed();
    const auto received = plugin->GetDataInstance(kSdkPluginId)->GetNumberOfFramesReceived();
    if (received - played > MAX_BUFFERED_AUDIO - kBufferHeadroomFrames) {
      // Playback must drain before we enqueue more; continue next Update tick.
      if (!_audioPosted) {
        // Should not happen with headroom after 0.2s post — force-start if needed.
        if (!PostAudioEvent()) {
          LOG_ERROR("SdkAudioComponent.FeedPendingPcmChunks", "Unable to post audio event");
          CleanupAudioEngine();
          return false;
        }
        _audioPosted = true;
      }
      return true;
    }

    const size_t n = std::min(kMaxChunk, _pendingPcm.size() - _pendingPcmOffset);
    RobotInterface::ExternalAudioChunk chunkMsg;
    chunkMsg.audio_chunk_size = static_cast<uint16_t>(n);
    std::memcpy(chunkMsg.audio_chunk_data, _pendingPcm.data() + _pendingPcmOffset, n);
    if (!AddAudioChunk(chunkMsg)) {
      CleanupAudioEngine();
      return false;
    }
    _pendingPcmOffset += n;

    // Match SDK streaming path: start playback after a short buffer, before the portal fills.
    if (!_audioPosted) {
      const float totalAudioReceived_s = _totalAudioFramesReceived / (float)_audioRate;
      if (totalAudioReceived_s > AUDIO_TO_BEGIN_PLAYING_SEC) {
        if (!PostAudioEvent()) {
          LOG_ERROR("SdkAudioComponent.FeedPendingPcmChunks", "Unable to post audio event");
          CleanupAudioEngine();
          return false;
        }
        _audioPosted = true;
      }
    }
  }

  // Entire file queued.
  if (!_audioPosted) {
    if (!PostAudioEvent()) {
      LOG_ERROR("SdkAudioComponent.FeedPendingPcmChunks", "Unable to post short clip");
      CleanupAudioEngine();
      return false;
    }
    _audioPosted = true;
  }

  if (_waveData) {
    _waveData->DoneProducingData();
  }

  LOG_INFO("SdkAudioComponent.FeedPendingPcmChunks", "Finished queueing PCM (%zu bytes)",
           _pendingPcmOffset);
  _pcmFilePlayActive = false;
  _pendingPcm.clear();
  _pendingPcmOffset = 0;
  return false;
}

//
// Send audio streaming status message to Engine process
//
bool SdkAudioComponent::SendAnimToEngine(SDKAudioStreamingState audioState, uint32_t audioSent, uint32_t audioPlayed)
{
  LOG_DEBUG("sdkAudioComponent.SendAnimToEngine", 
            "audioState %d audioSent %d audioPlayed %d", (int)audioState, audioSent, audioPlayed);
  AudioStreamStatusEvent evt;
  evt.streamResultID = audioState;
  evt.audioReceived = audioSent;  //used only for ChunkAdded messages
  evt.audioPlayed = audioPlayed;  //used only for ChunkAdded messages
  return AnimProcessMessages::SendAnimToEngine(std::move(evt));
}

}   // end namespace Anki
}   // end namespace Vector
