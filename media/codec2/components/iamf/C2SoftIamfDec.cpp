/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "C2SoftIamfDec"

#include "C2SoftIamfDec.h"

#include <cstddef>
#include <cstdint>

#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>
#include <android-base/properties.h>
#include <android_media_swcodec_flags.h>
#include <iamf_tools/iamf_decoder.h>
#include <iamf_tools/iamf_tools_api_types.h>
#include <log/log.h>
#include <media/stagefright/foundation/MediaDefs.h>  // For MEDIA_MIMETYPE_AUDIO_IAMF

#include "LayoutTranslation.h"

namespace android {
namespace {
constexpr char COMPONENT_NAME[] = "c2.android.iamf.decoder";

uint32_t UNSET_OUTPUT_CHANNEL_MASK = 0;
uint32_t UNSET_MAX_OUTPUT_CHANNELS = 0;
}  // namespace

using ::iamf_tools::api::IamfDecoder;
using ::iamf_tools::api::IamfStatus;
using ::std::shared_ptr;

class C2SoftIamfDec::IntfImpl : public SimpleInterface<void>::BaseParams {
  public:
    explicit IntfImpl(const shared_ptr<C2ReflectorHelper>& helper)
        : SimpleInterface<void>::BaseParams(helper, COMPONENT_NAME, C2Component::KIND_DECODER,
                                            C2Component::DOMAIN_AUDIO, MEDIA_MIMETYPE_AUDIO_IAMF) {
        noInputLatency();
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noTimeStretch();
        setDerivedInstance(this);

        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                             .withConstValue(new C2ComponentAttributesSetting(
                                     C2Component::ATTRIB_IS_TEMPORAL))
                             .build());

        // ===== Parameters for the input bitstream =====
        // The profile will be updated by the decoder once the DescriptorObus have all been parsed.
        // Level is unused by IAMF.
        // The profiles requested are set in GetIamfDecoderSettings and used to initialize the
        // decoder.
        std::vector<unsigned int> profiles = {
                C2Config::PROFILE_IAMF_SIMPLE_OPUS,
                C2Config::PROFILE_IAMF_SIMPLE_PCM,
                C2Config::PROFILE_IAMF_BASE_OPUS,
                C2Config::PROFILE_IAMF_BASE_PCM,
        };
        if (android::media::swcodec::flags::iamf_aac_flac()) {
            profiles.push_back(C2Config::PROFILE_IAMF_SIMPLE_FLAC);
            profiles.push_back(C2Config::PROFILE_IAMF_BASE_FLAC);
            profiles.push_back(C2Config::PROFILE_IAMF_SIMPLE_AAC);
            profiles.push_back(C2Config::PROFILE_IAMF_BASE_AAC);
        }
        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                        .withDefault(new C2StreamProfileLevelInfo::input(
                                0u, C2Config::PROFILE_IAMF_SIMPLE_PCM, C2Config::LEVEL_UNUSED))
                        .withFields({C2F(mProfileLevel, profile).oneOf(profiles),
                                     C2F(mProfileLevel, level).oneOf({C2Config::LEVEL_UNUSED})})
                        .withSetter(ProfileLevelSetter)
                        .build());

