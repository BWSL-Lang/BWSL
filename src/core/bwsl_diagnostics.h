#pragma once

#include "bwsl_ast_common.h"
#include "bwsl_defs.h"
#include "bwsl_utils.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace BWSL {

using DiagnosticRef = u32;
static constexpr DiagnosticRef INVALID_DIAGNOSTIC = 0xFFFFFFFFu;
static constexpr u32 INVALID_DIAGNOSTIC_STRING = 0xFFFFFFFFu;
static constexpr u32 DIAGNOSTIC_COMMON_STRING_FLAG = 0x80000000u;
static constexpr u32 DIAGNOSTIC_STRING_INDEX_MASK = 0x7FFFFFFFu;
static constexpr u32 INVALID_DIAGNOSTIC_TOKEN = 0xFFFFFFFFu;

enum class DiagnosticSeverity : u8 {
    Error = 0,
    Warning = 1,
    Note = 2,
    Hint = 3,
};

enum class DiagnosticPhase : u8 {
    CLI = 0,
    Lex = 1,
    Parse = 2,
    Variant = 3,
    Comptime = 4,
    Lowering = 5,
    Compile = 6,
    Validation = 7,
    IO = 8,
    ComputeGraph = 9,
    Internal = 10,
};

enum class DiagnosticMessageId : u16 {
    Raw = 0,
    NoInputFile,
    VariantExpectsNameValue,
    ValidationModeExpected,
    UnknownOption,
    CouldNotReadFile,
    NoPipelineFound,
    CouldNotWriteSpirv,
    ShaderStageCompileFailed,
    ValidationStrictToolMissing,
    ValidationAutoToolMissing,
    ValidationFailed,
    UnhandledException,
    UnhandledUnknownException,
    ParseError,
    ComptimeError,
    VariantResolutionFailed,
    VariantSelectionFailed,
    VariantReflectionFailed,
    VariantSpecializationFailed,
    LoweringError,
    CrossCompileFailed,
    MaxUserVaryingsExceeded,
    ComputeGraphValidationFailed,
};

enum class DiagnosticArgKind : u8 {
    String = 0,
    Int = 1,
    UInt = 2,
};

struct DiagnosticArg {
    DiagnosticArgKind kind = DiagnosticArgKind::String;
    std::string stringValue;
    s64 intValue = 0;
    u64 uintValue = 0;

    static DiagnosticArg String(std::string value) {
        DiagnosticArg arg;
        arg.kind = DiagnosticArgKind::String;
        arg.stringValue = std::move(value);
        return arg;
    }

    static DiagnosticArg Int(s64 value) {
        DiagnosticArg arg;
        arg.kind = DiagnosticArgKind::Int;
        arg.intValue = value;
        return arg;
    }

    static DiagnosticArg UInt(u64 value) {
        DiagnosticArg arg;
        arg.kind = DiagnosticArgKind::UInt;
        arg.uintValue = value;
        return arg;
    }
};

struct DiagnosticSpan {
    enum Flag : u32 {
        HasLocationFlag = 1u << 0,
        HasEndLocationFlag = 1u << 1,
        HasOffsetFlag = 1u << 2,
    };

    u32 offset = 0;
    u32 length = 0;
    u32 line = 0;
    u32 column = 0;
    u32 endLine = 0;
    u32 endColumn = 0;
    u32 token = INVALID_DIAGNOSTIC_TOKEN;
    u32 flags = 0;

    static bool HasAll(u32 packedFlags, u32 mask) { return (packedFlags & mask) == mask; }
    static bool HasAny(u32 packedFlags, u32 mask) { return (packedFlags & mask) != 0; }

    bool HasLocation() const { return HasFlags(HasLocationFlag); }
    bool HasEndLocation() const { return HasFlags(HasEndLocationFlag); }
    bool HasOffset() const { return HasFlags(HasOffsetFlag); }
    bool HasFlags(u32 mask) const { return HasAll(flags, mask); }
    bool HasAnyFlags(u32 mask) const { return HasAny(flags, mask); }

