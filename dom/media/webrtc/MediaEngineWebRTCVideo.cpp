/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaEngineWebRTC.h"
#include "Layers.h"
#include "ImageTypes.h"
#include "ImageContainer.h"
#include "mozilla/layers/GrallocTextureClient.h"
#include "nsMemory.h"
#include "mtransport/runnable_utils.h"
#include "MediaTrackConstraints.h"

namespace mozilla {

using namespace mozilla::gfx;
using dom::ConstrainLongRange;
using dom::ConstrainDoubleRange;
using dom::MediaTrackConstraintSet;

#ifdef PR_LOGGING
extern PRLogModuleInfo* GetMediaManagerLog();
#define LOG(msg) PR_LOG(GetMediaManagerLog(), PR_LOG_DEBUG, msg)
#define LOGFRAME(msg) PR_LOG(GetMediaManagerLog(), 6, msg)
#else
#define LOG(msg)
#define LOGFRAME(msg)
#endif

/**
 * Webrtc video source.
 */

NS_IMPL_ISUPPORTS0(MediaEngineWebRTCVideoSource)

int
MediaEngineWebRTCVideoSource::FrameSizeChange(
   unsigned int w, unsigned int h, unsigned int streams)
{
  mWidth = w;
  mHeight = h;
  LOG(("Video FrameSizeChange: %ux%u", w, h));
  return 0;
}

// ViEExternalRenderer Callback. Process every incoming frame here.
int
MediaEngineWebRTCVideoSource::DeliverFrame(
   unsigned char* buffer, int size, uint32_t time_stamp,
   int64_t ntp_time_ms, int64_t render_time, void *handle)
{
  // Check for proper state.
  if (mState != kStarted) {
    LOG(("DeliverFrame: video not started"));
    return 0;
  }

  if (mWidth*mHeight + 2*(((mWidth+1)/2)*((mHeight+1)/2)) != size) {
    MOZ_ASSERT(false, "Wrong size frame in DeliverFrame!");
    return 0;
  }

  // Create a video frame and append it to the track.
  nsRefPtr<layers::Image> image = mImageContainer->CreateImage(ImageFormat::PLANAR_YCBCR);

  layers::PlanarYCbCrImage* videoImage = static_cast<layers::PlanarYCbCrImage*>(image.get());

  uint8_t* frame = static_cast<uint8_t*> (buffer);
  const uint8_t lumaBpp = 8;
  const uint8_t chromaBpp = 4;

  // Take lots of care to round up!
  layers::PlanarYCbCrData data;
  data.mYChannel = frame;
  data.mYSize = IntSize(mWidth, mHeight);
  data.mYStride = (mWidth * lumaBpp + 7)/ 8;
  data.mCbCrStride = (mWidth * chromaBpp + 7) / 8;
  data.mCbChannel = frame + mHeight * data.mYStride;
  data.mCrChannel = data.mCbChannel + ((mHeight+1)/2) * data.mCbCrStride;
  data.mCbCrSize = IntSize((mWidth+1)/ 2, (mHeight+1)/ 2);
  data.mPicX = 0;
  data.mPicY = 0;
  data.mPicSize = IntSize(mWidth, mHeight);
  data.mStereoMode = StereoMode::MONO;

  videoImage->SetData(data);

#ifdef DEBUG
  static uint32_t frame_num = 0;
  LOGFRAME(("frame %d (%dx%d); timestamp %u, ntp_time %lu, render_time %lu", frame_num++,
            mWidth, mHeight, time_stamp, ntp_time_ms, render_time));
#endif

  // we don't touch anything in 'this' until here (except for snapshot,
  // which has it's own lock)
  MonitorAutoLock lock(mMonitor);

  // implicitly releases last image
  mImage = image.forget();

  // Push the frame into the MSG with a minimal duration.  This will likely
  // mean we'll still get NotifyPull calls which will then return the same
  // frame again with a longer duration.  However, this means we won't
  // fail to get the frame in and drop frames.

  // XXX The timestamp for the frame should be based on the Capture time,
  // not the MSG time, and MSG should never, ever block on a (realtime)
  // video frame (or even really for streaming - audio yes, video probably no).
  // Note that MediaPipeline currently ignores the timestamps from MSG
  uint32_t len = mSources.Length();
  for (uint32_t i = 0; i < len; i++) {
    if (mSources[i]) {
      AppendToTrack(mSources[i], mImage, mTrackID, 1); // shortest possible duration
    }
  }

  return 0;
}

// Called if the graph thinks it's running out of buffered video; repeat
// the last frame for whatever minimum period it think it needs.  Note that
// this means that no *real* frame can be inserted during this period.
void
MediaEngineWebRTCVideoSource::NotifyPull(MediaStreamGraph* aGraph,
                                         SourceMediaStream* aSource,
                                         TrackID aID,
                                         StreamTime aDesiredTime)
{
  VideoSegment segment;

  MonitorAutoLock lock(mMonitor);
  // B2G does AddTrack, but holds kStarted until the hardware changes state.
  // So mState could be kReleased here.  We really don't care about the state,
  // though.

  StreamTime delta = aDesiredTime - aSource->GetEndOfAppendedData(aID);
  LOGFRAME(("NotifyPull, desired = %ld, delta = %ld %s", (int64_t) aDesiredTime,
            (int64_t) delta, mImage.get() ? "" : "<null>"));

  // Bug 846188 We may want to limit incoming frames to the requested frame rate
  // mFps - if you want 30FPS, and the camera gives you 60FPS, this could
  // cause issues.
  // We may want to signal if the actual frame rate is below mMinFPS -
  // cameras often don't return the requested frame rate especially in low
  // light; we should consider surfacing this so that we can switch to a
  // lower resolution (which may up the frame rate)

  // Don't append if we've already provided a frame that supposedly goes past the current aDesiredTime
  // Doing so means a negative delta and thus messes up handling of the graph
  if (delta > 0) {
    // nullptr images are allowed
    AppendToTrack(aSource, mImage, aID, delta);
  }
}

/*static*/
bool
MediaEngineWebRTCVideoSource::SatisfiesConstraintSet(const MediaTrackConstraintSet &aConstraints,
                                                     const webrtc::CaptureCapability& aCandidate) {
  if (!MediaEngineCameraVideoSource::IsWithin(aCandidate.width, aConstraints.mWidth) ||
      !MediaEngineCameraVideoSource::IsWithin(aCandidate.height, aConstraints.mHeight)) {
    return false;
  }
  if (!MediaEngineCameraVideoSource::IsWithin(aCandidate.maxFPS, aConstraints.mFrameRate)) {
    return false;
  }
  return true;
}

typedef nsTArray<uint8_t> CapabilitySet;

// SatisfiesConstraintSets (plural) answers for the capture device as a whole
// whether it can satisfy an accumulated number of capabilitySets.

bool
MediaEngineWebRTCVideoSource::SatisfiesConstraintSets(
    const nsTArray<const MediaTrackConstraintSet*>& aConstraintSets)
{
  NS_ConvertUTF16toUTF8 uniqueId(mUniqueId);
  int num = mViECapture->NumberOfCapabilities(uniqueId.get(), kMaxUniqueIdLength);
  if (num <= 0) {
    return true;
  }

  CapabilitySet candidateSet;
  for (int i = 0; i < num; i++) {
    candidateSet.AppendElement(i);
  }

  for (size_t j = 0; j < aConstraintSets.Length(); j++) {
    for (size_t i = 0; i < candidateSet.Length();  ) {
      webrtc::CaptureCapability cap;
      mViECapture->GetCaptureCapability(uniqueId.get(), kMaxUniqueIdLength,
                                        candidateSet[i], cap);
      if (!SatisfiesConstraintSet(*aConstraintSets[j], cap)) {
        candidateSet.RemoveElementAt(i);
      } else {
        ++i;
      }
    }
  }
  return !!candidateSet.Length();
}

void
MediaEngineWebRTCVideoSource::ChooseCapability(
    const VideoTrackConstraintsN &aConstraints,
    const MediaEnginePrefs &aPrefs)
{
  NS_ConvertUTF16toUTF8 uniqueId(mUniqueId);
  int num = mViECapture->NumberOfCapabilities(uniqueId.get(), kMaxUniqueIdLength);
  if (num <= 0) {
    // Mac doesn't support capabilities.
    return GuessCapability(aConstraints, aPrefs);
  }

  // The rest is the full algorithm for cameras that can list their capabilities.

  LOG(("ChooseCapability: prefs: %dx%d @%d-%dfps",
       aPrefs.mWidth, aPrefs.mHeight, aPrefs.mFPS, aPrefs.mMinFPS));

  CapabilitySet candidateSet;
  for (int i = 0; i < num; i++) {
    candidateSet.AppendElement(i);
  }

  // Pick among capabilities: First apply required constraints.

  for (uint32_t i = 0; i < candidateSet.Length();) {
    webrtc::CaptureCapability cap;
    mViECapture->GetCaptureCapability(uniqueId.get(), kMaxUniqueIdLength,
                                      candidateSet[i], cap);
    if (!SatisfiesConstraintSet(aConstraints.mRequired, cap)) {
      candidateSet.RemoveElementAt(i);
    } else {
      ++i;
    }
  }

  CapabilitySet tailSet;

  // Then apply advanced (formerly known as optional) constraints.

  if (aConstraints.mAdvanced.WasPassed()) {
    auto &array = aConstraints.mAdvanced.Value();

    for (uint32_t i = 0; i < array.Length(); i++) {
      CapabilitySet rejects;
      for (uint32_t j = 0; j < candidateSet.Length();) {
        webrtc::CaptureCapability cap;
        mViECapture->GetCaptureCapability(uniqueId.get(), kMaxUniqueIdLength,
                                          candidateSet[j], cap);
        if (!SatisfiesConstraintSet(array[i], cap)) {
          rejects.AppendElement(candidateSet[j]);
          candidateSet.RemoveElementAt(j);
        } else {
          ++j;
        }
      }
      (candidateSet.Length()? tailSet : candidateSet).MoveElementsFrom(rejects);
    }
  }

  if (!candidateSet.Length()) {
    candidateSet.AppendElement(0);
  }

  int prefWidth = aPrefs.GetWidth();
  int prefHeight = aPrefs.GetHeight();

  // Default is closest to available capability but equal to or below;
  // otherwise closest above.  Since we handle the num=0 case above and
  // take the first entry always, we can never exit uninitialized.

  webrtc::CaptureCapability cap;
  bool higher = true;
  for (uint32_t i = 0; i < candidateSet.Length(); i++) {
    mViECapture->GetCaptureCapability(NS_ConvertUTF16toUTF8(mUniqueId).get(),
                                      kMaxUniqueIdLength, candidateSet[i], cap);
    if (higher) {
      if (i == 0 ||
          (mCapability.width > cap.width && mCapability.height > cap.height)) {
        // closer than the current choice
        mCapability = cap;
        // FIXME: expose expected capture delay?
      }
      if (cap.width <= (uint32_t) prefWidth && cap.height <= (uint32_t) prefHeight) {
        higher = false;
      }
    } else {
      if (cap.width > (uint32_t) prefWidth || cap.height > (uint32_t) prefHeight ||
          cap.maxFPS < (uint32_t) aPrefs.mMinFPS) {
        continue;
      }
      if (mCapability.width < cap.width && mCapability.height < cap.height) {
        mCapability = cap;
        // FIXME: expose expected capture delay?
      }
    }
    // Same resolution, maybe better format or FPS match
    if (mCapability.width == cap.width && mCapability.height == cap.height) {
      // FPS too low
      if (cap.maxFPS < (uint32_t) aPrefs.mMinFPS) {
        continue;
      }
      // Better match
      if (cap.maxFPS < mCapability.maxFPS) {
        mCapability = cap;
      } else if (cap.maxFPS == mCapability.maxFPS) {
        // Resolution and FPS the same, check format
        if (cap.rawType == webrtc::RawVideoType::kVideoI420
          || cap.rawType == webrtc::RawVideoType::kVideoYUY2
          || cap.rawType == webrtc::RawVideoType::kVideoYV12) {
          mCapability = cap;
        }
      }
    }
  }
  LOG(("chose cap %dx%d @%dfps codec %d raw %d",
       mCapability.width, mCapability.height, mCapability.maxFPS,
       mCapability.codecType, mCapability.rawType));
}

nsresult
MediaEngineWebRTCVideoSource::Allocate(const VideoTrackConstraintsN &aConstraints,
                                       const MediaEnginePrefs &aPrefs)
{
  LOG((__FUNCTION__));
  if (mState == kReleased && mInitDone) {
    // Note: if shared, we don't allow a later opener to affect the resolution.
    // (This may change depending on spec changes for Constraints/settings)

    ChooseCapability(aConstraints, aPrefs);

    if (mViECapture->AllocateCaptureDevice(NS_ConvertUTF16toUTF8(mUniqueId).get(),
                                           kMaxUniqueIdLength, mCaptureIndex)) {
      return NS_ERROR_FAILURE;
    }
    mState = kAllocated;
    LOG(("Video device %d allocated", mCaptureIndex));
  } else {
#ifdef PR_LOGGING
    MonitorAutoLock lock(mMonitor);
    if (mSources.IsEmpty()) {
      LOG(("Video device %d reallocated", mCaptureIndex));
    } else {
      LOG(("Video device %d allocated shared", mCaptureIndex));
    }
#endif
  }

  return NS_OK;
}

nsresult
MediaEngineWebRTCVideoSource::Deallocate()
{
  LOG((__FUNCTION__));
  bool empty;
  {
    MonitorAutoLock lock(mMonitor);
    empty = mSources.IsEmpty();
  }
  if (empty) {
    // If empty, no callbacks to deliver data should be occuring
    if (mState != kStopped && mState != kAllocated) {
      return NS_ERROR_FAILURE;
    }
#ifdef XP_MACOSX
    // Bug 829907 - on mac, in shutdown, the mainthread stops processing
    // 'native' events, and the QTKit code uses events to the main native CFRunLoop
    // in order to provide thread safety.  In order to avoid this locking us up,
    // release the ViE capture device synchronously on MainThread (so the native
    // event isn't needed).
    // XXX Note if MainThread Dispatch()es NS_DISPATCH_SYNC to us we can deadlock.
    // XXX It might be nice to only do this if we're in shutdown...  Hard to be
    // sure when that is though.
    // Thread safety: a) we call this synchronously, and don't use ViECapture from
    // another thread anywhere else, b) ViEInputManager::DestroyCaptureDevice() grabs
    // an exclusive object lock and deletes it in a critical section, so all in all
    // this should be safe threadwise.
    NS_DispatchToMainThread(WrapRunnable(mViECapture,
                                         &webrtc::ViECapture::ReleaseCaptureDevice,
                                         mCaptureIndex),
                            NS_DISPATCH_SYNC);
#else
    mViECapture->ReleaseCaptureDevice(mCaptureIndex);
#endif
    mState = kReleased;
    LOG(("Video device %d deallocated", mCaptureIndex));
  } else {
    LOG(("Video device %d deallocated but still in use", mCaptureIndex));
  }
  return NS_OK;
}

nsresult
MediaEngineWebRTCVideoSource::Start(SourceMediaStream* aStream, TrackID aID)
{
  LOG((__FUNCTION__));
  int error = 0;
  if (!mInitDone || !aStream) {
    return NS_ERROR_FAILURE;
  }

  {
    MonitorAutoLock lock(mMonitor);
    mSources.AppendElement(aStream);
  }

  aStream->AddTrack(aID, 0, new VideoSegment(), SourceMediaStream::ADDTRACK_QUEUED);

  if (mState == kStarted) {
    return NS_OK;
  }
  mImageContainer = layers::LayerManager::CreateImageContainer();

  mState = kStarted;
  mTrackID = aID;

  error = mViERender->AddRenderer(mCaptureIndex, webrtc::kVideoI420, (webrtc::ExternalRenderer*)this);
  if (error == -1) {
    return NS_ERROR_FAILURE;
  }

  error = mViERender->StartRender(mCaptureIndex);
  if (error == -1) {
    return NS_ERROR_FAILURE;
  }

  if (mViECapture->StartCapture(mCaptureIndex, mCapability) < 0) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
MediaEngineWebRTCVideoSource::Stop(SourceMediaStream *aSource, TrackID aID)
{
  LOG((__FUNCTION__));
  {
    MonitorAutoLock lock(mMonitor);

    if (!mSources.RemoveElement(aSource)) {
      // Already stopped - this is allowed
      return NS_OK;
    }

    aSource->EndTrack(aID);

    if (!mSources.IsEmpty()) {
      return NS_OK;
    }
    if (mState != kStarted) {
      return NS_ERROR_FAILURE;
    }

    mState = kStopped;
    // Drop any cached image so we don't start with a stale image on next
    // usage
    mImage = nullptr;
  }
  mViERender->StopRender(mCaptureIndex);
  mViERender->RemoveRenderer(mCaptureIndex);
  mViECapture->StopCapture(mCaptureIndex);

  return NS_OK;
}

void
MediaEngineWebRTCVideoSource::Init()
{
  // fix compile warning for these being unused. (remove once used)
  (void) mFps;
  (void) mMinFps;

  LOG((__FUNCTION__));
  if (mVideoEngine == nullptr) {
    return;
  }

  mViEBase = webrtc::ViEBase::GetInterface(mVideoEngine);
  if (mViEBase == nullptr) {
    return;
  }

  // Get interfaces for capture, render for now
  mViECapture = webrtc::ViECapture::GetInterface(mVideoEngine);
  mViERender = webrtc::ViERender::GetInterface(mVideoEngine);

  if (mViECapture == nullptr || mViERender == nullptr) {
    return;
  }

  char deviceName[kMaxDeviceNameLength];
  char uniqueId[kMaxUniqueIdLength];
  if (mViECapture->GetCaptureDevice(mCaptureIndex,
                                    deviceName, kMaxDeviceNameLength,
                                    uniqueId, kMaxUniqueIdLength)) {
    return;
  }

  CopyUTF8toUTF16(deviceName, mDeviceName);
  CopyUTF8toUTF16(uniqueId, mUniqueId);

  mInitDone = true;
}

void
MediaEngineWebRTCVideoSource::Shutdown()
{
  LOG((__FUNCTION__));
  if (!mInitDone) {
    return;
  }
  if (mState == kStarted) {
    SourceMediaStream *source;
    bool empty;

    while (1) {
      {
        MonitorAutoLock lock(mMonitor);
        empty = mSources.IsEmpty();
        if (empty) {
          break;
        }
        source = mSources[0];
      }
      Stop(source, kVideoTrack); // XXX change to support multiple tracks
    }
    MOZ_ASSERT(mState == kStopped);
  }

  if (mState == kAllocated || mState == kStopped) {
    Deallocate();
  }
  mViECapture->Release();
  mViERender->Release();
  mViEBase->Release();
  mState = kReleased;
  mInitDone = false;
}

void MediaEngineWebRTCVideoSource::Refresh(int aIndex) {
  // NOTE: mCaptureIndex might have changed when allocated!
  // Use aIndex to update information, but don't change mCaptureIndex!!
  // Caller looked up this source by uniqueId, so it shouldn't change
  char deviceName[kMaxDeviceNameLength];
  char uniqueId[kMaxUniqueIdLength];

  if (mViECapture->GetCaptureDevice(aIndex,
                                    deviceName, sizeof(deviceName),
                                    uniqueId, sizeof(uniqueId))) {
    return;
  }

  CopyUTF8toUTF16(deviceName, mDeviceName);
#ifdef DEBUG
  nsString temp;
  CopyUTF8toUTF16(uniqueId, temp);
  MOZ_ASSERT(temp.Equals(mUniqueId));
#endif
}

} // namespace mozilla
