/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/parallel_executor.h"

#include <string>
#include <vector>

#ifdef PADDLE_WITH_CUDA
#include "paddle/fluid/platform/nccl_helper.h"
#endif

#include "paddle/fluid/framework/details/multi_devices_graph_builder.h"
#include "paddle/fluid/framework/details/threaded_ssa_graph_executor.h"
#include "paddle/fluid/platform/profiler.h"

namespace paddle {
namespace framework {

class ParallelExecutorPrivate {
 public:
  explicit ParallelExecutorPrivate(const std::vector<platform::Place> &places)
      : places_(places) {}

  std::vector<platform::Place> places_;
  std::vector<Scope *> local_scopes_;
  Scope *global_scope_;
  std::unique_ptr<details::SSAGraphExecutor> executor_;

#ifdef PADDLE_WITH_CUDA
  std::unique_ptr<platform::NCCLContextMap> nccl_ctxs_;
#endif
};

std::vector<Scope *> &ParallelExecutor::GetLocalScopes() {
  return member_->local_scopes_;
}

ParallelExecutor::ParallelExecutor(
    size_t num_threads, bool use_event,
    const std::vector<platform::Place> &places,
    const std::unordered_set<std::string> &params,
    const std::unordered_set<std::string> &bcast_vars,
    const ProgramDesc &main_program, const std::string &loss_var_name,
    Scope *scope, const std::vector<Scope *> &local_scopes, bool allow_op_delay)
    : member_(new ParallelExecutorPrivate(places)) {
  member_->global_scope_ = scope;

  // Step 1. Bcast the params to devs.
  // Create local scopes
  if (local_scopes.empty()) {
    for (size_t i = 0; i < member_->places_.size(); ++i) {
      member_->local_scopes_.push_back(&scope->NewScope());
    }
  } else {
    PADDLE_ENFORCE_EQ(member_->places_.size(), local_scopes.size());
    for (size_t i = 0; i < member_->places_.size(); ++i) {
      member_->local_scopes_.push_back(local_scopes[i]);
    }
  }

// Bcast Parameters to all GPUs
#ifdef PADDLE_WITH_CUDA
  member_->nccl_ctxs_.reset(new platform::NCCLContextMap(member_->places_));
#endif
  if (platform::is_gpu_place(places[0]) && member_->local_scopes_.size() != 1 &&
      local_scopes.empty()) {  // Is CUDA
    BCastParamsToGPUs(bcast_vars);
  }
// Startup Program has been run. All local scopes has correct parameters.

// Step 2. Convert main_program to SSA form and dependency graph. Also, insert
// ncclOp
#ifdef PADDLE_WITH_CUDA
  details::MultiDevSSAGraphBuilder builder(member_->places_, loss_var_name,
                                           params, member_->local_scopes_,
                                           member_->nccl_ctxs_.get());
#else
  details::MultiDevSSAGraphBuilder builder(member_->places_, loss_var_name,
                                           params, member_->local_scopes_);
#endif
  auto graph = builder.Build(main_program);

  member_->executor_.reset(new details::ThreadedSSAGraphExecutor(
      num_threads, use_event, member_->local_scopes_, places, std::move(graph),
      allow_op_delay));

  // Step 3. Create vars in each scope;
  for (auto *scope : member_->local_scopes_) {
    for (auto *var : main_program.Block(0).AllVars()) {
      if (scope->FindVar(var->Name()) != nullptr) {
        continue;
      }

      InitializeVariable(scope->Var(var->Name()), var->GetType());
    }
  }
}

void ParallelExecutor::BCastParamsToGPUs(
    const std::unordered_set<std::string> &vars) const {
#ifdef PADDLE_WITH_CUDA
  auto *main_scope = member_->local_scopes_[0];

  for (auto &var : vars) {
    auto *main_var = main_scope->FindVar(var);
    if (main_var == nullptr || !main_var->IsType<LoDTensor>()) {
      continue;
    }

    auto &main_tensor = main_var->Get<LoDTensor>();
    auto &dims = main_tensor.dims();
    if (paddle::platform::is_gpu_place(main_tensor.place())) {
      size_t numel = main_tensor.numel();
      ncclDataType_t data_type = platform::ToNCCLDataType(main_tensor.type());
      platform::NCCLGroupGuard guard;
      for (size_t i = 0; i < member_->places_.size(); ++i) {
        auto place = member_->places_[i];
        void *buffer;
        if (i == 0) {
          buffer = const_cast<void *>(main_tensor.data<void>());
        } else {
          auto local_scope = member_->local_scopes_[i];
          auto *t = local_scope->Var(var)->GetMutable<LoDTensor>();
          t->Resize(dims);
          buffer = t->mutable_data(place, main_tensor.type());
        }
        auto &nccl_ctx = member_->nccl_ctxs_->at(place);
        platform::dynload::ncclBcast(buffer, numel, data_type, 0,
                                     nccl_ctx.comm_, nccl_ctx.stream());
      }
    } else {
      platform::CPUPlace cpu;
      for (size_t i = 1; i < member_->places_.size(); ++i) {
        auto local_scope = member_->local_scopes_[i];
        auto *t = local_scope->Var(var)->GetMutable<LoDTensor>();
        t->Resize(dims);
        t->mutable_data(cpu, main_tensor.type());
        paddle::framework::TensorCopy(main_tensor, cpu, t);
      }
    }
    member_->nccl_ctxs_->WaitAll();
  }
#else
  PADDLE_THROW("Not compiled with CUDA");
#endif
}

void ParallelExecutor::Run(
    const std::vector<std::string> &fetch_tensors,
    const std::string &fetched_var_name,
    const std::unordered_map<std::string, LoDTensor> &feed_tensors) {
  platform::RecordBlock b(0);
  SplitTensorToPlaces(feed_tensors);
  auto fetch_data = member_->executor_->Run(fetch_tensors);
  *member_->global_scope_->Var(fetched_var_name)->GetMutable<FeedFetchList>() =
      fetch_data;
}

void ParallelExecutor::SplitTensorToPlaces(
    const std::unordered_map<std::string, LoDTensor> &feed_tensors) {
  for (auto it : feed_tensors) {
    auto lod_tensors = it.second.SplitLoDTensor(member_->places_);
    PADDLE_ENFORCE_EQ(
        member_->places_.size(), lod_tensors.size(),
        "The number of samples of current batch is less than the count of "
        "devices, currently, it is not allowed. (%d vs %d)",
        member_->places_.size(), lod_tensors.size());
    for (size_t j = 0; j < member_->places_.size(); ++j) {
      // TODO(panxy0718): Do I need to delete this var?
      auto t =
          member_->local_scopes_[j]->Var(it.first)->GetMutable<LoDTensor>();
      t->ShareDataWith(lod_tensors[j]);
      t->set_lod(lod_tensors[j].lod());
    }
  }
}

}  // namespace framework
}  // namespace paddle