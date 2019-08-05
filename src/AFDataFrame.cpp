#include "include/AFDataFrame.h"
#include "include/BatchFunctions.h"
#include "include/TPCDI_Utils.h"
#include "include/Enums.h"
#include <cstring>
#include "include/Logger.h"
#if defined(USING_OPENCL)
#include "include/OpenCL/opencl_kernels.h"
#elif defined(USING_CUDA)
#include "CUDA/cuda_kernels.h"
#else
#include "CPU/vector_functions.h"
#endif
#ifndef ULL
#define ULL
    typedef unsigned long long ull;
#endif
using namespace BatchFunctions;
using namespace TPCDI_Utils;
using namespace af;


AFDataFrame::AFDataFrame(AFDataFrame&& other) noexcept : _data(std::move(other._data)),
                                                         _nameToIdx(std::move(other._nameToIdx)),
                                                         _idxToName(std::move(other._idxToName)),
                                                         _name(std::move(other._name)) {
}

AFDataFrame::AFDataFrame(AFDataFrame const &other) : _data(other._data),
                                                     _nameToIdx(other._nameToIdx),
                                                     _idxToName(other._idxToName),
                                                     _name(other._name) {
}

AFDataFrame& AFDataFrame::operator=(AFDataFrame&& other) noexcept {
    _data = std::move(other._data);
    _nameToIdx = std::move(other._nameToIdx);
    _idxToName = std::move(other._idxToName);
    _name = std::move(other._name);
    return *this;
}

AFDataFrame& AFDataFrame::operator=(AFDataFrame const &other) noexcept {
    _data = other._data;
    _nameToIdx = other._nameToIdx;
    _name = other._name;
    return *this;
}

void AFDataFrame::add(Column &column, const char *name) {
    _data.emplace_back(column);
    if (name) nameColumn(name, (int)(_data.size() - 1));
}

void AFDataFrame::add(Column &&column, const char *name) {
    add(column, name);
}

void AFDataFrame::insert(Column &column, int index, const char *name) {
    _idxToName.erase(index);
    for (auto &i : _nameToIdx) {
        if (i.second >= index) {
            i.second += 1;
            _idxToName.erase(i.second);
            _idxToName.insert(std::make_pair(i.second, i.first));
        }
    }
    _data.insert(_data.begin() + index, column);
    if (name) nameColumn(name, index);
}

void AFDataFrame::insert(Column &&column, int index, const char *name) {
    insert(column, index, name);
}

void AFDataFrame::remove(int index) {
    _data.erase(_data.begin() + index);
    _nameToIdx.erase(_idxToName[index]);
    _idxToName.erase(index);
    for (auto &i : _nameToIdx) {
        if (i.second >= index) {
            i.second -= 1;
            _idxToName.erase(i.second);
            _idxToName.insert(std::make_pair(i.second, i.first));
        }
    }
}

void AFDataFrame::_flush(af::array const &idx) {
    for (auto &i : _data) {
        i = Column(i(span, idx), i.index(span, idx), i.type());
    }
    //todo
}

af::array AFDataFrame::stringMatch(int column, char const *str) const {
    if (_data[column].type() != STRING) throw std::runtime_error("Invalid column type");
    auto length = strlen(str) + 1;
    if (_data[column].dims(0) < length) return constant(0, dim4(1, _data[column].dims(1)), b8);

    auto match = array(length, str);
    auto idx = allTrue(batchFunc(_data[column].rows(0, --length), match, batchEqual), 0);
    idx.eval();
    return idx;
}

AFDataFrame AFDataFrame::project(int const *columns, int size, std::string const &name) const {
    AFDataFrame output;
    output.name(name.empty() ? _name : name);
    for (int i = 0; i < size; i++) {
        int n = columns[i];
        output.add(project(n));
        if (_idxToName.count(n)) output.nameColumn(_idxToName.at(n), i);
    }
    return output;
}

AFDataFrame AFDataFrame::select(af::array const &index, std::string const &name) const {
    AFDataFrame out(*this);
    if (!name.empty()) out.name(name);
    for (auto &a : out._data) a.select(index, true);
    return out;
}

AFDataFrame AFDataFrame::project(std::string const *names, int size, std::string const &name) const {
    int columns[size];

    for (int i = 0; i < size; i++)
        columns[i] = _nameToIdx.at(names[i]);

    return project(columns, size, name);
}

