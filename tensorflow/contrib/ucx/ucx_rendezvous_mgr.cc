/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

//#ifdef TENSORFLOW_USE_VERBS

#include "tensorflow/contrib/ucx/ucx_rendezvous_mgr.h"
#include <unordered_set>
#include "tensorflow/contrib/ucx/ucx_util.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/common_runtime/gpu/gpu_util.h"
#include "tensorflow/core/common_runtime/gpu/process_state.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"

#define UCX_RENDEZVOUS_MGR_META_DATA_FLAG (1)
#define UCX_RENDEZVOUS_MGR_TENSOR_CONTENT_FLAG (0)

namespace tensorflow {

void UcxRemoteRendezvous::RecvFromRemoteAsync(
    const Rendezvous::ParsedKey& parsed, const Rendezvous::Args& args,
    DoneCallback done) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  Status s;
  Device* dst_dev;
  string src_worker, src_rel_device;
  DeviceNameUtils::SplitDeviceName(parsed.src_device, &src_worker,
                                     &src_rel_device);
  ucp_worker_h ucp_worker = ucx_mgr_->GetWorker();
  if (src_worker.compare(ucx_mgr_->GetLocalWorkerName()) == 0) {
      VLOG(INFO) << "Oh Shit!!! receive from local in remote!!!";
  }
  s = env_->device_mgr->LookupDevice(parsed.dst_device, &dst_dev);
  if (!s.ok()) {
    done(s, Args(), args, Tensor(), true);
    VLOG(INFO) << "Return from "<< __FUNCTION__ << " with status " << s.ToString();
    return;
  }
  UcxTensorRecv* context =
      new UcxTensorRecv(ucp_worker, parsed, args, step_id_, dst_dev, done);
  context->Start();
}

void UcxRemoteRendezvous::UcxTensorRecv::RecvTensorMetaData() {
  void* request = nullptr;
  int recv_data_size = 0;
  UcxTensorMetaData tensor_proto;

  string key((parsed_.FullKey().ToString()));
  char buf[128];
  char *out = buf;
  for (int i = 0; i < data_size_; ++i) {
    out += sprintf(out, "0x%02x ", (unsigned int)meta_data_msg_[i]);
  }
  LOG(INFO) << "[" << step_id_ << "] Got meta-data " << key << ": " << buf;
  CHECK(ParseProtoUnlimited(&tensor_proto, meta_data_msg_, data_size_))
      << "fail to parse proto from array. Data size " << data_size_;
  meta_data_ = new UcxMetaData(tensor_proto);
  bool can_memcpy = DataTypeCanUseMemcpy(meta_data_->dtype_);
  ucp_tag_t tag =
      UcxUtil::CalcTag(key, step_id_, UCX_RENDEZVOUS_MGR_TENSOR_CONTENT_FLAG);
  // Get the relevant information from meta data message and receive Tensor
  // content message
  if (meta_data_->is_dead_) {
    VLOG(INFO) << "Is dead not implemented yet";
    // TODO: handle in the future
  }
  // String case (can't DMA)
  if (can_memcpy) {
    result_tensor_ = new Tensor(dst_dev_->GetAllocator(recv_args_.alloc_attrs),
                                meta_data_->dtype_, meta_data_->tensor_shape_);
    // TODO: in the future handle GPU copy
    data_msg_ = DMAHelper::base(result_tensor_);
    recv_data_size = result_tensor_->TotalBytes();
  } else {
    result_tensor_ = new Tensor(meta_data_->dtype_, meta_data_->tensor_shape_);
    data_msg_ = malloc(meta_data_->proto_size_);
    CHECK(data_msg_ != nullptr) << ": allocate memory failed";
    recv_data_size = meta_data_->proto_size_;
  }
  request = (UcxTensorRecv*)ucp_tag_recv_nb(
      ucp_worker_, data_msg_, recv_data_size, ucp_dt_make_contig(1), tag, -1,
      &RecvTensorContentHandler);
  if (UCS_PTR_IS_ERR(request)) {
    VLOG(ERROR) << "unable to receive UCX data message"
                << UCS_PTR_STATUS(request);
    if (!can_memcpy) {
      free(data_msg_);
    }
    delete this;
  } else {
    VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] In progress, recv content tag " << tag;
    *(UcxTensorRecv**)request = this;
  }
}

