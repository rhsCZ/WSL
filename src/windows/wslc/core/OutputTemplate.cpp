/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputTemplate.cpp

Abstract:

    Implementation of OutputTemplate. The engine compiles template text into a node
    list once and then renders it against each record. Records are JSON, so the
    evaluator uses nlohmann::json as its dynamic value type throughout.

--*/

#include "precomp.h"
#include "OutputTemplate.h"
#include "Exceptions.h"
#include "Localization.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <limits>
#include <string_view>

using wsl::shared::Localization;
using wsl::shared::string::MultiByteToWide;
using wsl::shared::string::WideToMultiByte;

namespace wsl::windows::wslc {

namespace templates {

    // One term inside a template action: a constant, a lookup on the record, or a function
    // applied to other terms.
    struct Expression
    {
        enum class Kind
        {
            Literal,
            Field,
            Call,
        };

        Kind kind = Kind::Literal;

        // Kind::Literal
        nlohmann::json value;

        // Kind::Field. The dotted path to walk from the record; empty means the record itself.
        std::vector<std::string> path;

        // Kind::Field. When set, the path is walked from this term's result instead of the record.
        std::shared_ptr<const Expression> source;

        // Kind::Call
        std::string function;
        std::vector<std::shared_ptr<const Expression>> arguments;
    };

    using ExpressionPtr = std::shared_ptr<const Expression>;

    // One element of a compiled template.
    struct Node
    {
        enum class Kind
        {
            Text,
            Action,
            Conditional,
        };

        Kind kind = Kind::Text;

        // Kind::Text
        std::string text;

        // Kind::Action and Kind::Conditional
        ExpressionPtr expression;

        // Kind::Conditional
        NodeList thenNodes;
        NodeList elseNodes;
    };

} // namespace templates

namespace {

    using namespace templates;

    constexpr std::string_view c_missingValueText = "<no value>";
    constexpr std::string_view c_actionStart = "{{";
    constexpr std::string_view c_actionEnd = "}}";

    // Keywords of Go's template language that this engine deliberately does not implement.
    // They are rejected by name so the user gets a precise message instead of a parse failure.
    constexpr std::string_view c_unsupportedKeywords[] = {"range", "with", "template", "define", "block", "break", "continue"};

    bool IsSpace(char value) noexcept
    {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    }

    bool IsDigit(char value) noexcept
    {
        return value >= '0' && value <= '9';
    }

    bool IsAlpha(char value) noexcept
    {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    }

    bool IsIdentifierStart(char value) noexcept
    {
        return IsAlpha(value) || value == '_';
    }

    bool IsIdentifierCharacter(char value) noexcept
    {
        return IsIdentifierStart(value) || IsDigit(value);
    }

    std::string_view Trim(std::string_view value)
    {
        while (!value.empty() && IsSpace(value.front()))
        {
            value.remove_prefix(1);
        }

        while (!value.empty() && IsSpace(value.back()))
        {
            value.remove_suffix(1);
        }

        return value;
    }

    // Splits leading identifier characters off a trimmed action body so keywords can be
    // recognized without tokenizing the whole body first.
    std::string_view FirstWord(std::string_view value)
    {
        size_t length = 0;
        while (length < value.size() && IsIdentifierCharacter(value[length]))
        {
            ++length;
        }

        return value.substr(0, length);
    }

    [[noreturn]] void ThrowArgumentType(const std::string& functionName)
    {
        throw ExecutionException(Localization::WSLCCLI_TemplateFunctionArgumentTypeError(MultiByteToWide(functionName)));
    }

    [[noreturn]] void ThrowUnknownField(std::string_view field)
    {
        throw ExecutionException(Localization::WSLCCLI_TemplateUnknownFieldError(MultiByteToWide(std::string(field))));
    }

    // Renders a value the way Go's text/template prints it. Composite values have no Go
    // syntax equivalent here, so they fall back to their JSON encoding.
    std::string ToDisplayString(const nlohmann::json& value)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }

        if (value.is_null())
        {
            return std::string(c_missingValueText);
        }

        if (value.is_boolean())
        {
            return value.get<bool>() ? "true" : "false";
        }

        if (value.is_number_unsigned())
        {
            return std::to_string(value.get<unsigned long long>());
        }

        if (value.is_number_integer())
        {
            return std::to_string(value.get<long long>());
        }

        if (value.is_number_float())
        {
            return std::format("{}", value.get<double>());
        }