AFDataFrame AFDataFrame::zip(AFDataFrame &rhs) const {
    if (length() != rhs.length()) throw std::runtime_error("Left and Right tables do not have the same length");
    AFDataFrame output = *this;

    for (size_t i = 0; i < rhs._data.size(); ++i)
        output.add(rhs._data[i], (rhs.name() + "." + rhs._idxToName.at(i)).c_str());

    return output;
}

AFDataFrame AFDataFrame::unionize(AFDataFrame &frame) const {
    if (_data.size() != frame._data.size()) throw std::runtime_error("Number of attributes do not match");

    auto out(*this);
    for (size_t i = 0; i < out._data.size(); ++i)
        out._data[i] = out._data[i].concatenate(frame._data[i]);

    return out;
}

void AFDataFrame::sortBy(int column, bool isAscending) {
    array elements = hashColumn(column, true);
    auto const size = elements.dims(0);
    if (!size) return;
    array sorting;
    array idx;
    sort(sorting, idx, elements(end, span), 1, isAscending);
    for (auto j = size - 2; j >= 0; --j) {
        elements = elements(span, idx);
        sort(sorting, idx, elements(j, span), 1, isAscending);
    }
    _flush(idx);
}

void AFDataFrame::sortBy(int *columns, int size, bool const *isAscending) {
    for (auto i = size - 1; i >= 0; --i) {
        auto asc = isAscending ? isAscending[i] : true;
        sortBy(columns[i], asc);
    }
}

AFDataFrame AFDataFrame::equiJoin(AFDataFrame const &rhs, int lhs_column, int rhs_column) const {
    if (_data[lhs_column].type() != rhs._data[lhs_column].type())
        throw std::runtime_error("Supplied column data types do not match");
    // TODO keep bytehash
    auto l = hashColumn(lhs_column);
    auto r = rhs.hashColumn(rhs_column);

    auto idx = setCompare(l, r);
    if (_data[lhs_column].dims(0) <= rhs._data[rhs_column].dims(0)) {
        l = _data[lhs_column](span, idx.first);
        r = rhs._data[rhs_column](range(dim4(l.dims(0)), 0, u32), idx.second);
        r = moddims(r, l.dims());
    } else {
        r = rhs._data[rhs_column](span, idx.second);
        l = _data[lhs_column](range(dim4(r.dims(0)), 0, u32), idx.first);
        l = moddims(l, r.dims());
    }
    // TODO collision check use byte hash
    if (_data[lhs_column].type() == STRING) {
        auto tmp = allTrue(l == r, 0);
        idx.first = idx.first(tmp);
        idx.second = idx.second(tmp);
    }

    AFDataFrame result;
    for (size_t i = 0; i < _data.size(); i++) {
      result.add(_data[i].select(idx.first), _idxToName.at(i).c_str());
    }

    for (size_t i = 0; i < rhs._data.size(); i++) {
        if (i == rhs_column) continue;
        result.add(rhs._data[i].select(idx.second),
                   (rhs.name() + "." + rhs._idxToName.at(i)).c_str());
    }

    return result;
}

std::pair<af::array, af::array> AFDataFrame::setCompare(array const &left, array const &right) {
    printf("LHS rows: %llu\n", left.elements());
    printf("RHS rows: %llu\n", right.elements());
    Logger::startTimer("Join");
    array lhs;
    array rhs;
    array idx;
    sort(lhs, idx, left, 1);
    lhs = join(0, lhs, idx.as(lhs.type()));
    sort(rhs, idx, right, 1);
    rhs = join(0, rhs, idx.as(rhs.type()));

    auto const equalSet = flipdims(setIntersect(setUnique(lhs.row(0), true), setUnique(rhs.row(0), true), true));
    bagSetIntersect(lhs, equalSet);
    bagSetIntersect(rhs, equalSet);

    auto equals = equalSet.elements();
    joinScatter(lhs, rhs, equals);
    printf("Output rows: %llu\n", lhs.elements());
    Logger::logTime("Join");
    return { lhs, rhs };
}

std::string AFDataFrame::name(std::string const& str) {
    _name = str;
    return _name;
}

void AFDataFrame::nameColumn(std::string const& name, unsigned int column) {
    if (_idxToName.count(column)) _nameToIdx.erase(_idxToName.at(column));
    _nameToIdx[name] = column;
    _idxToName[column] = name;
}

void AFDataFrame::flushToHost() {
    if (_data.empty()) return;
    for (auto &a : _data) a.toHost(true);
}

void AFDataFrame::clear() {
    _data.clear();
    _name.clear();
    _idxToName.clear();
    _nameToIdx.clear();
}



