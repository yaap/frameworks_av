/*
 * Copyright (C) 2025 The Android Open Source Project
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

#define LOG_TAG "AHAL_EraserContext"

#include <android-base/logging.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "EraserContext.h"
#include "LiteRTInstance.h"

using aidl::android::media::audio::eraser::Mode;

namespace aidl::android::hardware::audio::effect {

const EraserContext::EraserConfiguration EraserContext::kDefaultConfig = {
        .mode = Mode::ERASER};
const EraserContext::EraserCapability EraserContext::kCapability = {
        .modes = {Mode::ERASER, Mode::CLASSIFIER}};

EraserContext::EraserContext(int statusDepth, const Parameter::Common& common)
    : EffectContext(statusDepth, common),
      mCommon(common),
      mConfig(kDefaultConfig) {
    LOG(DEBUG) << __func__ << ": Creating EraserContext";
    init();
}

EraserContext::~EraserContext() {
    LOG(DEBUG) << __func__ << ": Destroying EraserContext";
}

void EraserContext::init() {
    mChannelCount = static_cast<int>(::aidl::android::hardware::audio::common::getChannelCount(
            mCommon.input.base.channelMask));
}

RetCode EraserContext::enable() {
    if (mConfig.mode == Mode::ERASER) {
        if (!mSeparatorInstance) {
            mSeparatorInstance = std::make_unique<LiteRTInstance>(kSeparatorModelPath);
        }
        if (mSeparatorInstance && mSeparatorInstance->initialize()) {
            mSeparatorInstance->warmup();
        } else {
            LOG(ERROR) << __func__ << ": failed to enable separator";
            return RetCode::ERROR_EFFECT_LIB_ERROR;
        }
    } else {
        mSeparatorInstance.reset();
    }

    if (!mClassifierInstance) {
        mClassifierInstance = std::make_unique<LiteRTInstance>(kClassifierModelPath);
    }
    if (mClassifierInstance && mClassifierInstance->initialize()) {
        mClassifierInstance->warmup();
    } else {
        LOG(ERROR) << __func__ << ": failed to enable classifier";
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }

    return RetCode::SUCCESS;
}

RetCode EraserContext::disable() {
    mClassifierInstance.reset();
    mSeparatorInstance.reset();
    return RetCode::SUCCESS;
}

RetCode EraserContext::reset() {
    // reset model inference indexes
    if (mSeparatorInstance) mSeparatorInstance->resetTensorIndex();
    if (mClassifierInstance) mClassifierInstance->resetTensorIndex();
    return RetCode::SUCCESS;
}

std::optional<Eraser> EraserContext::getParam(Eraser::Tag tag) {
    switch (tag) {
        case Eraser::capability:
            return Eraser::make<Eraser::capability>(kCapability);
        case Eraser::configuration:
            return Eraser::make<Eraser::configuration>(mConfig);
        default: {
            LOG(ERROR) << __func__ << " unsupported tag: " << toString(tag);
            return std::nullopt;
        }
    }
}

ndk::ScopedAStatus EraserContext::setParam(Eraser eraser) {
    const auto tag = eraser.getTag();
    switch (tag) {
        case Eraser::configuration: {
            mConfig = eraser.get<Eraser::configuration>();
            return ndk::ScopedAStatus::ok();
        }
        default: {
            LOG(ERROR) << __func__ << " unsupported tag: " << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
}

IEffect::Status EraserContext::process(float*, float*, int samples) {
    IEffect::Status procStatus = {EX_ILLEGAL_ARGUMENT, 0, 0};
    const auto inputChCount = common::getChannelCount(mCommon.input.base.channelMask);
    const auto outputChCount = common::getChannelCount(mCommon.output.base.channelMask);
    if (inputChCount < outputChCount) {
        LOG(ERROR) << __func__ << " invalid channel count, in: " << inputChCount
                   << " out: " << outputChCount;
        return procStatus;
    }

    // TODO: convert input buffer to tensor input format (16kHz/mono/float16) and process

    const int inputFrames = samples / inputChCount;
    return IEffect::Status{STATUS_OK, static_cast<int32_t>(inputFrames * inputChCount),
                           static_cast<int32_t>(inputFrames * outputChCount)};
}

}  // namespace aidl::android::hardware::audio::effect
