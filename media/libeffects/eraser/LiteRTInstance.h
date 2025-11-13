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

#pragma once

#include <memory>
#include <string>
#include <string_view>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"

#include <tensorflow/lite/c/c_api_types.h>
#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model.h>

#pragma clang diagnostic pop

namespace aidl::android::hardware::audio::effect {

class LiteRTInstance {
  public:
    // Constructor stores configuration, does not load model yet
    explicit LiteRTInstance(const std::string_view& modelPath);
    ~LiteRTInstance();

    /**
     * @brief Initialization: Loads model, creates delegate, builds interpreter, allocate tensors.
     * @param numThreads Optional: Number of threads for the interpreter. `-1` lets TFLite decide.
     */
    bool initialize(int numThreads = 1);

    /**
     * @brief Checks if the instance has been successfully initialized.
     * @return true if initialized, false otherwise.
     */
    bool isInitialized() const { return mInterpreter != nullptr; }

    // Releases TFLite resources (interpreter, model) in the correct order.
    // Safe to call multiple times or on a non-initialized instance
    void cleanup();

    // Performs a warmup inference run, this can help optimize subsequent inference calls.
    // Must be called after successful initialize().
    bool warmup();

    // Gets a pointer to the underlying TFLite interpreter.
    tflite::Interpreter* interpreter() const { return mInterpreter.get(); }
    // Gets the index of the primary input tensor.
    int inputTensorIndex() const { return mInputTensorIdx; }
    // Gets the data type of the primary input tensor.
    TfLiteType inputType() const { return mInputType; }
    // Gets the index of the primary output tensor.
    int outputTensorIndex() const { return mOutputTensorIdx; }
    // Gets the data type of the primary output tensor.
    TfLiteType outputType() const { return mOutputType; }

    void resetTensorIndex();

    /**
     * @brief Gets a pointer to the primary input tensor structure.
     * Provides access to the input tensor metadata (dims, type) and data buffer.
     * @return Pointer to the TfLiteTensor, or nullptr if not initialized or index is invalid.
     */
     TfLiteTensor* inputTensor() const;

     /**
      * @brief Gets a pointer to the primary output tensor structure.
      * Provides access to the output tensor metadata (dims, type) and data buffer.
      * @return Pointer to the TfLiteTensor, or nullptr if not initialized or index is invalid.
      */
     TfLiteTensor* outputTensor() const;

     /**
      * @brief Dumps details about the loaded model (inputs, outputs, ops).
      * Useful for debugging. Requires interpreter to be initialized.
      * @return A string containing model details, or an error message.
      */
     std::string dumpModelDetails();

private:
    const std::string mModelPath;
    // Resources (Managed internally)
    std::unique_ptr<tflite::FlatBufferModel> mModel;
    std::unique_ptr<tflite::Interpreter> mInterpreter;
    // TODO: Add delegate unique_ptr


    // Metadata (Extracted during initialization)
    int mInputTensorIdx;
    TfLiteType mInputType;
    int mOutputTensorIdx;
    TfLiteType mOutputType;

    /**
     * @brief Dumps shape information for a specific tensor.
     * @param tensorIndex The index of the tensor.
     * @return A string describing the tensor's shape, or an error message.
     */
    std::string dumpTensorShape(const int tensorIndex);

    // Prevent copying and moving as this class manages unique resources.
    LiteRTInstance(const LiteRTInstance&) = delete;
    LiteRTInstance& operator=(const LiteRTInstance&) = delete;
    LiteRTInstance(LiteRTInstance&&) = delete;
    LiteRTInstance& operator=(LiteRTInstance&&) = delete;
};

}  // namespace aidl::android::hardware::audio::effect