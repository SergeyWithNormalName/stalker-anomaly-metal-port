#pragma once

#include <cstddef>
#include <cstdlib>
#include <string_view>

enum class R4BackendKind
{
    Dxvk,
    Metal,
};

enum class R4BackendSelectionError
{
    None,
    MissingValue,
    UnknownValue,
    DuplicateOption,
};

struct R4BackendSelectionResult
{
    R4BackendKind backend = R4BackendKind::Dxvk;
    R4BackendSelectionError error = R4BackendSelectionError::None;
};

namespace R4BackendSelectionDetail
{
inline bool isWhitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

inline std::string_view nextToken(std::string_view commandLine, std::size_t& cursor)
{
    while (cursor < commandLine.size() && isWhitespace(commandLine[cursor]))
        ++cursor;
    const std::size_t begin = cursor;
    while (cursor < commandLine.size() && !isWhitespace(commandLine[cursor]))
        ++cursor;
    return commandLine.substr(begin, cursor - begin);
}
}

inline R4BackendSelectionResult ParseR4BackendSelection(const char* rawCommandLine)
{
    const std::string_view commandLine = rawCommandLine ? rawCommandLine : "";
    constexpr std::string_view option = "-render_backend";
    R4BackendSelectionResult result;
    bool found = false;
    std::size_t cursor = 0;

    while (cursor < commandLine.size())
    {
        const std::string_view token = R4BackendSelectionDetail::nextToken(commandLine, cursor);
        if (token.empty())
            break;
        if (token != option)
        {
            if (token.size() > option.size() && token.substr(0, option.size()) == option &&
                token[option.size()] == '=')
                result.error = R4BackendSelectionError::UnknownValue;
            if (result.error != R4BackendSelectionError::None)
                return result;
            continue;
        }
        if (found)
        {
            result.error = R4BackendSelectionError::DuplicateOption;
            return result;
        }

        const std::string_view value = R4BackendSelectionDetail::nextToken(commandLine, cursor);
        if (value.empty())
        {
            result.error = R4BackendSelectionError::MissingValue;
            return result;
        }
        if (value == "dxvk")
            result.backend = R4BackendKind::Dxvk;
        else if (value == "metal")
            result.backend = R4BackendKind::Metal;
        else
        {
            result.error = R4BackendSelectionError::UnknownValue;
            return result;
        }
        found = true;
    }

    return result;
}

inline const char* R4BackendName(R4BackendKind backend)
{
    return backend == R4BackendKind::Metal ? "metal" : "dxvk";
}

inline const char* R4BackendSelectionErrorMessage(R4BackendSelectionError error)
{
    switch (error)
    {
    case R4BackendSelectionError::None:
        return "none";
    case R4BackendSelectionError::MissingValue:
        return "-render_backend requires dxvk or metal";
    case R4BackendSelectionError::UnknownValue:
        return "unknown -render_backend value; expected dxvk or metal";
    case R4BackendSelectionError::DuplicateOption:
        return "-render_backend may be specified only once";
    }
    return "unknown backend selection error";
}

inline R4BackendKind& SelectedR4BackendStorage()
{
    static R4BackendKind backend = R4BackendKind::Dxvk;
    return backend;
}

inline void SetSelectedR4Backend(R4BackendKind backend)
{
    SelectedR4BackendStorage() = backend;
}

inline R4BackendKind SelectedR4Backend()
{
    return SelectedR4BackendStorage();
}
