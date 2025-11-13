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

#define LOG_TAG "AHAL_LiteRTInstance"

#include <sstream>
#include <string>

#include <android-base/logging.h>

#include "LiteRTInstance.h"

namespace aidl::android::hardware::audio::effect {

LiteRTInstance::LiteRTInstance(const std::string_view& modelPath)
    : mModelPath(modelPath),
      mInputTensorIdx(-1),
      mInputType(kTfLiteNoType),
      mOutputTensorIdx(-1),
      mOutputType(kTfLiteNoType) {
    LOG(DEBUG) << "LiteRTInstance created for model: " << mModelPath;
}

LiteRTInstance::~LiteRTInstance() {
    cleanup();
    LOG(DEBUG) << "LiteRTInstance destructor called for model: " << mModelPath;
}

bool LiteRTInstance::initialize(int numThreads) {
    if (isInitialized()) {
        LOG(WARNING) << "Instance already initialized for model " << mModelPath;
        return true;
    }

    mModel = tflite::FlatBufferModel::BuildFromFile(std::string(mModelPath).c_str());
    if (!mModel) {
        LOG(ERROR) << "Failed to load model file " << mModelPath;
        return false;
    }
    LOG(INFO) << "Model " << mModelPath << "successfully loaded";

    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder builder(*mModel, resolver);
    if (builder.SetNumThreads(numThreads) != kTfLiteOk) {
        LOG(ERROR) << "Failed to set number of threads to: " << numThreads;
        cleanup();
        return false;
    }

    const TfLiteStatus status = builder(&mInterpreter);
    if(status != kTfLiteOk || mInterpreter == nullptr) {
        LOG(ERROR) << "Interpreter builder failed with ret " << status << ", interpreter "
                   << mInterpreter;
        return false;
    }

    if (mInterpreter->AllocateTensors() != kTfLiteOk) {
        LOG(ERROR) << "Failed to allocate tensors";
        cleanup();
        return false;
    }

    // Get Input and Output Tensor Details for sanity checks
    // Note: This assumes the model has at least one input and one output.
    if (mInterpreter->inputs().empty() || mInterpreter->outputs().empty()) {
        LOG(ERROR) << "Invalid input/output for model " << mModelPath;
        cleanup();
        return false;
    }

    mInputTensorIdx = mInterpreter->inputs()[0];
    TfLiteTensor* inputTensor = mInterpreter->tensor(mInputTensorIdx);
    if (!inputTensor) {
        LOG(ERROR) << "Failed to get input tensor structure for index " << mInputTensorIdx;
        cleanup();
        return false;
    }
    mInputType = inputTensor->type;
    // Perform type check if needed (e.g., assert mInputType == kTfLiteFloat16)

    mOutputTensorIdx = mInterpreter->outputs()[0];
    TfLiteTensor* outputTensor = mInterpreter->tensor(mOutputTensorIdx);
    if (!outputTensor) {
        LOG(ERROR) << "Failed to get output tensor structure for index " << mOutputTensorIdx;
        cleanup();
        return false;
    }
    mOutputType = outputTensor->type;

    LOG(DEBUG) << "Model " << mModelPath << " loaded, tensor Info: " << dumpModelDetails();
    return true;
}

TfLiteTensor* LiteRTInstance::inputTensor() const {
    if (!isInitialized() || mInputTensorIdx < 0) {
        return nullptr;
    }
    return mInterpreter->tensor(mInputTensorIdx);
}

TfLiteTensor* LiteRTInstance::outputTensor() const {
    if (!isInitialized() || mOutputTensorIdx < 0) {
        return nullptr;
    }
    return mInterpreter->tensor(mOutputTensorIdx);
}

void LiteRTInstance::resetTensorIndex() {
    mInputTensorIdx = 0;
    mOutputTensorIdx = 0;
}

// Releases resources in the correct order
void LiteRTInstance::cleanup() {
    // Interpreter must be reset before the delegate it uses
    mInterpreter.reset();
    mModel.reset();
    LOG(INFO) << "Instance cleaned up " << mModelPath;
}

// warmup inference once with all zero data
bool LiteRTInstance::warmup() {
    if (!isInitialized()) {
        LOG(DEBUG) << "Warmup called on non-initialized instance: " << mModelPath;
        return false;
    }

    TfLiteStatus invokeStatus = mInterpreter->Invoke();
    if (invokeStatus != kTfLiteOk) {
        LOG(ERROR) << "Model " << mModelPath << " warmup failed: " <<  invokeStatus;
        return false;
    }
    return true;
}

std::string LiteRTInstance::dumpTensorShape(const int tensorIndex) {
    if (!isInitialized()) {
        return "Not Initialized";
    }
    TfLiteTensor* tensor = mInterpreter->tensor(tensorIndex);
    if (!tensor || !tensor->dims) {
        return "Invalid Tensor or Dims";
    }

    // TODO: move to utility method
    std::ostringstream oss;
    oss << "[";
    for (int i = 0; i < tensor->dims->size; ++i) {
        oss << tensor->dims->data[i] << (i == tensor->dims->size - 1 ? "" : ", ");
    }
    oss << "]";
    return oss.str();
}

std::string LiteRTInstance::dumpModelDetails() {
    if (!isInitialized()) {
       return "uninitialized.";
   }

   std::ostringstream oss;
   oss << "Model Path: " << mModelPath << "\n";
   oss << "Input Tensors (" << mInterpreter->inputs().size() << "):\n";
   for (int index : mInterpreter->inputs()) {
       TfLiteTensor* tensor = mInterpreter->tensor(index);
       oss << "  Index " << index << ": " << (tensor ? tensor->name : "N/A")
           << ", Type " << static_cast<int>(tensor ? tensor->type : kTfLiteNoType)
           << ", Shape " << dumpTensorShape(index) << "\n";
   }

   oss << "Output Tensors (" << mInterpreter->outputs().size() << "):\n";
   for (int index : mInterpreter->outputs()) {
       TfLiteTensor* tensor = mInterpreter->tensor(index);
       oss << "  Index " << index << ": " << (tensor ? tensor->name : "N/A")
           << ", Type " << static_cast<int>(tensor ? tensor->type : kTfLiteNoType)
           << ", Shape " << dumpTensorShape(index) << "\n";
   }
   return oss.str();
}

}  // namespace aidl::android::hardware::audio::effect