        addParameter(DefineParam(mInputMaxBufSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                             // Starting with a safe max size based on max(opus,aac,flac) + space
                             // for DescriptorObus. Opus: 960*6, Aac: 8196, Flac: 32768
                             .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, 36000))
                             .build());

        // ===== Params for the output audio =====
        // The max output channel count is a way of setting the requested output layout when
        // CHANNEL_MASK cannot be used due to API level.  Default to 0 so we can see when it is set.
        // IN/OUT details: Optionally set by the caller and read by the decoder.
        addParameter(
                DefineParam(mMaxOutputChannelCount, C2_PARAMKEY_MAX_CHANNEL_COUNT)
                        .withDefault(new C2StreamMaxChannelCountInfo::output(
                                0u, UNSET_MAX_OUTPUT_CHANNELS))
                        .withFields({C2F(mMaxOutputChannelCount, value).inRange(2, 8)})
                        .withSetter(
                                Setter<decltype(*mMaxOutputChannelCount)>::StrictValueWithNoDeps)
                        .build());

        // The ChannelMask represents what the physical speaker layout of the decoded (output)
        // audio. Only a fixed set of Layouts are supported by IAMF and the any given Layout might
        // not be present in the given file.  The actual Layout of the output audio is updated once
        // the DescriptorObus are processed.
        //
        // IN/OUT details: Set by the caller (if possible based on API level), then read by the
        // decoder (instead of MAX_OUTPUT_CHANNELS).  In either case, it is set the decoder and read
        // by the caller to see what output layout is actually produced.
        addParameter(
                DefineParam(mRenderedChannelMask, C2_PARAMKEY_CHANNEL_MASK)
                        .withDefault(
                                new C2StreamChannelMaskInfo::output(0u, UNSET_OUTPUT_CHANNEL_MASK))
                        .withFields({C2F(mRenderedChannelMask, value).inRange(0, 4294967292)})
                        .withSetter(Setter<decltype(*mRenderedChannelMask)>::StrictValueWithNoDeps)
                        .build());

        // Channel Count matches the ChannelMask above.
        // IN/OUT details: Set by the decoder and read by the caller.
        addParameter(DefineParam(mChannelCount, C2_PARAMKEY_CHANNEL_COUNT)
                             .withDefault(new C2StreamChannelCountInfo::output(0u, 0))
                             .withFields({C2F(mChannelCount, value).inRange(1, 24)})
                             .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                             .build());

        // The sample rate will be determined by the content in the IAMF stream and updated once the
        // DescriptorObus are processed.
        // IN/OUT details: Set by the decoder, read by the caller.
        addParameter(DefineParam(mSampleRate, C2_PARAMKEY_SAMPLE_RATE)
                             .withDefault(new C2StreamSampleRateInfo::output(0u, 48000))
                             // This decoder is currently PCM and Opus only.
                             // IAMF spec allows PCM with sample rates {44.1k, 16k, 32k, 48k, 96k}.
                             .withFields({C2F(mSampleRate, value)
                                                  .oneOf({16000, 32000, 44100, 48000, 96000})})
                             .withSetter((Setter<decltype(*mSampleRate)>::StrictValueWithNoDeps))
                             .build());

        // Only output audio in 16-bit ints are supported by this decoder.
        addParameter(
                DefineParam(mPcmEncodingInfo, C2_PARAMKEY_PCM_ENCODING)
                        .withDefault(new C2StreamPcmEncodingInfo::output(0u, C2Config::PCM_16))
                        .withFields({C2F(mPcmEncodingInfo, value).oneOf({C2Config::PCM_16})})
                        .withSetter((Setter<decltype(*mPcmEncodingInfo)>::StrictValueWithNoDeps))
                        .build());
    }

    uint32_t getOutputChannelMask() const { return mRenderedChannelMask->value; }
    uint32_t getMaxOutputChannelCount() const { return mMaxOutputChannelCount->value; }
    C2Config::pcm_encoding_t getOutputPcmEncoding() const { return mPcmEncodingInfo->value; }

    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input>& me) {
        (void)mayBlock;
        (void)me;  // TODO: validate
        return C2R::Ok();
    }

  private:
    // Params relating to the input IAMF bitstream.
    shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputMaxBufSize;
    // Params relating to the output audio.
    shared_ptr<C2StreamChannelMaskInfo::output> mRenderedChannelMask;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2StreamMaxChannelCountInfo::output> mMaxOutputChannelCount;
    shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    shared_ptr<C2StreamPcmEncodingInfo::output> mPcmEncodingInfo;
};

C2SoftIamfDec::C2SoftIamfDec(const char* name, c2_node_id_t id,
                             const shared_ptr<IntfImpl>& intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl) {}

C2SoftIamfDec::~C2SoftIamfDec() {
    onRelease();
}

