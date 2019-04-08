/*
 * dnn_object_detection.cpp -  object detection
 *
 *  Copyright (c) 2019 Intel Corporation
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
 *
 * Author: Zong Wei <wei.zong@intel.com>
 */

#include <inference_engine.hpp>

#include "dnn_object_detection.h"

using namespace std;
using namespace InferenceEngine;

namespace XCam {

DnnObjectDetection::DnnObjectDetection (DnnInferConfig& config)
    : DnnInferenceEngine (config)
{
    XCAM_LOG_DEBUG ("DnnObjectDetection::DnnObjectDetection");
}


DnnObjectDetection::~DnnObjectDetection ()
{

}

XCamReturn
DnnObjectDetection::get_model_input_info (DnnInferInputOutputInfo& info)
{
    if (!_model_created) {
        XCAM_LOG_ERROR ("Please create the model firstly!");
        return XCAM_RETURN_ERROR_ORDER;
    }

    int id = 0;
    InputsDataMap inputs_info (_network.getInputsInfo ());

    for (auto & item : inputs_info) {
        auto& input = item.second;
        const InferenceEngine::SizeVector input_dims = input->getDims ();

        info.width[id] = input_dims[0];
        info.height[id] = input_dims[1];
        info.channels[id] = input_dims[2];
        info.object_size[id] = input_dims[3];
        info.precision[id] = convert_precision_type (input->getPrecision());
        info.layout[id] = convert_layout_type (input->getLayout());

        item.second->setPrecision(Precision::U8);

        id++;
    }
    info.batch_size = get_batch_size ();
    info.numbers = inputs_info.size ();

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DnnObjectDetection::set_model_input_info (DnnInferInputOutputInfo& info)
{
    XCAM_LOG_DEBUG ("DnnObjectDetection::set_model_input_info");

    if (!_model_created) {
        XCAM_LOG_ERROR ("Please create the model firstly!");
        return XCAM_RETURN_ERROR_ORDER;
    }

    InputsDataMap inputs_info (_network.getInputsInfo ());
    if (info.numbers != inputs_info.size ()) {
        XCAM_LOG_ERROR ("Input size is not matched with model info numbers %d !", info.numbers);
        return XCAM_RETURN_ERROR_PARAM;
    }
    int id = 0;

    for (auto & item : inputs_info) {
        Precision precision = convert_precision_type (info.precision[id]);
        item.second->setPrecision (precision);
        Layout layout = convert_layout_type (info.layout[id]);
        item.second->setLayout (layout);
        id++;
    }

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DnnObjectDetection::get_model_output_info (DnnInferInputOutputInfo& info)
{
    if (!_model_created) {
        XCAM_LOG_ERROR ("Please create the model firstly!");
        return XCAM_RETURN_ERROR_ORDER;
    }

    int id = 0;
    std::string output_name;
    OutputsDataMap outputs_info (_network.getOutputsInfo ());
    DataPtr output_info;
    for (const auto& out : outputs_info) {
        if (out.second->creatorLayer.lock()->type == "DetectionOutput") {
            output_name = out.first;
            output_info = out.second;
            break;
        }
    }
    if (output_info.get ()) {
        const InferenceEngine::SizeVector output_dims = output_info->getTensorDesc().getDims();

        info.width[id]    = output_dims[0];
        info.height[id]   = output_dims[1];
        info.channels[id] = output_dims[2];
        info.object_size[id] = output_dims[3];

        info.precision[id] = convert_precision_type (output_info->getPrecision());
        info.layout[id] = convert_layout_type (output_info->getLayout());

        info.batch_size = 1;
        info.numbers = outputs_info.size ();
    } else {
        XCAM_LOG_ERROR ("Get output info error!");
        return XCAM_RETURN_ERROR_UNKNOWN;
    }
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DnnObjectDetection::set_model_output_info (DnnInferInputOutputInfo& info)
{
    if (!_model_created) {
        XCAM_LOG_ERROR ("Please create the model firstly!");
        return XCAM_RETURN_ERROR_ORDER;
    }

    OutputsDataMap outputs_info (_network.getOutputsInfo ());
    if (info.numbers != outputs_info.size ()) {
        XCAM_LOG_ERROR ("Output size is not matched with model!");
        return XCAM_RETURN_ERROR_PARAM;
    }

    int id = 0;
    for (auto & item : outputs_info) {
        Precision precision = convert_precision_type (info.precision[id]);
        item.second->setPrecision (precision);
        Layout layout = convert_layout_type (info.layout[id]);
        item.second->setLayout (layout);
        id++;
    }

    return XCAM_RETURN_NO_ERROR;
}

void*
DnnObjectDetection::get_inference_results (uint32_t idx, uint32_t& size)
{
    if (! _model_created || ! _model_loaded) {
        XCAM_LOG_ERROR ("Please create and load the model firstly!");
        return NULL;
    }
    uint32_t id = 0;
    std::string item_name;

    OutputsDataMap outputs_info (_network.getOutputsInfo ());
    if (idx > outputs_info.size ()) {
        XCAM_LOG_ERROR ("Output is out of range");
        return NULL;
    }

    for (auto & item : outputs_info) {
        if (item.second->creatorLayer.lock()->type == "DetectionOutput") {
            item_name = item.first;
            break;
        }
        id++;
    }

    if (item_name.empty ()) {
        XCAM_LOG_ERROR ("item name is empty!");
        return NULL;
    }

    const Blob::Ptr blob = _infer_request.GetBlob (item_name);
    float* output_result = static_cast<PrecisionTrait<Precision::FP32>::value_type*>(blob->buffer ());

    size = blob->byteSize ();

    return (reinterpret_cast<void *>(output_result));
}

XCamReturn
DnnObjectDetection::get_bounding_boxes (const float* result_ptr,
                                        const uint32_t idx,
                                        std::vector<Vec4i> &boxes,
                                        std::vector<int32_t> &classes)
{
    if (!_model_created) {
        XCAM_LOG_ERROR ("Please create the model firstly!");
        return XCAM_RETURN_ERROR_ORDER;
    }

    if (!result_ptr) {
        XCAM_LOG_ERROR ("Inference results error!");
        return XCAM_RETURN_ERROR_PARAM;
    }

    DnnInferInputOutputInfo output_infos;
    get_model_output_info (output_infos);

    uint32_t image_width = get_input_image_width ();
    uint32_t image_height = get_input_image_height ();
    uint32_t max_proposal_count = output_infos.channels[idx];
    uint32_t object_size = output_infos.object_size[idx];

    uint32_t box_count = 0;
    for (uint32_t cur_proposal = 0; cur_proposal < max_proposal_count; cur_proposal++) {
        float image_id = result_ptr[cur_proposal * object_size + 0];
        if (image_id < 0) {
            break;
        }

        float label = result_ptr[cur_proposal * object_size + 1];
        float confidence = result_ptr[cur_proposal * object_size + 2];
        float xmin = result_ptr[cur_proposal * object_size + 3] * image_width;
        float ymin = result_ptr[cur_proposal * object_size + 4] * image_height;
        float xmax = result_ptr[cur_proposal * object_size + 5] * image_width;
        float ymax = result_ptr[cur_proposal * object_size + 6] * image_height;

        if (confidence > 0.5) {
            classes.push_back(static_cast<int32_t>(label));
            boxes.push_back (Vec4i ( static_cast<int32_t>(xmin),
                                     static_cast<int32_t>(ymin),
                                     static_cast<int32_t>(xmax - xmin),
                                     static_cast<int32_t>(ymax - ymin) ));

            XCAM_LOG_DEBUG ("Proposal:%d label:%d confidence:%f", cur_proposal, classes[box_count], confidence);
            XCAM_LOG_DEBUG ("Boxes[%d] {%d, %d, %d, %d}",
                            box_count, boxes[box_count][0], boxes[box_count][1],
                            boxes[box_count][2], boxes[box_count][3]);
            box_count++;
        }
    }
    return XCAM_RETURN_NO_ERROR;
}

}  // namespace XCam