void UcxRemoteRendezvous::UcxTensorRecv::RecvTensorContent() {
  bool can_memcpy = DataTypeCanUseMemcpy(meta_data_->dtype_);

  // String
  if (can_memcpy) {
    // TODO: CPU to GPU copy --> in the future
    Done(Status::OK());
  } else {
    TensorProto proto;
    CHECK(ParseProtoUnlimited(&proto, data_msg_, meta_data_->proto_size_))
        << "fail to parse proto from array";
    Status s = dst_dev_->MakeTensorFromProto(proto, recv_args_.alloc_attrs,
                                             result_tensor_);
    free(data_msg_);
    Done(s);
  }
}

void UcxRemoteRendezvous::UcxTensorRecv::Done(const Status& s) {
  Tensor val = std::move(*result_tensor_);
  Rendezvous::Args recv_args = std::move(recv_args_);
  bool is_dead = meta_data_->is_dead_;
  DoneCallback done = done_;
  // Should be called last:
  delete this;
  done(s, Rendezvous::Args(), recv_args, val, is_dead);
}

/* static */
UcxRemoteRendezvous::UcxTensorRecv*
UcxRemoteRendezvous::UcxTensorRecv::WaitForContext(void* request,
                                                   ucs_status_t status,
                                                   ucp_tag_recv_info_t* info,
                                                   string func_name) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  UcxTensorRecv* context = nullptr;
  while (context == nullptr) {
    VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] Waiting for context...";
    context = *(UcxTensorRecv**)request;
  }
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << func_name
             << " called with status: " << status << "("
             << ucs_status_string(status) << ") ,length: " << info->length;
  context->data_size_ = info->length;
//  for (int i = 0 ; i < info->length; i++)
//      VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] meta_data_msg_[" << i << "] = "
//      << std::hex << std::setfill('0') << std::setw(2) << (int)context->meta_data_msg_[i] << " ";

  ucp_request_free(request);
  return context;
}

/* static */
void UcxRemoteRendezvous::UcxTensorRecv::RecvTensorContentHandler(
    void* request, ucs_status_t status, ucp_tag_recv_info_t* info) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  WaitForContext(request, status, info, "RecvTensorContentHandler")
      ->RecvTensorContent();
}

/* static */
void UcxRemoteRendezvous::UcxTensorRecv::RecvMetaDataHandler(
    void* request, ucs_status_t status, ucp_tag_recv_info_t* info) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  WaitForContext(request, status, info, "RecvMetaDataHandler")
      ->RecvTensorMetaData();
}

