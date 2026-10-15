// Standalone test tooling. Tint owns its allocations; the production BWSL
// backend does not depend on this file or on Tint.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/tint/api/helpers/generate_bindings.h"
#include "src/tint/api/tint.h"
#include "src/tint/lang/spirv/writer/writer.h"
#include "src/tint/lang/wgsl/inspector/inspector.h"
#include "src/tint/lang/wgsl/reader/reader.h"

struct Input {
    std::string source;
    std::vector<uint32_t> words;
};

struct Output {
    std::string source;
    std::vector<uint32_t> words;

    size_t ByteSize() const {
        return source.size() + words.size() * sizeof(uint32_t);
    }
};

class TintSession {
public:
    TintSession() { tint::Initialize(); }
    ~TintSession() { tint::Shutdown(); }
};

static Input ReadInput(const std::string& path, bool binary) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open " + path);
    }
    Input input;
    input.source.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (binary) {
        if (input.source.empty() || input.source.size() % sizeof(uint32_t) != 0) {
            throw std::runtime_error("SPIR-V size must be a nonzero multiple of four");
        }
        input.words.resize(input.source.size() / sizeof(uint32_t));
        std::memcpy(input.words.data(), input.source.data(), input.source.size());
    }
    return input;
}

static Output Compile(const Input& input, const std::string& mode) {
    Output output;
    if (mode == "spv-to-wgsl") {
        tint::Result<std::string> result = tint::SpirvToWgsl(input.words);
        if (result != tint::Success) {
            throw std::runtime_error(result.Failure().reason);
        }
        output.source = std::move(result.Get());
        return output;
    }

    tint::Source::File file("benchmark.wgsl", input.source);
    tint::Program program = tint::wgsl::reader::Parse(&file);
    if (!program.IsValid()) {
        throw std::runtime_error(program.Diagnostics().Str());
    }
    if (mode == "validate") {
        return output;
    }
    if (mode != "wgsl-to-spv") {
        throw std::runtime_error("Expected validate, wgsl-to-spv, or spv-to-wgsl");
    }
    tint::inspector::Inspector inspector(program);
    std::vector<tint::inspector::EntryPoint> entryPoints = inspector.GetEntryPoints();
    if (entryPoints.size() != 1) {
        throw std::runtime_error("Benchmark fixtures require exactly one entry point");
    }
    tint::Result<tint::core::ir::Module> ir = tint::wgsl::reader::ProgramToLoweredIR(program);
    if (ir != tint::Success) {
        throw std::runtime_error(ir.Failure().reason);
    }
    tint::spirv::writer::Options options;
    options.entry_point_name = entryPoints.front().name;
    options.bindings = tint::GenerateBindings(ir.Get(), options.entry_point_name, false, false);
    tint::Result<tint::spirv::writer::Output> result =
        tint::spirv::writer::Generate(ir.Get(), options);
    if (result != tint::Success) {
        throw std::runtime_error(result.Failure().reason);
    }
    output.words = std::move(result.Get().spirv);
    return output;
}

static double Percentile(const std::vector<double>& sorted, double fraction) {
    double rank = static_cast<double>(sorted.size() - 1) * fraction;
    size_t low = static_cast<size_t>(rank);
    size_t high = std::min(low + 1, sorted.size() - 1);
    return sorted[low] + (sorted[high] - sorted[low]) * (rank - static_cast<double>(low));
}

static int Benchmark(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        throw std::runtime_error("Usage: bwsl-tint-bench bench INPUT MODE ITERATIONS [WARMUP]");
    }
    std::string mode = argv[3];
    int iterations = std::stoi(argv[4]);
    int warmup = argc == 6 ? std::stoi(argv[5]) : 5;
    if (iterations < 1 || warmup < 0 || iterations > 1000000 || warmup > 1000000) {
        throw std::runtime_error("Invalid benchmark iteration count");
    }
    Input input = ReadInput(argv[2], mode == "spv-to-wgsl");
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    size_t outputBytes = 0;
    for (int i = -warmup; i < iterations; ++i) {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        {
            Output output = Compile(input, mode);
            outputBytes = output.ByteSize();
        }
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        if (i >= 0) {
            samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::cout << std::setprecision(12)
              << "{\"mode\":\"" << mode << "\",\"tint_revision\":\"" << BWSL_TINT_REVISION
              << "\",\"iterations\":" << iterations << ",\"warmup\":" << warmup
              << ",\"input_bytes\":" << input.source.size()
              << ",\"output_bytes\":" << outputBytes
              << ",\"median_us\":" << Percentile(sorted, 0.5)
              << ",\"min_us\":" << sorted.front()
              << ",\"p95_us\":" << sorted[(sorted.size() * 95 + 99) / 100 - 1]
              << ",\"timing_scope\":\"in_process_parse_validate_emit_and_destroy\""
              << ",\"robustness\":\"Tint writer defaults\",\"samples_us\":[";
    for (size_t i = 0; i < samples.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << samples[i];
    }
    std::cout << "]}\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        TintSession session;
        if (argc >= 2 && std::string(argv[1]) == "bench") {
            return Benchmark(argc, argv);
        }
        if (argc == 3 && std::string(argv[1]) == "validate") {
            Compile(ReadInput(argv[2], false), "validate");
            std::cout << "{\"valid\":true,\"tint_revision\":\""
                      << BWSL_TINT_REVISION << "\"}\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "translate") {
            std::string sourcePath = argv[2];
            bool binary = sourcePath.ends_with(".spv");
            Output output = Compile(ReadInput(sourcePath, binary),
                                    binary ? "spv-to-wgsl" : "wgsl-to-spv");
            std::ofstream stream(argv[3], std::ios::binary);
            if (!stream) {
                throw std::runtime_error("Cannot create output file");
            }
            if (binary) {
                stream.write(output.source.data(), static_cast<std::streamsize>(output.source.size()));
            } else {
                stream.write(reinterpret_cast<const char*>(output.words.data()),
                             static_cast<std::streamsize>(output.words.size() * sizeof(uint32_t)));
            }
            if (!stream) {
                throw std::runtime_error("Could not write complete output");
            }
            return 0;
        }
        throw std::runtime_error(
            "Usage: bwsl-tint-bench {validate INPUT | translate INPUT OUTPUT | "
            "bench INPUT MODE ITERATIONS [WARMUP]}");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
