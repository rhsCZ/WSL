/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputTemplate.h

Abstract:

    Header file for OutputTemplate, the template engine backing custom --format
    values. It implements the subset of Go's text/template syntax that command
    output formatting relies on: literal text, field substitution, function
    pipelines and if/else conditionals.

--*/

#pragma once

#include "JsonUtils.h"
#include <memory>
#include <string>
#include <vector>

namespace wsl::windows::wslc {

namespace templates {

    struct Node;

    using NodeList = std::vector<std::shared_ptr<const Node>>;

} // namespace templates

// A compiled --format template. Copyable so it can be cached alongside the rest of the
// parsed argument value, and rendered once per record.
class OutputTemplate
{
public:
    OutputTemplate() = default;

    // Compiles template text. Throws ArgumentException when the text is malformed or uses a
    // construct outside the supported subset.
    static OutputTemplate Parse(const std::wstring& text, const std::wstring& argName = {});

    // Renders a single record. Throws ExecutionException when the template references a field
    // the record does not have, or applies a function to an unsupported value.
    std::wstring Render(const nlohmann::json& record) const;

    bool IsEmpty() const noexcept
    {
        return m_nodes.empty();
    }

private:
    explicit OutputTemplate(templates::NodeList nodes) : m_nodes(std::move(nodes))
    {
    }

    templates::NodeList m_nodes;
};

} // namespace wsl::windows::wslc