    void SetFlags(u32 mask) { flags |= mask; }
    void ClearFlags(u32 mask) { flags &= ~mask; }
    void SetLocation() { SetFlags(HasLocationFlag); }
    void SetEndLocation() { SetFlags(HasEndLocationFlag); }
    void SetOffset() { SetFlags(HasOffsetFlag); }
};

class DiagnosticStream {
public:
    ArenaArray<u8> severities;
    ArenaArray<u8> phases;
    ArenaArray<u16> messageIds;
    ArenaArray<u32> rawMessageStringIds;
    ArenaArray<u32> fileStringIds;
    ArenaArray<u32> passStringIds;
    ArenaArray<u32> stageStringIds;
    ArenaArray<u32> offsets;
    ArenaArray<u32> lengths;
    ArenaArray<u32> lines;
    ArenaArray<u32> columns;
    ArenaArray<u32> endLines;
    ArenaArray<u32> endColumns;
    ArenaArray<u32> tokens;
    ArenaArray<u32> spanFlags;
    ArenaArray<u32> argStarts;
    ArenaArray<u16> argCounts;
    ArenaArray<u8> argKinds;
    ArenaArray<u32> argStringIds;
    ArenaArray<s64> argIntValues;
    ArenaArray<u64> argUIntValues;

    void Init(BWSL_Arena* arena_,
              u32 initialDiagnosticCapacity = 16,
              u32 initialStringCapacity = 32,
              u32 initialArgCapacity = 16) {
        arena = arena_;
        if (!arena) return;

        severities.Init(arena, initialDiagnosticCapacity);
        phases.Init(arena, initialDiagnosticCapacity);
        messageIds.Init(arena, initialDiagnosticCapacity);
        rawMessageStringIds.Init(arena, initialDiagnosticCapacity);
        fileStringIds.Init(arena, initialDiagnosticCapacity);
        passStringIds.Init(arena, initialDiagnosticCapacity);
        stageStringIds.Init(arena, initialDiagnosticCapacity);
        offsets.Init(arena, initialDiagnosticCapacity);
        lengths.Init(arena, initialDiagnosticCapacity);
        lines.Init(arena, initialDiagnosticCapacity);
        columns.Init(arena, initialDiagnosticCapacity);
        endLines.Init(arena, initialDiagnosticCapacity);
        endColumns.Init(arena, initialDiagnosticCapacity);
        tokens.Init(arena, initialDiagnosticCapacity);
        spanFlags.Init(arena, initialDiagnosticCapacity);
        argStarts.Init(arena, initialDiagnosticCapacity);
        argCounts.Init(arena, initialDiagnosticCapacity);
        argKinds.Init(arena, initialArgCapacity);
        argStringIds.Init(arena, initialArgCapacity);
        argIntValues.Init(arena, initialArgCapacity);
        argUIntValues.Init(arena, initialArgCapacity);
        (void)initialStringCapacity;
        dynamicStrings.clear();
    }

    void Clear() {
        ClearArray(severities);
        ClearArray(phases);
        ClearArray(messageIds);
        ClearArray(rawMessageStringIds);
        ClearArray(fileStringIds);
        ClearArray(passStringIds);
        ClearArray(stageStringIds);
        ClearArray(offsets);
        ClearArray(lengths);
        ClearArray(lines);
        ClearArray(columns);
        ClearArray(endLines);
        ClearArray(endColumns);
        ClearArray(tokens);
        ClearArray(spanFlags);
        ClearArray(argStarts);
        ClearArray(argCounts);
        ClearArray(argKinds);
        ClearArray(argStringIds);
        ClearArray(argIntValues);
        ClearArray(argUIntValues);
        dynamicStrings.clear();
    }

    void Reset() { Clear(); }

    u32 Count() const { return severities.count; }

    DiagnosticRef AddRaw(DiagnosticSeverity severity,
                         DiagnosticPhase phase,
                         const std::string& message,
                         const DiagnosticSpan& span = {},
                         const std::string& file = "",
                         const std::string& pass = "",
                         const std::string& stage = "",
                         DiagnosticMessageId messageId = DiagnosticMessageId::Raw) {
        return AddInternal(severity, phase, messageId, message,
                           span, file, pass, stage, {});
    }

