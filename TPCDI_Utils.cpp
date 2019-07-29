#include "TPCDI_Utils.h"
#include "AFDataFrame.h"
#include "BatchFunctions.h"
#include <fstream>
#include <string>
#include <thread>
using namespace af;
using namespace BatchFunctions;

void printStr(array str_array) {
    str_array.row(end) = '\n';
    str_array = str_array(where(str_array));
    str_array = join(0, flat(str_array), af::constant(0, 1, u8));
    str_array.eval();
    auto d = str_array.host<uint8_t>();
    print((char*)d);
    af::freeHost(d);
}

std::string TPCDI_Utils::loadFile(char const *filename) {
    std::ifstream file(filename);
    std::string text;
    file.seekg(0, std::ios::end);
    text.reserve(((size_t)file.tellg()) + 1);
    file.seekg(0, std::ios::beg);
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    file.close();
    return text;
}

std::string TPCDI_Utils::collect(std::vector<std::string> const &files) {
    auto const num = files.size();
    auto const limit = std::thread::hardware_concurrency() - 1;

    std::vector<std::string> data((size_t)num);
    std::vector<unsigned long> sizes((size_t)num);
    static auto loader = [&](int i) {
        data[i] = TPCDI_Utils::loadFile(files[i].c_str());
        if (data[i].back() != '\n') data[i] += '\n';
        sizes[i] = data[i].size();
    };

    std::thread threads[limit];
    for (unsigned long i = 0; i < num / limit + 1; ++i) {
        auto jlim = (i == num / limit) ? num % limit : limit;
        for (auto j = i % limit; j < jlim; ++j) threads[j] = std::thread(loader, i * limit + j);
        for (auto j = i % limit; j < jlim; ++j) threads[j].join();
    }

    size_t total_size = 0;
    for (int i = 0; i < num; ++i) total_size += sizes[i];
    std::string output;
    output.reserve(total_size + 1);
    for (int i = 0; i < num; ++i) output.append(data[i]);

    return output;
}

inline af::array getNegatives(af::array &str, unsigned int maximum) {
    auto cond = where(str == '-');
    auto negatives = cond / maximum;
    negatives = moddims(negatives, dim4(1, negatives.dims(0)));
    str(cond) = 255;
    return negatives;
}

inline af::array getDecimal(af::array &str, unsigned int maximum) {
    auto cond = where(str == '.');
    auto points = constant(0, 1, str.elements() / maximum, s32);
    points(cond / maximum) = (maximum - cond % maximum).as(s32);
    str(cond) = 255;
    return points;
}

inline void digitsToNumber(af::array &output, af::array const &numLengths, af::array const &points, af::dtype type, unsigned int maximum) {

    auto exponent = batchFunc(flip(range(dim4(maximum), 0, s32), 0) - maximum, numLengths, batchAdd).as(s32);
    exponent = batchFunc(exponent, points, batchSub);
    {
        auto tmp = where(exponent < 0);
        if (!tmp.isempty()) exponent(tmp) += 1;
    }
    exponent = pow(10, exponent.as(type));
    exponent.eval();
    output(output == 255) = 0;
    output *= exponent;
    output = sum(output, 0).as(type);
}

af::array TPCDI_Utils::stringToNum(af::array &numstr, af::dtype type) {
    if (!numstr.dims(1)) return array(0, type);
    auto lengths = numstr;
    lengths(where(lengths)) = 255;
    auto idx = where(lengths == 0);
    lengths(idx) = (idx % lengths.dims(0)).as(u8);
    lengths = min(lengths, 0);

    af::array output = numstr.rows(0, end - 1); // removes delimiter
    output(where(output == ' ')) = '0';
    auto const maximum = output.dims(0);
    if (!maximum) return constant(0, numstr.dims(1), type);

    auto negatives = getNegatives(output, maximum);
    auto points = getDecimal(output, maximum);
    output(where(output >= '0' && output <= '9')) -= '0';

    digitsToNumber(output, lengths, points, type, maximum);
    if (!negatives.isempty()) output(negatives) *= -1;

    output.eval();
    return output;
}

af::array TPCDI_Utils::flipdims(af::array const &arr) {
    return moddims(arr, af::dim4(arr.dims(1), arr.dims(0)));
}

af::array TPCDI_Utils::stringToDate(af::array const &datestr, bool isDelimited, DateFormat dateFormat) {
    if (!datestr.dims(1)) return array(3, 0, u16);

    af::array out = datestr.rows(0, end - 1);
    out(span, where(allTrue(out == ' ', 0))) = '0';
    out(where(out >= '0' && out <='9')) -= '0';
    if (isDelimited) {
        auto delims = dateDelimIndices(dateFormat);
        out(seq(delims.first, delims.second, delims.second - delims.first), span) = 255;
        out = out(where(out >= 0 && out <= 9));
    }
    out = moddims(out, dim4(8, out.elements() / 8));
    out = batchFunc(out, flip(pow(10, range(dim4(8, 1), 0, u32)), 0), batchMult);
    out = sum(out, 0);
    out = dehashDate(out, dateFormat);
    out.eval();

    return out;
}

