/*
 * Copyright 2021 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_RUNTIME_LOCAL_VECTORIZED_TASKS_H
#define SRC_RUNTIME_LOCAL_VECTORIZED_TASKS_H

#include <runtime/local/datastructures/DenseMatrix.h>
#include <runtime/local/context/DaphneContext.h>
#include <ir/daphneir/Daphne.h>

#include <functional>
#include <vector>
#include <mutex>

using mlir::daphne::VectorSplit;
using mlir::daphne::VectorCombine;

class Task {
public:
    virtual ~Task() = default;

    virtual void execute() = 0;
};

// task for signaling closed input queue (no more tasks)
class EOFTask : public Task {
public:
    EOFTask() = default;
    ~EOFTask() override = default;
    void execute() override {}
};

// Deprecated
// single operation task (multi-threaded operations)
template <class VT>
class SingleOpTask : public Task {
private:
    void (*_func)(DenseMatrix<VT>*,DenseMatrix<VT>*,DenseMatrix<VT>*);
    DenseMatrix<VT>* _res;
    DenseMatrix<VT>* _input1;
    DenseMatrix<VT>* _input2;
    uint64_t _rl{};    // row lower index
    uint64_t _ru{};    // row upper index
    uint64_t _bsize{}; // batch size (data binding)

public:
    SingleOpTask() = default;

    SingleOpTask(uint64_t rl, uint64_t ru, uint64_t bsize) :
        SingleOpTask(nullptr, nullptr, nullptr, nullptr, rl, ru, bsize) {}

    SingleOpTask(void (*func)(DenseMatrix<VT>*,DenseMatrix<VT>*,DenseMatrix<VT>*),
        DenseMatrix<VT>* res, DenseMatrix<VT>* input1, DenseMatrix<VT>* input2,
        uint64_t rl, uint64_t ru, uint64_t bsize)
    {
        _func = func;
        _res = res;
        _input1 = input1;
        _input2 = input2;
        _rl = rl;
        _ru = ru;
        _bsize = bsize;
    }

    ~SingleOpTask() override = default;

    void execute() override {
       for( uint64_t r = _rl; r < _ru; r+=_bsize ) {
           //create zero-copy views of inputs/outputs
           uint64_t r2 = std::min(r+_bsize, _ru);
           DenseMatrix<VT>* lres = _res->slice(r, r2);
           DenseMatrix<VT>* linput1 = _input1->slice(r, r2);
           DenseMatrix<VT>* linput2 = (_input2->getNumRows()==1) ?
               _input2 : _input2->slice(r, r2); //broadcasting
           //execute function on given data binding (batch size)
           _func(lres, linput1, linput2);
           //cleanup
           //TODO can't delete views without destroying the underlying arrays + private
       }
    }
};

//TODO tasks for compiled pipelines
template<class VT>
class CompiledPipelineTask : public Task
{
protected:
    std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))> _func;
    std::mutex &_resLock;
    DenseMatrix<VT> *&_res;
    DenseMatrix<VT> **_inputs;
    size_t _numInputs;
    [[maybe_unused]] size_t _numOutputs;
    const int64_t *_outRows;
    [[maybe_unused]] const int64_t *_outCols;
    VectorSplit *_splits;
    VectorCombine *_combines;
    uint64_t _rl;    // row lower index
    uint64_t _ru;    // row upper index
    uint64_t _bsize; // batch size (data binding)
    uint64_t _offset;
    DCTX(_ctx);

public:
    CompiledPipelineTask(std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))> func,
                         std::mutex &resLock,
                         DenseMatrix<VT> *&res,
                         DenseMatrix<VT> **inputs,
                         size_t numInputs,
                         size_t numOutputs,
                         const int64_t *outRows,
                         const int64_t *outCols,
                         VectorSplit *splits,
                         VectorCombine *combines,
                         uint64_t rl,
                         uint64_t ru,
                         uint64_t bsize, uint64_t offset, DCTX(ctx))
        : _func(func), _resLock(resLock), _res(res), _inputs(inputs), _numInputs(numInputs), _numOutputs(numOutputs),
          _outRows(outRows), _outCols(outCols), _splits(splits), _combines(combines), _rl(rl), _ru(ru), _bsize(bsize),
         _offset(offset), _ctx(ctx)
    {}

    ~CompiledPipelineTask() override = default;

    void execute() override;

private:
    void accumulateOutputs(DenseMatrix<VT> *&lres, DenseMatrix<VT> *&localAddRes, uint64_t rowStart, uint64_t rowEnd);

protected:
    std::vector<DenseMatrix<VT> *> createFuncInputs(uint64_t rowStart, uint64_t rowEnd);
};

template<typename VT>
class CompiledPipelineTaskCUDA : public CompiledPipelineTask<VT> {
public:
    CompiledPipelineTaskCUDA(std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))> func,
                         std::mutex &resLock,
                         DenseMatrix<VT> *&res,
                         DenseMatrix<VT> **inputs,
                         size_t numInputs,
                         size_t numOutputs,
                         const int64_t *outRows,
                         const int64_t *outCols,
                         VectorSplit *splits,
                         VectorCombine *combines,
                         uint64_t rl,
                         uint64_t ru,
                         uint64_t bsize, uint64_t offset, DCTX(ctx))
            : CompiledPipelineTask<VT>(func, resLock, res, inputs, numInputs, numOutputs, outRows, outCols, splits, combines, rl, ru, bsize, offset, ctx)
//            _func(func), _resLock(resLock), _res(res), _inputs(inputs), _numInputs(numInputs), _numOutputs(numOutputs),
//              _outRows(outRows), _outCols(outCols), _splits(splits), _combines(combines), _rl(rl), _ru(ru), _bsize(bsize),
//              _offset(offset), _ctx(ctx)
    {}

    ~CompiledPipelineTaskCUDA() override = default;

    void execute() override;

    void accumulateOutputs(DenseMatrix<VT> *&lres, DenseMatrix<VT> *&localAddRes, uint64_t rowStart, uint64_t rowEnd);
};

#endif //SRC_RUNTIME_LOCAL_VECTORIZED_TASKS_H