    DiagnosticRef AddMessage(DiagnosticSeverity severity,
                             DiagnosticPhase phase,
                             DiagnosticMessageId messageId,
                             std::initializer_list<DiagnosticArg> args = {},
                             const DiagnosticSpan& span = {},
                             const std::string& file = "",
                             const std::string& pass = "",
                             const std::string& stage = "") {
        return AddInternal(severity, phase, messageId, "", span, file, pass, stage, args);
    }

    DiagnosticSeverity GetSeverity(DiagnosticRef ref) const {
        return static_cast<DiagnosticSeverity>(severities[ref]);
    }

    DiagnosticPhase GetPhase(DiagnosticRef ref) const {
        return static_cast<DiagnosticPhase>(phases[ref]);
    }

    DiagnosticMessageId GetMessageId(DiagnosticRef ref) const {
        return static_cast<DiagnosticMessageId>(messageIds[ref]);
    }

    std::string GetFile(DiagnosticRef ref) const { return GetString(fileStringIds[ref]); }
    std::string GetPass(DiagnosticRef ref) const { return GetString(passStringIds[ref]); }
    std::string GetStage(DiagnosticRef ref) const { return GetString(stageStringIds[ref]); }

    std::string FormatMessage(DiagnosticRef ref) const {
        DiagnosticMessageId id = GetMessageId(ref);
        std::string rawMessage = GetString(rawMessageStringIds[ref]);
        if (!rawMessage.empty()) {
            return rawMessage;
        }

        std::string result = TemplateFor(id);
        u32 start = argStarts[ref];
        u16 count = argCounts[ref];
        for (u16 i = 0; i < count; i++) {
            std::string placeholder = "{" + std::to_string(i) + "}";
            std::string value = FormatArg(start + i);
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.size(), value);
                pos += value.size();
            }
        }
        return result;
    }

    std::string GetCode(DiagnosticRef ref) const {
        return MessageCode(GetMessageId(ref), GetPhase(ref), FormatMessage(ref));
    }

    static const char* SeverityName(DiagnosticSeverity severity) {
        switch (severity) {
            case DiagnosticSeverity::Error: return "error";
            case DiagnosticSeverity::Warning: return "warning";
            case DiagnosticSeverity::Note: return "note";
            case DiagnosticSeverity::Hint: return "hint";
            default: return "error";
        }
    }

    static const char* PhaseName(DiagnosticPhase phase) {
        switch (phase) {
            case DiagnosticPhase::CLI: return "cli";
            case DiagnosticPhase::Lex: return "lex";
            case DiagnosticPhase::Parse: return "parse";
            case DiagnosticPhase::Variant: return "variant";
            case DiagnosticPhase::Comptime: return "comptime";
            case DiagnosticPhase::Lowering: return "lowering";
            case DiagnosticPhase::Compile: return "compile";
            case DiagnosticPhase::Validation: return "validation";
            case DiagnosticPhase::IO: return "io";
            case DiagnosticPhase::ComputeGraph: return "compute_graph";
            case DiagnosticPhase::Internal: return "internal";
            default: return "compile";
        }
    }

    static const char* MessageName(DiagnosticMessageId id) {
        switch (id) {
            case DiagnosticMessageId::Raw: return "Raw";
            case DiagnosticMessageId::NoInputFile: return "NoInputFile";
            case DiagnosticMessageId::VariantExpectsNameValue: return "VariantExpectsNameValue";
            case DiagnosticMessageId::ValidationModeExpected: return "ValidationModeExpected";
            case DiagnosticMessageId::UnknownOption: return "UnknownOption";
            case DiagnosticMessageId::CouldNotReadFile: return "CouldNotReadFile";
            case DiagnosticMessageId::NoPipelineFound: return "NoPipelineFound";
            case DiagnosticMessageId::CouldNotWriteSpirv: return "CouldNotWriteSpirv";
            case DiagnosticMessageId::ShaderStageCompileFailed: return "ShaderStageCompileFailed";
            case DiagnosticMessageId::ValidationStrictToolMissing: return "ValidationStrictToolMissing";
            case DiagnosticMessageId::ValidationAutoToolMissing: return "ValidationAutoToolMissing";
            case DiagnosticMessageId::ValidationFailed: return "ValidationFailed";
            case DiagnosticMessageId::UnhandledException: return "UnhandledException";
            case DiagnosticMessageId::UnhandledUnknownException: return "UnhandledUnknownException";
            case DiagnosticMessageId::ParseError: return "ParseError";
            case DiagnosticMessageId::ComptimeError: return "ComptimeError";
            case DiagnosticMessageId::VariantResolutionFailed: return "VariantResolutionFailed";
            case DiagnosticMessageId::VariantSelectionFailed: return "VariantSelectionFailed";
            case DiagnosticMessageId::VariantReflectionFailed: return "VariantReflectionFailed";
            case DiagnosticMessageId::VariantSpecializationFailed: return "VariantSpecializationFailed";
            case DiagnosticMessageId::LoweringError: return "LoweringError";
            case DiagnosticMessageId::CrossCompileFailed: return "CrossCompileFailed";
            case DiagnosticMessageId::MaxUserVaryingsExceeded: return "MaxUserVaryingsExceeded";
            case DiagnosticMessageId::ComputeGraphValidationFailed: return "ComputeGraphValidationFailed";
            default: return "Raw";
        }
    }

    static std::string MessageCode(DiagnosticMessageId id,
                                   DiagnosticPhase phase = DiagnosticPhase::Internal,
                                   std::string_view message = {}) {
        const char* base = MessageCodeBase(id);
        if (!MessageCodeUsesFingerprint(id)) {
            return base;
        }

        u32 fingerprint = HashDiagnosticIdentity(id, phase, message);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%s-%08X", base, fingerprint);
        return buffer;
    }