af::array TPCDI_Utils::dehashDate(af::array const &dateHash, DateFormat format) {
    switch (format) {
        case YYYYMMDD:
            return join(0, dateHash / 10000, dateHash % 10000 / 100, dateHash % 100).as(u16);
        case YYYYDDMM:
            return join(0, dateHash / 10000, dateHash % 100, dateHash % 10000 / 100).as(u16);
        case MMDDYYYY:
            return join(0, dateHash % 10000, dateHash / 1000000, dateHash % 10000 % 100).as(u16);
        case DDMMYYYY:
            return join(0, dateHash % 10000, dateHash % 10000 % 100, dateHash / 1000000).as(u16);
        default:
            throw std::runtime_error("No such date format");
    }
}

array TPCDI_Utils::stringToTime(af::array const &timestr, bool isDelimited) {
    if (!timestr.dims(1)) return array(3, 0, u16);

    af::array out = timestr.rows(0, end - 1);
    out(where(allTrue(out == 0 || out == ' ', 0))) = '0';
    out(where(out >= '0' && out <='9')) -= '0';

    if (isDelimited) out(seq(2, 5, 3), span) = 255;

    out = moddims(out(where(out >= 0 && out <= 9)), dim4(6, out.elements() / 6));
    out = batchFunc(out, flip(pow(10, range(dim4(6, 1), 0, u32)), 0), batchMult);
    out = sum(out, 0);
    out = join(0, out / 10000, out % 10000 / 100, out % 100).as(u16);
    out.eval();

    return out;
}

af::array TPCDI_Utils::stringToDateTime(af::array &datetimestr, bool isDelimited, DateFormat dateFormat) {
    if (!datetimestr.dims(1)) return array(6, 0, u16);
    af::array date = datetimestr.rows(0, isDelimited ? 11 : 9);
    date = stringToDate(date, isDelimited, dateFormat);

    af::array time = datetimestr.rows(end - (isDelimited ? 8 : 6), end);
    time = stringToTime(time, isDelimited);

    return join(0, date, time);
}

af::array TPCDI_Utils::stringToBoolean(af::array &boolstr) {
    if (!boolstr.dims(1)) return array(0, b8);
    array out = boolstr.row(0);
    out(out == 'T' || out == 't' || out == '1' || out =='Y' || out == 'y' || out != 0) = 1;
    out(out != 1) = 0;
    out = moddims(out, dim4(out.dims(1), out.dims(0))).as(b8);
    out.eval();
    return out;
}

af::array TPCDI_Utils::polyHash(array const &column) {
    uint64_t const prime = 67llU;
    auto hash = range(dim4(column.dims(0),1), 0, u64);
    hash = pow(prime, hash).as(u64);
    hash = batchFunc(column, hash, batchMult);
    hash = sum(hash, 0);
    hash.eval();
    if (hash.type() != u64) hash = hash.as(u64);
    return hash;
}

af::array TPCDI_Utils::dateHash(const array &date) {
    auto mult = flip(pow(100, range(dim4(3,1), 0, u32)), 0);
    auto key = batchFunc(mult, date, batchMult);
    key = sum(key, 0);
    return key;
}

af::array TPCDI_Utils::datetimeHash(af::array const &datetime) {
    auto mult = flip(pow(100U, range(dim4(6,1), 0, u64)), 0);
    auto key = batchFunc(mult, datetime.as(u64), batchMult);
    key = sum(key, 0);
    return key;
}

af::array TPCDI_Utils::prefixHash(array const &column) {
    if (column.type() != u8) throw std::invalid_argument("Unexpected array type, input must be unsigned char");
    auto const n = column.dims(0);
    if (!n) throw std::runtime_error("Cannot hash null column");
    auto const r = n % 8;
    auto const h = n / 8 + 1 * (r != 0);
    auto const s1 = flip(range(dim4(8), 0, u64),0) * 8;
    auto out = n < 8 ? sum(batchFunc(column.rows(0, n - 1), s1.rows(0, n - 1), bitShiftLeft), 0) :
               sum(batchFunc(column.rows(0, 7), s1, bitShiftLeft), 0);
    for (int i = 1; i < h; ++i) {
        auto e = i * 8 + 7;
        if (e < n) out = join(0, out, sum(batchFunc(column.rows(i * 8, e), s1, bitShiftLeft), 0));
        else out = join(0, out, sum(batchFunc(column.rows(i * 8, i * 8 + r), s1.rows(0, r - 1), bitShiftLeft), 0));
    }
    if (out.type() != u64) out = out.as(u64);
    out.eval();
    return out;
}