iamf_tools::api::OutputLayout C2SoftIamfDec::getTargetOutputLayout() {
    mCachedOutputChannelMask = mIntf->getOutputChannelMask();
    mCachedMaxOutputChannelCount = mIntf->getMaxOutputChannelCount();
    if (mCachedOutputChannelMask != UNSET_OUTPUT_CHANNEL_MASK) {
        ALOGI("channel mask set, trying to use value %d", mCachedOutputChannelMask);
        // If the channel mask has been set, we'll use that.
        return c2_soft_iamf_internal::GetIamfLayout(mCachedOutputChannelMask);
    }
    // Fall back to using the max output channels.
    ALOGI("channel mask not set, checking max channel count: %d", mCachedMaxOutputChannelCount);
    if (mCachedMaxOutputChannelCount == UNSET_MAX_OUTPUT_CHANNELS) {
        // Stereo default, if not set.
        ALOGI("max output channels not set, defaulting to stereo.");
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemA_0_2_0;
    }
    if (mCachedMaxOutputChannelCount <= 1) {
        ALOGI("max output channels set to %d, using mono.", mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kIAMF_SoundSystemExtension_0_1_0;
    }
    if (mCachedMaxOutputChannelCount <= 5) {  // 2 to 5 channels, use stereo.
        ALOGI("max output channels set to %d, using stereo.", mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemA_0_2_0;
    }
    if (mCachedMaxOutputChannelCount <= 7) {  // 6 or 7 channels, use 5.1.
        ALOGI("max output channels set to %d, using 5.1.", mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemB_0_5_0;
    }
    if (mCachedMaxOutputChannelCount <= 9) {  // 8 or 9 channels, use 7.1.
        ALOGI("max output channels set to %d, using 7.1.", mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemI_0_7_0;
    }
    if (mCachedMaxOutputChannelCount == 10) {  // 10 channels, use Sound System D, 5.1.4.
        ALOGI("max output channels set to %d, using Sound System D, (5.1.4)",
              mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemD_4_5_0;
    }
    if (mCachedMaxOutputChannelCount == 11) {  // Exactly 11 channels, use Sound System E.
        ALOGI("max output channels set to %d, using Sound System E (4+5+1)",
              mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemE_4_5_1;
    }
    if (mCachedMaxOutputChannelCount == 12) {  // Exactly 12 channels, use Sound System J, 7.1.4.
        ALOGI("max output channels set to %d, using Sound System J (7.1.4)",
              mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemJ_4_7_0;
    }
    if (mCachedMaxOutputChannelCount <= 15) {  // 13 to 15 channels, use Sound System G (4+9+0)
        ALOGI("max output channels set to %d, using Sound System G (4+9+0)",
              mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemG_4_9_0;
    }
    if (mCachedMaxOutputChannelCount < 24) {  // 16 to 23 channels, use 9.1.6
        ALOGI("max output channels set to %d, using 9.1.6", mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kIAMF_SoundSystemExtension_6_9_0;
    }
    if (mCachedMaxOutputChannelCount == 24) {  // 24 channels use Sound System H (22.2)
        ALOGI("max output channels set to %d, using Sound System H (22.2)",
              mCachedMaxOutputChannelCount);
        return iamf_tools::api::OutputLayout::kItu2051_SoundSystemH_9_10_3;
    }
    // Any other value.
    ALOGI("max output channels set to %d, defaulting to stereo.", mCachedMaxOutputChannelCount);
    return iamf_tools::api::OutputLayout::kItu2051_SoundSystemA_0_2_0;
}

::iamf_tools::api::IamfDecoder::Settings C2SoftIamfDec::getIamfDecoderSettings() {
    mOutputLayout = getTargetOutputLayout();
    ALOGV("Creating decoder with IAMF OutputLayout %d.", mOutputLayout);
    return {.requested_layout = mOutputLayout,
            // Here we ask for default, IAMF ordering in favor of reordering for Android in this
            // file.
            .channel_ordering = ::iamf_tools::api::ChannelOrdering::kOrderingForAndroid,
            .requested_profile_versions = {
                    // Explicitly only request simple and base.
                    ::iamf_tools::api::ProfileVersion::kIamfSimpleProfile,
                    ::iamf_tools::api::ProfileVersion::kIamfBaseProfile,
            }};
}

c2_status_t C2SoftIamfDec::initializeDecoder() {
    ALOGV("Creating new decoder.");
    mIamfDecoder = nullptr;
    mOutputBufferSizeBytes = 0;
    mDescriptorProcessingComplete = false;
    mSignalledError = false;
    mSignalledEos = false;

    const auto settings = getIamfDecoderSettings();
    const IamfStatus status = IamfDecoder::Create(settings, mIamfDecoder);
    if (!status.ok()) {
        // IamfDecoder::Create fails if it cannot create the ReadBitBuffer.
        mSignalledError = true;
        return C2_NO_MEMORY;
    }
    // Set to request only LE int16 samples as specified in the above params.
    mIamfDecoder->ConfigureOutputSampleType(iamf_tools::api::OutputSampleType::kInt16LittleEndian);

    ALOGV("Decoder created.");
    return C2_OK;
}

c2_status_t C2SoftIamfDec::createNewDecoderWithDescriptorObus(const uint8_t* data,
                                                              size_t dataSize) {
    ALOGV("Creating new decoder from Descriptor OBUs.");
    mIamfDecoder = nullptr;
    mOutputBufferSizeBytes = 0;
    mDescriptorProcessingComplete = false;
    mSignalledError = false;
    mSignalledEos = false;
    const auto settings = getIamfDecoderSettings();
    const IamfStatus status =
            IamfDecoder::CreateFromDescriptors(settings, data, dataSize, mIamfDecoder);
    if (!status.ok()) {
        ALOGW("Failed to create decoder.");
        // IamfDecoder::Create fails if it cannot create the ReadBitBuffer.
        mSignalledError = true;
        return C2_NO_MEMORY;
    }
    // Set to request only LE int16 samples as specified in the above params.
    mIamfDecoder->ConfigureOutputSampleType(iamf_tools::api::OutputSampleType::kInt16LittleEndian);

    ALOGV("Decoder created with Descriptor OBUs");
    // We do NOT set mDescriptorProcessingComplete true here because we need to
    // get all the frame size, sample rate, etc values in `process`.
    return C2_OK;
}

c2_status_t C2SoftIamfDec::onInit() {
    ALOGV("onInit.");
    return initializeDecoder();
}

c2_status_t C2SoftIamfDec::onStop() {
    ALOGV("onStop.");
    // onStop should preserve the mIntf state, but not the underlying decoder.
    if (mIamfDecoder) {
        // We're destroying the decoder, nothing we want to do with an error.
        (void)mIamfDecoder->Close();
        mIamfDecoder = nullptr;
    }
    mOutputBufferSizeBytes = 0;
    mDescriptorProcessingComplete = false;
    mSignalledError = false;
    mSignalledEos = false;
    return initializeDecoder();
}

void C2SoftIamfDec::onReset() {
    ALOGV("onReset.");
    // Reset should re-initialize.
    (void)onStop();
}

void C2SoftIamfDec::onRelease() {
    ALOGV("onRelease.");
    // onRelease, the decoder is guaranteed not to be used again, so we release
    // the decoder without worrying about resetting state.
    if (mIamfDecoder) {
        // We're destroying the decoder, nothing we want to do with an error.
        (void)mIamfDecoder->Close();
        mIamfDecoder = nullptr;
    }
}

c2_status_t C2SoftIamfDec::onFlush_sm() {
    ALOGV("onFlush_sm.");
    IamfStatus status = mIamfDecoder->Reset();  // Throw away any pending work.
    // The decoder may fail to reset if it was not created with DescriptorOBUs.
    if (status.ok()) {
        // We may be jumping back in time.
        mSignalledEos = false;
        return C2_OK;
    }
    ALOGE("Failed to reset. Error: %s", status.error_message.c_str());
    // We failed to Reset.  We'll just get rid of the decoder.
    mIamfDecoder = nullptr;
    return C2_OK;
}

void C2SoftIamfDec::getAnyTemporalUnits(const std::unique_ptr<C2Work>& work,
                                        const std::shared_ptr<C2BlockPool>& pool) {
    while (mIamfDecoder->IsTemporalUnitAvailable()) {
        // Get writing block into a Span for writing by the |mIamfDecoder|.
        shared_ptr<C2LinearBlock> block;
        c2_status_t fetch_block_status =
                pool->fetchLinearBlock(mOutputBufferSizeBytes,
                                       {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block);
        if (fetch_block_status != C2_OK) {
            ALOGE("fetchLinearBlock for Output failed with status %d", fetch_block_status);
            mSignalledError = true;
            work->result = C2_NO_MEMORY;
            return;
        }
        C2WriteView wView = block->map().get();
        if (wView.error()) {
            ALOGE("write view map failed %d", wView.error());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        size_t bytesWritten = 0;
        IamfStatus status = mIamfDecoder->GetOutputTemporalUnit(
                wView.data(), mOutputBufferSizeBytes, bytesWritten);
        if (!status.ok()) {
            ALOGE("Failed to get temporal unit. Error message: %s", status.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        ALOGV("out buffer attr. size %zu", bytesWritten);
        work->worklets.front()->output.buffers.push_back(
                createLinearBuffer(block, 0, bytesWritten));
        work->worklets.front()->output.ordinal = work->input.ordinal;
    }
}

void C2SoftIamfDec::process(const std::unique_ptr<C2Work>& work,
                            const shared_ptr<C2BlockPool>& pool) {
    if (!mIamfDecoder) {
        ALOGE("Decoder instance is null.");
        return;
    }
    // N.B.: Android only supports single input buffer and single worklet.

    // Initialize output work to assume OK and that we will process one worklet.
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    // For output buffers, `configUpdate` is for communicating responses, start clear.
    work->worklets.front()->output.configUpdate.clear();
    // Copy flags from work->input to first worklet's output.
    work->worklets.front()->output.flags = work->input.flags;
    // Clear output buffers
    work->worklets.front()->output.buffers.clear();

    if (mSignalledError || mSignalledEos) {
        // We already had an error or EOS signalled previously, we should not have
        // had `process` called again.
        work->result = C2_BAD_VALUE;
        return;
    }

    // If channel mask or max output channel count has changed, we reset the decoder if and only if
    // the new values result in a different layout.
    const bool outputMaskOrMaxCountChanged =
            mCachedOutputChannelMask != mIntf->getOutputChannelMask() ||
            mCachedMaxOutputChannelCount != mIntf->getMaxOutputChannelCount();
    if (outputMaskOrMaxCountChanged) {
        // Since resetting to a different layout is disruptive, only do it if we're sure it results
        // in a different output IAMF Layout.
        if (auto newLayout = getTargetOutputLayout(); newLayout != mOutputLayout) {
            mOutputLayout = newLayout;
            IamfStatus status = mIamfDecoder->ResetWithNewLayout(mOutputLayout);
            if (!status.ok()) {
                // Layout cannot be changed if decoder was not created with DescriptorOBUs.
                ALOGE("Failed to reset with new layout. Error message: %s",
                      status.error_message.c_str());
                mSignalledError = true;
                work->result = C2_CORRUPTED;
                return;
            }
        }
    }

    // mDummyReadView provided by SimpleC2Component just returns C2_NO_INIT.
    // It is here as a placeholder.
    C2ReadView readView = mDummyReadView;
    size_t inSize = 0u;  // Initially zero until set from readView.
    if (!work->input.buffers.empty()) {
        // Input buffers are not empty, so there is work to be done.
        readView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = readView.capacity();
        // readView could give a capacity of 0 when there are no new bytes to process,
        // so it signals an error only when the readView has an error.
        if (inSize != 0 && readView.error()) {
            ALOGE("ReadView map failed %d", readView.error());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
    }
    if (inSize == 0) {
        // If inSize is still zero at this point, then there is no input to process.
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
        // No more data to send to decode if inSize is 0 and EOS signalled.
        if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
            mIamfDecoder->SignalEndOfDecoding();
            mSignalledEos = true;
            ALOGV("signalled EOS");
        }
        // For IAMF, we will still try to fetch a temporal unit at EOS.
        getAnyTemporalUnits(work, pool);
        return;
    }

    ALOGV("in buffer attr. size %zu timestamp %d frameindex %d", inSize,
          (int)work->input.ordinal.timestamp.peeku(), (int)work->input.ordinal.frameIndex.peeku());

    const bool isCodecConfig = work->input.flags & C2FrameData::FLAG_CODEC_CONFIG;
    if (isCodecConfig) {
        ALOGV("Got codec config.");
        // If the CodecConfig flag is set, then we're assuming the buffer contains
        // exactly and only the DescriptorObus.  We can re-create the IamfDecoder with the
        // DescriptorObus (Codec Config) for more efficient decoding of all subsequent Temporal
        // Units.
        c2_status_t initializeStatus = createNewDecoderWithDescriptorObus(readView.data(), inSize);
        if (initializeStatus != C2_OK) {
            ALOGE("Failed to initialize decoder with descriptor OBUs. Error code: %d",
                  initializeStatus);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
    } else {
        // If not a CodecConfig, we just try to Decode.
        IamfStatus decodeStatus = mIamfDecoder->Decode(readView.data(), inSize);
        if (!decodeStatus.ok()) {
            ALOGE("Failed to decode. Error message: %s", decodeStatus.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
    }

    // The first time that IsDescriptorProcessingComplete returns true, we update config.
    if (!mDescriptorProcessingComplete && mIamfDecoder->IsDescriptorProcessingComplete()) {
        // First time we are seeing descriptor processing as complete.
        ALOGV("Decoder signaled descriptor processing complete.");
        mDescriptorProcessingComplete = true;

        // Here we should get the sample rate info and Layout and update.
        uint32_t sampleRate;
        IamfStatus sampleRateStatus = mIamfDecoder->GetSampleRate(sampleRate);
        if (!sampleRateStatus.ok()) {
            ALOGE("Failed to get sample rate. Error message: %s",
                  sampleRateStatus.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        ALOGV("successfully got sample rate: %d", sampleRate);
        C2StreamSampleRateInfo::output sampleRateInfo(0u, sampleRate);

        // The Layout used may be different than what was requested in IamfDecoder::Create
        // because of the content of the stream.  Here we get the actual Layout that will be
        // used and convert to a ChannelMask.
        iamf_tools::api::OutputLayout actualLayout;
        IamfStatus layoutStatus = mIamfDecoder->GetOutputLayout(actualLayout);
        if (!layoutStatus.ok()) {
            ALOGE("Failed to get output layout. Error message: %s",
                  layoutStatus.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        ALOGV("successfully got actual output layout: %d", actualLayout);
        uint32_t actualChannelMask = c2_soft_iamf_internal::GetAndroidChannelMask(actualLayout);
        C2StreamChannelMaskInfo::output channelMaskInfo(0u, actualChannelMask);

        // Get the number of output channels.
        int numOutputChannels;
        IamfStatus numOutputChannelsStatus =
                mIamfDecoder->GetNumberOfOutputChannels(numOutputChannels);
        if (!numOutputChannelsStatus.ok()) {
            ALOGE("Failed to get number of output channels. Error message: %s",
                  numOutputChannelsStatus.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        ALOGV("successfully got number of output channels: %d", numOutputChannels);
        C2StreamChannelCountInfo::output channelCountInfo(0u, numOutputChannels);

        // We collect the failures but do not use them.
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        // Update the config in the params.
        const c2_status_t configStatus = mIntf->config(
                {&sampleRateInfo, &channelMaskInfo, &channelCountInfo}, C2_MAY_BLOCK, &failures);
        if (configStatus == C2_OK) {
            // Include the config update in the work for the caller to see.
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(sampleRateInfo));
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(channelMaskInfo));
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(channelCountInfo));
            ALOGV("successfully updated config.");
        } else {
            ALOGE("Config Update failed");
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }

        // We want to calculate the max size we need for the write buffer for output and for
        // that, we need the frame size.
        uint32_t frameSize;
        IamfStatus frameSizeStatus = mIamfDecoder->GetFrameSize(frameSize);
        if (!frameSizeStatus.ok()) {
            ALOGE("Failed to get frame size. Error message: %s",
                  frameSizeStatus.error_message.c_str());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
        ALOGV("successfully got frame size: %d", frameSize);

        mOutputBufferSizeBytes = (size_t)frameSize * sizeof(int16_t) * (size_t)numOutputChannels;
        ALOGV("calculated frame size bytes: %zu", mOutputBufferSizeBytes);
    }  // Done updating config.

    // In any case, check for finished temporal units to return.
    getAnyTemporalUnits(work, pool);
}

c2_status_t C2SoftIamfDec::drain(uint32_t drainMode, const shared_ptr<C2BlockPool>& pool) {
    // Practically speaking, drain is unused.
    (void)pool;
    // SimpleC2Component
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    return C2_OK;
}

class C2SoftIamfDecFactory : public C2ComponentFactory {
  public:
    C2SoftIamfDecFactory()
        : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2PlatformComponentStore()->getParamReflector())) {}

    virtual c2_status_t createComponent(c2_node_id_t id, shared_ptr<C2Component>* const component,
                                        std::function<void(C2Component*)> deleter) override {
        *component = shared_ptr<C2Component>(
                new C2SoftIamfDec(COMPONENT_NAME, id,
                                  std::make_shared<C2SoftIamfDec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2SoftIamfDec::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2SoftIamfDec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual ~C2SoftIamfDecFactory() override = default;

  private:
    shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

static bool SufficientSdkVersion() {
    static int sCurrentSdk = 0;
    static std::string sCurrentCodeName;
    static std::once_flag sCheckOnce;
    std::call_once(sCheckOnce, [&]() {
        sCurrentSdk = android_get_device_api_level();
        sCurrentCodeName = android::base::GetProperty("ro.build.version.codename", "<none>");
    });
    return sCurrentSdk >= 36 || sCurrentCodeName == "Baklava";
}

__attribute__((cfi_canonical_jump_table)) extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    if (!android::media::swcodec::flags::iamf_software_decoder()) {
        ALOGV("IAMF SW decoder is disabled by flag.");
        return nullptr;
    }

    bool enabled = SufficientSdkVersion();
    if (!enabled) {
        return nullptr;
    }
    return new ::android::C2SoftIamfDecFactory();
}

__attribute__((cfi_canonical_jump_table)) extern "C" void DestroyCodec2Factory(
        ::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