private:
    BWSL_Arena* arena = nullptr;
    std::deque<std::string> dynamicStrings;

    template<typename T>
    static void ClearArray(ArenaArray<T>& array) {
        array.count = 0;
    }

    template<typename T>
    void Push(ArenaArray<T>& array, const T& value) {
        array.Push(arena, value);
    }

    DiagnosticRef AddInternal(DiagnosticSeverity severity,
                              DiagnosticPhase phase,
                              DiagnosticMessageId messageId,
                              const std::string& rawMessage,
                              const DiagnosticSpan& span,
                              const std::string& file,
                              const std::string& pass,
                              const std::string& stage,
                              std::initializer_list<DiagnosticArg> args) {
        if (!arena) return INVALID_DIAGNOSTIC;

        DiagnosticRef ref = Count();
        Push(severities, static_cast<u8>(severity));
        Push(phases, static_cast<u8>(phase));
        Push(messageIds, static_cast<u16>(messageId));
        Push(rawMessageStringIds, rawMessage.empty() ? INVALID_DIAGNOSTIC_STRING : InternString(rawMessage));
        Push(fileStringIds, file.empty() ? INVALID_DIAGNOSTIC_STRING : InternString(file));
        Push(passStringIds, pass.empty() ? INVALID_DIAGNOSTIC_STRING : InternString(pass));
        Push(stageStringIds, stage.empty() ? INVALID_DIAGNOSTIC_STRING : InternString(stage));
        Push(offsets, span.offset);
        Push(lengths, span.length);
        Push(lines, span.line);
        Push(columns, span.column);
        Push(endLines, span.endLine);
        Push(endColumns, span.endColumn);
        Push(tokens, span.token);
        Push(spanFlags, span.flags);
        Push(argStarts, argKinds.count);
        Push(argCounts, static_cast<u16>(args.size()));

        for (const DiagnosticArg& arg : args) {
            Push(argKinds, static_cast<u8>(arg.kind));
            Push(argStringIds, arg.kind == DiagnosticArgKind::String
                                   ? InternString(arg.stringValue)
                                   : INVALID_DIAGNOSTIC_STRING);
            Push(argIntValues, arg.intValue);
            Push(argUIntValues, arg.uintValue);
        }

        return ref;
    }

    static u32 MakeCommonStringId(u32 index) {
        return DIAGNOSTIC_COMMON_STRING_FLAG | index;
    }

    static bool IsCommonStringId(u32 stringId) {
        return stringId != INVALID_DIAGNOSTIC_STRING &&
               (stringId & DIAGNOSTIC_COMMON_STRING_FLAG) != 0;
    }

    static const char* CommonString(u32 index) {
        static constexpr const char* table[] = {
            "unknown",
            "vertex",
            "fragment",
            "compute",
            "No input file specified",
            "-variant expects name=value",
            "-validation expects auto, strict, or off",
            "Could not write SPIR-V file",
            "Shader stage compilation failed",
            "Unhandled unknown exception",
            "Expected ';' after expression",
            "Comptime interpretation failed",
            "Variant selection failed",
            "Variant reflection failed",
            "Variant specialization failed",
            "Compute graph validation failed",
            "Maximum 16 user varyings exceeded",
        };

        return index < (sizeof(table) / sizeof(table[0])) ? table[index] : "";
    }

    static u32 FindCommonString(std::string_view value) {
        for (u32 i = 0;; i++) {
            const char* common = CommonString(i);
            if (!common || common[0] == '\0') break;
            size_t commonLength = std::strlen(common);
            if (commonLength == value.size() &&
                (value.empty() || std::memcmp(common, value.data(), commonLength) == 0)) {
                return i;
            }
        }
        return INVALID_DIAGNOSTIC_STRING;
    }

    u32 InternString(const std::string& value) {
        return InternString(std::string_view(value.data(), value.size()));
    }

    u32 InternString(std::string_view value) {
        u32 commonIndex = FindCommonString(value);
        if (commonIndex != INVALID_DIAGNOSTIC_STRING) {
            return MakeCommonStringId(commonIndex);
        }

        for (u32 i = 0; i < dynamicStrings.size(); i++) {
            if (dynamicStrings[i].size() == value.size() &&
                (value.empty() || std::memcmp(dynamicStrings[i].data(), value.data(), value.size()) == 0)) {
                return i;
            }
        }

        u32 id = static_cast<u32>(dynamicStrings.size());
        dynamicStrings.emplace_back(value.data(), value.size());
        return id;
    }

    std::string GetString(u32 stringId) const {
        if (stringId == INVALID_DIAGNOSTIC_STRING) return "";
        if (IsCommonStringId(stringId)) {
            return CommonString(stringId & DIAGNOSTIC_STRING_INDEX_MASK);
        }
        if (stringId >= dynamicStrings.size()) return "";
        return dynamicStrings[stringId];
    }

    std::string FormatArg(u32 argIndex) const {
        if (argIndex >= argKinds.count) return "";
        DiagnosticArgKind kind = static_cast<DiagnosticArgKind>(argKinds[argIndex]);
        switch (kind) {
            case DiagnosticArgKind::String:
                return GetString(argStringIds[argIndex]);
            case DiagnosticArgKind::Int:
                return std::to_string(argIntValues[argIndex]);
            case DiagnosticArgKind::UInt:
                return std::to_string(argUIntValues[argIndex]);
            default:
                return "";
        }
    }

    static const char* TemplateFor(DiagnosticMessageId id) {
        switch (id) {
            case DiagnosticMessageId::NoInputFile:
                return "No input file specified";
            case DiagnosticMessageId::VariantExpectsNameValue:
                return "-variant expects name=value";
            case DiagnosticMessageId::ValidationModeExpected:
                return "-validation expects auto, strict, or off";
            case DiagnosticMessageId::UnknownOption:
                return "Unknown option: {0}";
            case DiagnosticMessageId::CouldNotReadFile:
                return "Could not read file '{0}'";
            case DiagnosticMessageId::NoPipelineFound:
                return "No pipeline found in '{0}'";
            case DiagnosticMessageId::CouldNotWriteSpirv:
                return "Could not write SPIR-V file";
            case DiagnosticMessageId::ShaderStageCompileFailed:
                return "Shader stage compilation failed";
            case DiagnosticMessageId::ValidationStrictToolMissing:
                return "SPIR-V validation requested but {0}";
            case DiagnosticMessageId::ValidationAutoToolMissing:
                return "{0}; skipping SPIR-V validation (-validation auto)";
            case DiagnosticMessageId::ValidationFailed:
                return "SPIR-V validation failed:\n{0}";
            case DiagnosticMessageId::UnhandledException:
                return "Unhandled exception: {0}";
            case DiagnosticMessageId::UnhandledUnknownException:
                return "Unhandled unknown exception";
            case DiagnosticMessageId::ParseError:
                return "Parse error";
            case DiagnosticMessageId::ComptimeError:
                return "Comptime interpretation failed";
            case DiagnosticMessageId::VariantResolutionFailed:
                return "Variant resolution failed: {0}";
            case DiagnosticMessageId::VariantSelectionFailed:
                return "Variant selection failed: {0}";
            case DiagnosticMessageId::VariantReflectionFailed:
                return "Variant reflection failed: {0}";
            case DiagnosticMessageId::VariantSpecializationFailed:
                return "Variant specialization failed: {0}";
            case DiagnosticMessageId::LoweringError:
                return "Lowering failed";
            case DiagnosticMessageId::CrossCompileFailed:
                return "{0} cross-compilation failed";
            case DiagnosticMessageId::MaxUserVaryingsExceeded:
                return "Maximum 16 user varyings exceeded";
            case DiagnosticMessageId::ComputeGraphValidationFailed:
                return "Compute graph validation failed: {0}";
            default:
                return "";
        }
    }

    static const char* MessageCodeBase(DiagnosticMessageId id) {
        switch (id) {
            case DiagnosticMessageId::Raw: return "BWSL0000";
            case DiagnosticMessageId::NoInputFile: return "BWSL0100";
            case DiagnosticMessageId::VariantExpectsNameValue: return "BWSL0101";
            case DiagnosticMessageId::ValidationModeExpected: return "BWSL0102";
            case DiagnosticMessageId::UnknownOption: return "BWSL0103";
            case DiagnosticMessageId::CouldNotReadFile: return "BWSL0200";
            case DiagnosticMessageId::CouldNotWriteSpirv: return "BWSL0201";
            case DiagnosticMessageId::ParseError: return "BWSL1000";
            case DiagnosticMessageId::NoPipelineFound: return "BWSL1001";
            case DiagnosticMessageId::ComptimeError: return "BWSL1100";
            case DiagnosticMessageId::VariantResolutionFailed: return "BWSL1200";
            case DiagnosticMessageId::VariantSelectionFailed: return "BWSL1201";
            case DiagnosticMessageId::VariantReflectionFailed: return "BWSL1202";
            case DiagnosticMessageId::VariantSpecializationFailed: return "BWSL1203";
            case DiagnosticMessageId::LoweringError: return "BWSL1300";
            case DiagnosticMessageId::MaxUserVaryingsExceeded: return "BWSL1301";
            case DiagnosticMessageId::ShaderStageCompileFailed: return "BWSL1400";
            case DiagnosticMessageId::CrossCompileFailed: return "BWSL1401";
            case DiagnosticMessageId::ValidationStrictToolMissing: return "BWSL1500";
            case DiagnosticMessageId::ValidationAutoToolMissing: return "BWSL1501";
            case DiagnosticMessageId::ValidationFailed: return "BWSL1502";
            case DiagnosticMessageId::ComputeGraphValidationFailed: return "BWSL1600";
            case DiagnosticMessageId::UnhandledException: return "BWSL9000";
            case DiagnosticMessageId::UnhandledUnknownException: return "BWSL9001";
            default: return "BWSL0000";
        }
    }

    static bool MessageCodeUsesFingerprint(DiagnosticMessageId id) {
        switch (id) {
            case DiagnosticMessageId::Raw:
            case DiagnosticMessageId::ParseError:
            case DiagnosticMessageId::ComptimeError:
            case DiagnosticMessageId::LoweringError:
                return true;
            default:
                return false;
        }
    }

    static u32 HashDiagnosticIdentity(DiagnosticMessageId id,
                                      DiagnosticPhase phase,
                                      std::string_view message) {
        u32 hash = Utils::FNV_OFFSET_BASIS;
        hash = Utils::HashMix(hash, static_cast<u8>(id));
        hash = Utils::HashMix(hash, static_cast<u8>(phase));
        for (char ch : message) {
            hash = Utils::HashMix(hash, static_cast<u8>(ch));
        }
        return hash == 0 ? 1u : hash;
    }
};

} // namespace BWSL