        return value.dump();
    }

    // Go treats the zero value of a type as false.
    bool IsTruthy(const nlohmann::json& value)
    {
        switch (value.type())
        {
        case nlohmann::json::value_t::boolean:
            return value.get<bool>();

        case nlohmann::json::value_t::string:
            return !value.get_ref<const std::string&>().empty();

        case nlohmann::json::value_t::number_integer:
            return value.get<long long>() != 0;

        case nlohmann::json::value_t::number_unsigned:
            return value.get<unsigned long long>() != 0;

        case nlohmann::json::value_t::number_float:
            return value.get<double>() != 0.0;

        case nlohmann::json::value_t::array:
        case nlohmann::json::value_t::object:
            return !value.empty();

        default:
            return false;
        }
    }

    // Scalars are coerced so that numeric fields can be passed to the text functions.
    std::string RequireString(const std::string& functionName, const nlohmann::json& value)
    {
        if (value.is_array() || value.is_object())
        {
            ThrowArgumentType(functionName);
        }

        return value.is_null() ? std::string{} : ToDisplayString(value);
    }

    long long RequireInteger(const std::string& functionName, const nlohmann::json& value)
    {
        if (value.is_number_float())
        {
            return static_cast<long long>(value.get<double>());
        }

        if (value.is_number())
        {
            return value.get<long long>();
        }

        if (value.is_string())
        {
            const auto& text = value.get_ref<const std::string&>();
            long long parsed = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
            {
                return parsed;
            }
        }

        ThrowArgumentType(functionName);
    }

    int CompareValues(const std::string& functionName, const nlohmann::json& left, const nlohmann::json& right)
    {
        if (left.is_number() && right.is_number())
        {
            const auto first = left.get<double>();
            const auto second = right.get<double>();
            return first < second ? -1 : (first > second ? 1 : 0);
        }

        if (left.is_string() && right.is_string())
        {
            const auto result = left.get_ref<const std::string&>().compare(right.get_ref<const std::string&>());
            return result < 0 ? -1 : (result > 0 ? 1 : 0);
        }

        ThrowArgumentType(functionName);
    }

    std::string ApplyPadding(std::string text, std::string_view flags, size_t width)
    {
        if (text.size() >= width)
        {
            return text;
        }

        const auto padding = width - text.size();
        if (flags.find('-') != std::string_view::npos)
        {
            text.append(padding, ' ');
            return text;
        }

        const char fill = flags.find('0') != std::string_view::npos ? '0' : ' ';
        return std::string(padding, fill) + text;
    }

    std::string RenderVerb(const std::string& functionName, char verb, const nlohmann::json& value, bool hasPrecision, int precision)
    {
        switch (verb)
        {
        case 'v':
        case 's':
            return ToDisplayString(value);

        case 'q':
            return nlohmann::json(RequireString(functionName, value)).dump();

        case 'd':
            return std::to_string(RequireInteger(functionName, value));

        case 'x':
            return std::format("{:x}", RequireInteger(functionName, value));

        case 'X':
            return std::format("{:X}", RequireInteger(functionName, value));

        case 't':
            return IsTruthy(value) ? "true" : "false";

        case 'f':
        {
            if (!value.is_number())
            {
                ThrowArgumentType(functionName);
            }

            return std::format("{:.{}f}", value.get<double>(), hasPrecision ? precision : 6);
        }

        default:
            ThrowArgumentType(functionName);
        }
    }

    // Implements the subset of Go's fmt verbs that command output formatting needs.
    std::string FormatPrintf(const std::string& functionName, const std::string& format, const std::vector<nlohmann::json>& arguments)
    {
        std::string result;
        size_t argumentIndex = 0;

        for (size_t index = 0; index < format.size(); ++index)
        {
            if (format[index] != '%')
            {
                result += format[index];
                continue;
            }

            ++index;
            if (index >= format.size())
            {
                result += "%!(NOVERB)";
                break;
            }

            std::string flags;
            while (index < format.size() && (format[index] == '-' || format[index] == '+' || format[index] == '0' ||
                                             format[index] == ' ' || format[index] == '#'))
            {
                flags += format[index++];
            }

            size_t width = 0;
            while (index < format.size() && IsDigit(format[index]))
            {
                width = (width * 10) + static_cast<size_t>(format[index++] - '0');
            }

            bool hasPrecision = false;
            int precision = 0;
            if (index < format.size() && format[index] == '.')
            {
                hasPrecision = true;
                ++index;
                while (index < format.size() && IsDigit(format[index]))
                {
                    precision = (precision * 10) + (format[index++] - '0');
                }
            }

            if (index >= format.size())
            {
                result += "%!(NOVERB)";
                break;
            }

            const char verb = format[index];
            if (verb == '%')
            {
                result += '%';
                continue;
            }

            if (argumentIndex >= arguments.size())
            {
                result += "%!";
                result += verb;
                result += "(MISSING)";
                continue;
            }

            result += ApplyPadding(RenderVerb(functionName, verb, arguments[argumentIndex++], hasPrecision, precision), flags, width);
        }

        return result;
    }

    nlohmann::json FunctionJson(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        return arguments[0].dump();
    }

    nlohmann::json FunctionSplit(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        const auto text = RequireString(functionName, arguments[0]);
        const auto separator = RequireString(functionName, arguments[1]);

        auto result = nlohmann::json::array();
        if (separator.empty())
        {
            for (const char character : text)
            {
                result.push_back(std::string(1, character));
            }

            return result;
        }

        size_t position = 0;
        while (true)
        {
            const auto next = text.find(separator, position);
            if (next == std::string::npos)
            {
                result.push_back(text.substr(position));
                break;
            }

            result.push_back(text.substr(position, next - position));
            position = next + separator.size();
        }

        return result;
    }

    nlohmann::json FunctionJoin(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        if (arguments[0].is_null())
        {
            return std::string{};
        }

        if (!arguments[0].is_array())
        {
            ThrowArgumentType(functionName);
        }

        const auto separator = RequireString(functionName, arguments[1]);

        std::string result;
        for (const auto& element : arguments[0])
        {
            if (!result.empty())
            {
                result += separator;
            }

            result += RequireString(functionName, element);
        }

        return result;
    }

    nlohmann::json FunctionTitle(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        auto text = RequireString(functionName, arguments[0]);
        bool startOfWord = true;
        for (auto& character : text)
        {
            if (startOfWord && character >= 'a' && character <= 'z')
            {
                character = static_cast<char>(character - ('a' - 'A'));
            }

            startOfWord = !IsAlpha(character);
        }

        return text;
    }

    nlohmann::json FunctionLower(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        auto text = RequireString(functionName, arguments[0]);
        for (auto& character : text)
        {
            if (character >= 'A' && character <= 'Z')
            {
                character = static_cast<char>(character + ('a' - 'A'));
            }
        }

        return text;
    }

    nlohmann::json FunctionUpper(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        auto text = RequireString(functionName, arguments[0]);
        for (auto& character : text)
        {
            if (character >= 'a' && character <= 'z')
            {
                character = static_cast<char>(character - ('a' - 'A'));
            }
        }

        return text;
    }

    nlohmann::json FunctionPad(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        const auto text = RequireString(functionName, arguments[0]);
        if (text.empty())
        {
            return text;
        }

        const auto prefix = std::max<long long>(0, RequireInteger(functionName, arguments[1]));
        const auto suffix = std::max<long long>(0, RequireInteger(functionName, arguments[2]));

        return std::string(static_cast<size_t>(prefix), ' ') + text + std::string(static_cast<size_t>(suffix), ' ');
    }

    nlohmann::json FunctionTruncate(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        const auto text = RequireString(functionName, arguments[0]);
        const auto length = RequireInteger(functionName, arguments[1]);
        if (length <= 0)
        {
            return std::string{};
        }

        return static_cast<size_t>(length) < text.size() ? text.substr(0, static_cast<size_t>(length)) : text;
    }

    nlohmann::json FunctionLen(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        const auto& value = arguments[0];
        if (value.is_null())
        {
            return 0;
        }

        if (value.is_string())
        {
            return value.get_ref<const std::string&>().size();
        }

        if (value.is_array() || value.is_object())
        {
            return value.size();
        }

        ThrowArgumentType(functionName);
    }

    nlohmann::json FunctionNot(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        return !IsTruthy(arguments[0]);
    }

    nlohmann::json FunctionAnd(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        for (auto& argument : arguments)
        {
            if (!IsTruthy(argument))
            {
                return argument;
            }
        }

        return arguments.back();
    }

    nlohmann::json FunctionOr(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        for (auto& argument : arguments)
        {
            if (IsTruthy(argument))
            {
                return argument;
            }
        }

        return arguments.back();
    }

    nlohmann::json FunctionEqual(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            if (arguments[0] == arguments[index])
            {
                return true;
            }
        }

        return false;
    }

    nlohmann::json FunctionNotEqual(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        return arguments[0] != arguments[1];
    }

    nlohmann::json FunctionLess(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        return CompareValues(functionName, arguments[0], arguments[1]) < 0;
    }

    nlohmann::json FunctionLessOrEqual(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        return CompareValues(functionName, arguments[0], arguments[1]) <= 0;
    }

    nlohmann::json FunctionGreater(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        return CompareValues(functionName, arguments[0], arguments[1]) > 0;
    }

    nlohmann::json FunctionGreaterOrEqual(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        return CompareValues(functionName, arguments[0], arguments[1]) >= 0;
    }

    nlohmann::json FunctionIndex(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        auto current = arguments[0];
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            if (current.is_array())
            {
                const auto position = RequireInteger(functionName, arguments[index]);
                if (position < 0 || static_cast<size_t>(position) >= current.size())
                {
                    ThrowArgumentType(functionName);
                }

                current = current[static_cast<size_t>(position)];
            }
            else if (current.is_object())
            {
                const auto key = RequireString(functionName, arguments[index]);
                const auto found = current.find(key);
                if (found == current.end())
                {
                    ThrowUnknownField(key);
                }

                current = *found;
            }
            else
            {
                ThrowArgumentType(functionName);
            }
        }

        return current;
    }

    nlohmann::json FunctionPrint(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        std::string result;
        for (const auto& argument : arguments)
        {
            result += ToDisplayString(argument);
        }

        return result;
    }

    nlohmann::json FunctionPrintln(const std::string&, std::vector<nlohmann::json>& arguments)
    {
        std::string result;
        for (const auto& argument : arguments)
        {
            if (!result.empty())
            {
                result += ' ';
            }

            result += ToDisplayString(argument);
        }

        result += '\n';
        return result;
    }

    nlohmann::json FunctionPrintf(const std::string& functionName, std::vector<nlohmann::json>& arguments)
    {
        const auto format = RequireString(functionName, arguments[0]);
        return FormatPrintf(functionName, format, {arguments.begin() + 1, arguments.end()});
    }

    struct FunctionDefinition
    {
        std::string_view name;
        size_t minimumArguments;
        size_t maximumArguments;
        nlohmann::json (*handler)(const std::string& functionName, std::vector<nlohmann::json>& arguments);
    };

    constexpr size_t c_variadic = std::numeric_limits<size_t>::max();

    // The functions available to a template: the set the docker CLI exposes, plus the Go
    // template builtins that are useful without variables or iteration.
    constexpr FunctionDefinition c_functions[] = {
        {"json", 1, 1, FunctionJson},
        {"split", 2, 2, FunctionSplit},
        {"join", 2, 2, FunctionJoin},
        {"title", 1, 1, FunctionTitle},
        {"lower", 1, 1, FunctionLower},
        {"upper", 1, 1, FunctionUpper},
        {"pad", 3, 3, FunctionPad},
        {"truncate", 2, 2, FunctionTruncate},
        {"len", 1, 1, FunctionLen},
        {"not", 1, 1, FunctionNot},
        {"and", 1, c_variadic, FunctionAnd},
        {"or", 1, c_variadic, FunctionOr},
        {"eq", 2, c_variadic, FunctionEqual},
        {"ne", 2, 2, FunctionNotEqual},
        {"lt", 2, 2, FunctionLess},
        {"le", 2, 2, FunctionLessOrEqual},
        {"gt", 2, 2, FunctionGreater},
        {"ge", 2, 2, FunctionGreaterOrEqual},
        {"index", 2, c_variadic, FunctionIndex},
        {"print", 1, c_variadic, FunctionPrint},
        {"println", 1, c_variadic, FunctionPrintln},
        {"printf", 1, c_variadic, FunctionPrintf},
    };

    const FunctionDefinition* LookupFunction(std::string_view name)
    {
        for (const auto& definition : c_functions)
        {
            if (definition.name == name)
            {
                return &definition;
            }
        }

        return nullptr;
    }

    struct Token
    {
        enum class Kind
        {
            End,
            Identifier,
            Field,
            Literal,
            LeftParen,
            RightParen,
            Pipe,
        };

        Kind kind = Kind::End;
        std::string text;
        std::vector<std::string> path;
        nlohmann::json value;
    };

    bool IsOperandStart(const Token& token) noexcept
    {
        switch (token.kind)
        {
        case Token::Kind::Identifier:
        case Token::Kind::Field:
        case Token::Kind::Literal:
        case Token::Kind::LeftParen:
            return true;

        default:
            return false;
        }
    }

    std::string ParseQuotedString(const std::string& body, size_t& position, const std::wstring& argName)
    {
        std::string result;
        ++position;

        while (position < body.size() && body[position] != '"')
        {
            if (body[position] == '\\' && position + 1 < body.size())
            {
                ++position;
                switch (body[position])
                {
                case 'n':
                    result += '\n';
                    break;

                case 't':
                    result += '\t';
                    break;

                case 'r':
                    result += '\r';
                    break;

                default:
                    result += body[position];
                    break;
                }

                ++position;
                continue;
            }

            result += body[position++];
        }

        if (position >= body.size())
        {
            throw ArgumentException(Localization::WSLCCLI_TemplateUnterminatedStringError(argName));
        }

        ++position;
        return result;
    }

    std::vector<Token> TokenizeAction(const std::string& body, const std::wstring& argName)
    {
        std::vector<Token> tokens;
        size_t position = 0;

        while (position < body.size())
        {
            const char character = body[position];
            if (IsSpace(character))
            {
                ++position;
                continue;
            }

            if (character == '(' || character == ')' || character == '|')
            {
                Token token;
                token.kind = character == '(' ? Token::Kind::LeftParen : (character == ')' ? Token::Kind::RightParen : Token::Kind::Pipe);
                token.text = std::string(1, character);
                tokens.push_back(std::move(token));
                ++position;
                continue;
            }

            if (character == '$')
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateVariablesUnsupportedError(argName));
            }

            if (character == '"')
            {
                Token token;
                token.kind = Token::Kind::Literal;
                token.value = ParseQuotedString(body, position, argName);
                token.text = token.value.get<std::string>();
                tokens.push_back(std::move(token));
                continue;
            }

            if (character == '`')
            {
                const auto close = body.find('`', position + 1);
                if (close == std::string::npos)
                {
                    throw ArgumentException(Localization::WSLCCLI_TemplateUnterminatedStringError(argName));
                }

                Token token;
                token.kind = Token::Kind::Literal;
                token.text = body.substr(position + 1, close - position - 1);
                token.value = token.text;
                position = close + 1;
                tokens.push_back(std::move(token));
                continue;
            }

            if (character == '.')
            {
                Token token;
                token.kind = Token::Kind::Field;
                token.text = ".";
                ++position;

                while (position < body.size() && IsIdentifierStart(body[position]))
                {
                    const auto start = position;
                    while (position < body.size() && IsIdentifierCharacter(body[position]))
                    {
                        ++position;
                    }

                    token.path.push_back(body.substr(start, position - start));
                    token.text += token.path.back();

                    if (position < body.size() && body[position] == '.')
                    {
                        ++position;
                        token.text += '.';
                        continue;
                    }

                    break;
                }

                tokens.push_back(std::move(token));
                continue;
            }

            if (IsDigit(character) || (character == '-' && position + 1 < body.size() && IsDigit(body[position + 1])))
            {
                const auto start = position;
                if (body[position] == '-')
                {
                    ++position;
                }

                bool isFloat = false;
                while (position < body.size() && (IsDigit(body[position]) || body[position] == '.'))
                {
                    isFloat = isFloat || body[position] == '.';
                    ++position;
                }

                const auto text = body.substr(start, position - start);
                Token token;
                token.kind = Token::Kind::Literal;
                token.text = text;

                if (isFloat)
                {
                    char* parseEnd = nullptr;
                    const auto parsed = std::strtod(text.c_str(), &parseEnd);
                    if (parseEnd != text.c_str() + text.size())
                    {
                        throw ArgumentException(Localization::WSLCCLI_TemplateUnexpectedTokenError(argName, MultiByteToWide(text)));
                    }

                    token.value = parsed;
                }
                else
                {
                    long long parsed = 0;
                    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
                    {
                        throw ArgumentException(Localization::WSLCCLI_TemplateUnexpectedTokenError(argName, MultiByteToWide(text)));
                    }

                    token.value = parsed;
                }

                tokens.push_back(std::move(token));
                continue;
            }

            if (IsIdentifierStart(character))
            {
                const auto start = position;
                while (position < body.size() && IsIdentifierCharacter(body[position]))
                {
                    ++position;
                }

                Token token;
                token.text = body.substr(start, position - start);

                if (token.text == "true" || token.text == "false")
                {
                    token.kind = Token::Kind::Literal;
                    token.value = token.text == "true";
                }
                else if (token.text == "nil")
                {
                    token.kind = Token::Kind::Literal;
                    token.value = nullptr;
                }
                else
                {
                    token.kind = Token::Kind::Identifier;
                }

                tokens.push_back(std::move(token));
                continue;
            }

            throw ArgumentException(Localization::WSLCCLI_TemplateUnexpectedTokenError(argName, MultiByteToWide(std::string(1, character))));
        }

        tokens.emplace_back();
        return tokens;
    }

    // Parses one action body into an expression tree. Grammar, following Go's:
    //   pipeline := command ('|' command)*
    //   command  := function operand* | operand
    //   operand  := field | literal | '(' pipeline ')'
    class ExpressionParser
    {
    public:
        ExpressionParser(std::vector<Token> tokens, const std::wstring& argName) : m_tokens(std::move(tokens)), m_argName(argName)
        {
        }

        ExpressionPtr ParseComplete()
        {
            auto expression = ParsePipeline();
            if (Peek().kind != Token::Kind::End)
            {
                ThrowUnexpected(Peek());
            }

            return expression;
        }

    private:
        const Token& Peek() const
        {
            return m_tokens[m_index];
        }

        Token Take()
        {
            // The token list always ends with an End token, which is never consumed.
            if (Peek().kind == Token::Kind::End)
            {
                return m_tokens.back();
            }

            return std::move(m_tokens[m_index++]);
        }

        [[noreturn]] void ThrowUnexpected(const Token& token) const
        {
            throw ArgumentException(Localization::WSLCCLI_TemplateUnexpectedTokenError(m_argName, MultiByteToWide(token.text)));
        }

        ExpressionPtr ParsePipeline()
        {
            auto expression = ParseCommand(nullptr);
            while (Peek().kind == Token::Kind::Pipe)
            {
                Take();
                expression = ParseCommand(std::move(expression));
            }

            return expression;
        }

        ExpressionPtr ParseCommand(ExpressionPtr chained)
        {
            if (!IsOperandStart(Peek()))
            {
                ThrowUnexpected(Peek());
            }

            if (Peek().kind != Token::Kind::Identifier)
            {
                auto operand = ParseOperand();
                if (chained || IsOperandStart(Peek()))
                {
                    ThrowUnexpected(chained ? m_tokens[m_index - 1] : Peek());
                }

                return operand;
            }

            auto token = Take();
            const auto* definition = LookupFunction(token.text);
            if (definition == nullptr)
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateUnknownFunctionError(m_argName, MultiByteToWide(token.text)));
            }

            auto call = std::make_shared<Expression>();
            call->kind = Expression::Kind::Call;
            call->function = token.text;

            while (IsOperandStart(Peek()))
            {
                call->arguments.push_back(ParseOperand());
            }

            // A pipeline feeds the previous stage in as the final argument.
            if (chained)
            {
                call->arguments.push_back(std::move(chained));
            }

            if (call->arguments.size() < definition->minimumArguments || call->arguments.size() > definition->maximumArguments)
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateArgumentCountError(m_argName, MultiByteToWide(token.text)));
            }

            return call;
        }

        ExpressionPtr ParseOperand()
        {
            auto token = Take();
            switch (token.kind)
            {
            case Token::Kind::Field:
            {
                auto expression = std::make_shared<Expression>();
                expression->kind = Expression::Kind::Field;
                expression->path = std::move(token.path);
                return expression;
            }

            case Token::Kind::Literal:
            {
                auto expression = std::make_shared<Expression>();
                expression->kind = Expression::Kind::Literal;
                expression->value = std::move(token.value);
                return expression;
            }

            case Token::Kind::LeftParen:
            {
                auto expression = ParsePipeline();
                if (Peek().kind != Token::Kind::RightParen)
                {
                    ThrowUnexpected(Peek());
                }

                Take();

                // A parenthesized term can be followed by a field path, which is walked from the
                // term's result.
                if (Peek().kind == Token::Kind::Field)
                {
                    auto chained = std::make_shared<Expression>();
                    chained->kind = Expression::Kind::Field;
                    chained->path = std::move(Take().path);
                    chained->source = std::move(expression);
                    return chained;
                }

                return expression;
            }

            case Token::Kind::Identifier:
            {
                if (LookupFunction(token.text) == nullptr)
                {
                    throw ArgumentException(Localization::WSLCCLI_TemplateUnknownFunctionError(m_argName, MultiByteToWide(token.text)));
                }

                throw ArgumentException(Localization::WSLCCLI_TemplateArgumentCountError(m_argName, MultiByteToWide(token.text)));
            }

            default:
                ThrowUnexpected(token);
            }
        }

        std::vector<Token> m_tokens;
        size_t m_index = 0;
        std::wstring m_argName;
    };

    // Splits template text into literal runs and actions, and builds the conditional tree.
    class TemplateParser
    {
    public:
        TemplateParser(std::string text, const std::wstring& argName) : m_text(std::move(text)), m_argName(argName)
        {
        }

        NodeList ParseDocument()
        {
            std::string terminator;
            std::string elseBody;
            auto nodes = ParseNodes(false, terminator, elseBody);
            return nodes;
        }

    private:
        ExpressionPtr ParseExpression(std::string_view body) const
        {
            const auto trimmed = Trim(body);
            if (trimmed.empty())
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateEmptyActionError(m_argName));
            }

            return ExpressionParser(TokenizeAction(std::string(trimmed), m_argName), m_argName).ParseComplete();
        }

        static void FlushText(NodeList& nodes, std::string& pending)
        {
            if (pending.empty())
            {
                return;
            }

            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Text;
            node->text = std::move(pending);
            nodes.push_back(std::move(node));
            pending.clear();
        }

        // Locates the "}}" that closes an action, skipping over any that appear inside string literals.
        size_t FindActionEnd(size_t start) const
        {
            for (size_t index = start; index < m_text.size(); ++index)
            {
                const char character = m_text[index];
                if (character == '"' || character == '`')
                {
                    const char quote = character;
                    ++index;
                    while (index < m_text.size() && m_text[index] != quote)
                    {
                        if (quote == '"' && m_text[index] == '\\')
                        {
                            ++index;
                        }

                        ++index;
                    }

                    if (index >= m_text.size())
                    {
                        throw ArgumentException(Localization::WSLCCLI_TemplateUnterminatedStringError(m_argName));
                    }

                    continue;
                }

                if (character == '}' && index + 1 < m_text.size() && m_text[index + 1] == '}')
                {
                    return index;
                }
            }

            throw ArgumentException(Localization::WSLCCLI_TemplateUnclosedActionError(m_argName));
        }

        std::shared_ptr<Node> ParseConditional(std::string_view condition)
        {
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Conditional;
            node->expression = ParseExpression(condition);

            std::string terminator;
            std::string elseBody;
            node->thenNodes = ParseNodes(true, terminator, elseBody);

            if (terminator == "else")
            {
                const auto trimmed = Trim(elseBody);
                if (trimmed.empty())
                {
                    std::string innerTerminator;
                    std::string innerElseBody;
                    node->elseNodes = ParseNodes(true, innerTerminator, innerElseBody);
                    if (innerTerminator != "end")
                    {
                        throw ArgumentException(Localization::WSLCCLI_TemplateMissingEndError(m_argName));
                    }
                }
                else if (FirstWord(trimmed) == "if")
                {
                    // "else if" nests another conditional that shares this action's closing end.
                    node->elseNodes.push_back(ParseConditional(trimmed.substr(2)));
                }
                else
                {
                    throw ArgumentException(Localization::WSLCCLI_TemplateUnexpectedTokenError(
                        m_argName, MultiByteToWide(std::string(FirstWord(trimmed)))));
                }
            }
            else if (terminator != "end")
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateMissingEndError(m_argName));
            }

            return node;
        }

        NodeList ParseNodes(bool nested, std::string& terminator, std::string& elseBody)
        {
            NodeList nodes;
            std::string pending;

            while (true)
            {
                const auto start = m_text.find(c_actionStart, m_pos);
                if (start == std::string::npos)
                {
                    pending.append(m_text, m_pos, std::string::npos);
                    m_pos = m_text.size();
                    break;
                }

                pending.append(m_text, m_pos, start - m_pos);
                auto cursor = start + c_actionStart.size();

                // "{{- " removes the whitespace immediately before the action.
                if (cursor + 1 < m_text.size() && m_text[cursor] == '-' && IsSpace(m_text[cursor + 1]))
                {
                    ++cursor;
                    while (!pending.empty() && IsSpace(pending.back()))
                    {
                        pending.pop_back();
                    }
                }

                const auto end = FindActionEnd(cursor);
                auto bodyEnd = end;

                // " -}}" removes the whitespace immediately after the action.
                bool trimRight = false;
                if (bodyEnd >= cursor + 2 && m_text[bodyEnd - 1] == '-' && IsSpace(m_text[bodyEnd - 2]))
                {
                    --bodyEnd;
                    trimRight = true;
                }

                const auto body = m_text.substr(cursor, bodyEnd - cursor);
                m_pos = end + c_actionEnd.size();

                if (trimRight)
                {
                    while (m_pos < m_text.size() && IsSpace(m_text[m_pos]))
                    {
                        ++m_pos;
                    }
                }

                const auto trimmed = Trim(body);
                if (trimmed.starts_with("/*"))
                {
                    continue;
                }

                const auto keyword = FirstWord(trimmed);

                if (keyword == "end" || keyword == "else")
                {
                    if (!nested)
                    {
                        throw ArgumentException(
                            Localization::WSLCCLI_TemplateUnexpectedKeywordError(m_argName, MultiByteToWide(std::string(keyword))));
                    }

                    FlushText(nodes, pending);
                    terminator = keyword;
                    elseBody = keyword == "else" ? std::string(trimmed.substr(keyword.size())) : std::string{};
                    return nodes;
                }

                if (std::find(std::begin(c_unsupportedKeywords), std::end(c_unsupportedKeywords), keyword) != std::end(c_unsupportedKeywords))
                {
                    throw ArgumentException(
                        Localization::WSLCCLI_TemplateUnsupportedKeywordError(m_argName, MultiByteToWide(std::string(keyword))));
                }

                FlushText(nodes, pending);

                if (keyword == "if")
                {
                    nodes.push_back(ParseConditional(trimmed.substr(keyword.size())));
                    continue;
                }

                auto node = std::make_shared<Node>();
                node->kind = Node::Kind::Action;
                node->expression = ParseExpression(trimmed);
                nodes.push_back(std::move(node));
            }

            FlushText(nodes, pending);

            if (nested)
            {
                throw ArgumentException(Localization::WSLCCLI_TemplateMissingEndError(m_argName));
            }

            return nodes;
        }

        std::string m_text;
        size_t m_pos = 0;
        std::wstring m_argName;
    };

    nlohmann::json Evaluate(const Expression& expression, const nlohmann::json& record)
    {
        switch (expression.kind)
        {
        case Expression::Kind::Literal:
            return expression.value;

        case Expression::Kind::Field:
        {
            nlohmann::json origin;
            if (expression.source)
            {
                origin = Evaluate(*expression.source, record);
            }

            const nlohmann::json* current = expression.source ? &origin : &record;
            for (const auto& segment : expression.path)
            {
                if (!current->is_object())
                {
                    ThrowUnknownField(segment);
                }

                const auto found = current->find(segment);
                if (found == current->end())
                {
                    ThrowUnknownField(segment);
                }

                current = &*found;
            }

            return *current;
        }

        case Expression::Kind::Call:
        {
            std::vector<nlohmann::json> arguments;
            arguments.reserve(expression.arguments.size());
            for (const auto& argument : expression.arguments)
            {
                arguments.push_back(Evaluate(*argument, record));
            }

            return LookupFunction(expression.function)->handler(expression.function, arguments);
        }
        }

        return nullptr;
    }

    void RenderNodes(const NodeList& nodes, const nlohmann::json& record, std::string& output)
    {
        for (const auto& node : nodes)
        {
            switch (node->kind)
            {
            case Node::Kind::Text:
                output += node->text;
                break;

            case Node::Kind::Action:
                output += ToDisplayString(Evaluate(*node->expression, record));
                break;

            case Node::Kind::Conditional:
                RenderNodes(IsTruthy(Evaluate(*node->expression, record)) ? node->thenNodes : node->elseNodes, record, output);
                break;
            }
        }
    }

} // namespace

OutputTemplate OutputTemplate::Parse(const std::wstring& text, const std::wstring& argName)
{
    return OutputTemplate(TemplateParser(WideToMultiByte(text.c_str()), argName).ParseDocument());
}

std::wstring OutputTemplate::Render(const nlohmann::json& record) const
{
    std::string output;
    RenderNodes(m_nodes, record, output);

    return MultiByteToWide(output);
}

} // namespace wsl::windows::wslc