void UcxRemoteRendezvous::UcxTensorRecv::Start() {
  void* request;
  string key((parsed_.FullKey().ToString()));
  VLOG(INFO) << "[" << step_id_ << "] Starting TensorRecv: " << key;
  ucp_tag_t tag =
      UcxUtil::CalcTag(key, step_id_, UCX_RENDEZVOUS_MGR_META_DATA_FLAG);
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] receive metadata: tag " << tag;
  // Receive MetaData message
  request = ucp_tag_recv_nb(
      ucp_worker_, meta_data_msg_, UCX_RENDEZVOUS_MGR_META_DATA_SIZE,
      ucp_dt_make_contig(1), tag, -1, RecvMetaDataHandler);
  if (UCS_PTR_IS_ERR(request)) {
    VLOG(ERROR) << "unable to receive UCX meta data message"
                << UCS_PTR_STATUS(request);
    delete this;
  } else {
    ucp_worker_progress(ucp_worker_);
//    VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] In progress, recv metadata tag " << tag <<
//        " request " << request << " status " << UCS_PTR_STATUS(request);
//    usleep(2000);
//    for (int i = 0 ; i < 8; i++)
//        VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] before meta_data_msg_[" << i << "] = "
//        << std::hex << std::setfill('0') << std::setw(2) << (int)meta_data_msg_[i] << " ";
    VLOG(INFO) << " passing context " << this;
    *(UcxTensorRecv**)request = this;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////////

Status UcxRemoteRendezvous::Send(const Rendezvous::ParsedKey& parsed,
                                 const Rendezvous::Args& args,
                                 const Tensor& val, const bool is_dead) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  string dst_worker, dst_rel_device;
  DeviceNameUtils::SplitDeviceName(parsed.dst_device, &dst_worker,
                                   &dst_rel_device);
  if (dst_worker.compare(ucx_mgr_->GetLocalWorkerName()) == 0) {
    return BaseRemoteRendezvous::Send(parsed, args, val, is_dead);
  }
  UcxChannel* uc = ucx_mgr_->FindChannel(dst_worker);
  UcxTensorSend* send_req =
    new UcxTensorSend(uc->GetEp(), parsed, args, step_id_, val, is_dead);

  VLOG(1) << "UcxRemoteRendezvous Send " << this << " " << parsed.FullKey();
  send_req->Start();
  return Status::OK();
}

void UcxRemoteRendezvous::UcxTensorSend::SendDone() {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  bool can_memcpy = DataTypeCanUseMemcpy(val_.dtype());
  if (send_args_.device_context) {
    send_args_.device_context->Unref();
  }
  if (tensor_buffer_ != nullptr) {
    tensor_buffer_->Unref();
  }
  while (is_meta_data_send_ == false) {
  }
  if (!can_memcpy) {
    free(data_msg_);
  }
  delete this;
}

UcxRemoteRendezvous::UcxTensorSend*
UcxRemoteRendezvous::UcxTensorSend::WaitForContext(void* request,
                                                   ucs_status_t status,
                                                   string func_name) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  UcxTensorSend* context = nullptr;
  while (context == nullptr) {
    context = *(UcxTensorSend**)request;
  }
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "]" << func_name
             << " called with status" << status << ucs_status_string(status);
  ucp_request_free(request);
  return context;
}

void UcxRemoteRendezvous::UcxTensorSend::SendTensorContentHandler(
    void* request, ucs_status_t status) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  WaitForContext(request, status, "SendTensorContentHandler")->SendDone();
}

void UcxRemoteRendezvous::UcxTensorSend::SendMetaDataHandler(
    void* request, ucs_status_t status) {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  WaitForContext(request, status, "SendMetaDataHandler")->is_meta_data_send_ =
      true;
}

void UcxRemoteRendezvous::UcxTensorSend::SendTensorContent() {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  TensorProto tensor_proto;
  void* request = 0;
  size_t msg_size = 0;
  string key((parsed_.FullKey().ToString()));
  ucp_tag_t tag =
      UcxUtil::CalcTag(key, step_id_, UCX_RENDEZVOUS_MGR_TENSOR_CONTENT_FLAG);
  bool can_memcpy = DataTypeCanUseMemcpy(val_.dtype());

  if (can_memcpy) {
    tensor_buffer_ = (TensorBuffer*)DMAHelper::buffer(&val_);
    if (tensor_buffer_ != nullptr) {
      tensor_buffer_->Ref();  // Keep buffer alive until send is completed
      msg_size = val_.TotalBytes();
      data_msg_ = tensor_buffer_->data();
    }
  } else {
    val_.AsProtoTensorContent(&tensor_proto);
    msg_size = tensor_proto.ByteSize();
    data_msg_ = malloc(msg_size);
    tensor_proto.SerializeToArray(data_msg_, tensor_proto.ByteSize());
  }

  if (send_args_.device_context) {
    send_args_.device_context->Ref();
  }
  // Send Tensor content
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__ << " tag " << tag;
  request = ucp_tag_send_nb(ep_, data_msg_, msg_size, ucp_dt_make_contig(1),
                            tag, SendTensorContentHandler);
  if (UCS_PTR_IS_ERR(request)) {
    VLOG(ERROR) << "unable to send UCX meta data message"
                << UCS_PTR_STATUS(request);
    if (!can_memcpy) {
      free(data_msg_);
    }
    delete this;
  } else if (UCS_PTR_STATUS(request) == UCS_OK) {
    SendDone();
  } else {
    *(UcxTensorSend**)request = this;
  }
}

void UcxRemoteRendezvous::UcxTensorSend::SendTensorMetaData() {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__;
  void* request;
  size_t proto_size = 0;
  UcxTensorMetaData meta_data_proto;
  TensorProto proto;
  string key((parsed_.FullKey().ToString()));
  ucp_tag_t tag =
      UcxUtil::CalcTag(key, step_id_, UCX_RENDEZVOUS_MGR_META_DATA_FLAG);
  bool can_memcpy = DataTypeCanUseMemcpy(val_.dtype());

  if (can_memcpy) {
    proto_size = 0;
  } else {
    val_.AsProtoTensorContent(&proto);
    proto_size = proto.ByteSize();
  }
  meta_data_proto.set_dtype(val_.dtype());
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__ << " type " << val_.dtype();
  val_.shape().AsProto(meta_data_proto.mutable_tensor_shape());
  meta_data_proto.set_is_dead(is_dead_);
  meta_data_proto.set_proto_size(proto_size);
  meta_data_proto.SerializeToArray(meta_data_msg_, meta_data_proto.ByteSize());

  // Send MetaData message:
//  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] " << __FUNCTION__
//       << " tag " << tag;
//  for (int i = 0 ; i < meta_data_proto.ByteSize(); i++)
//      VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] meta_data_msg_[" << i << "] = "
//      << std::hex << std::setfill('0') << std::setw(2) << (int)meta_data_msg_[i] << " ";
  char buf[128];
  char *out = buf;
  for (int i = 0; i < meta_data_proto.ByteSize(); ++i) {
    out += sprintf(out, "0x%02x ", (unsigned int)meta_data_msg_[i]);
  }
  LOG(INFO) << "Send meta-data " << key << ": " << buf;


  CHECK(ep_ != nullptr) << "EP is NULL";
  request = ucp_tag_send_nb(ep_, meta_data_msg_, meta_data_proto.ByteSize(),
                            ucp_dt_make_contig(1), tag, SendMetaDataHandler);
  if (UCS_PTR_IS_ERR(request)) {
    VLOG(ERROR) << "unable to send UCX meta data message"
                << UCS_PTR_STATUS(request);
    ucp_request_free(request);
    delete this;
  } else if (UCS_PTR_STATUS(request) == UCS_OK) {
    is_meta_data_send_ = true;
  } else {
    *(UcxTensorSend**)request = this;
  }
}

void UcxRemoteRendezvous::UcxTensorSend::Start() {
  VLOG(INFO) << "[0x" << std::hex << pthread_self() << "] Send " << __FUNCTION__;
  // Send MetaData and Tensor content one after the other in two separate
  // messages.
  SendTensorMetaData();
  SendTensorContent();
}

UcxRendezvousMgr::UcxRendezvousMgr(const WorkerEnv* env)
    : BaseRendezvousMgr(env) {}

BaseRemoteRendezvous* UcxRendezvousMgr::Create(int64 step_id,
                                               const WorkerEnv* worker_env) {
  return new UcxRemoteRendezvous(worker_env, step_id, ucx_mgr_);
}

}  // end namespace tensorflow
