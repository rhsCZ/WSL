/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIOutputTemplateUnitTests.cpp

Abstract:

    This file contains unit tests for the --format template engine.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "ArgumentValidation.h"
#include "Exceptions.h"
#include "JsonUtils.h"
#include "OutputTemplate.h"
#include "SpecParsing.h"

using namespace wsl::windows::wslc;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIOutputTemplateUnitTests {

static nlohmann::json SampleRecord()
{
    return nlohmann::json{
        {"ID", "abc123"},
        {"Names", "web,api"},
        {"Image", "ubuntu:24.04"},
        {"State", "running"},
        {"Ports", ""},
        {"Size", 0},
        {"Running", true},
        {"Platform", {{"os", "linux"}}},
        {"Sessions", nlohmann::json::array({{{"Name", "first"}}, {{"Name", "second"}}})},
    };
}

// N.B. Template text and expected output are passed as parameters rather than written inside a
// VERIFY_* macro. The macros stringize their arguments, and the quotes and backslashes that
// templates contain do not survive that intact.
static void VerifyRender(const wchar_t* text, const wchar_t* expected)
{
    const auto actual = OutputTemplate::Parse(text, L"--format").Render(SampleRecord());
    VERIFY_ARE_EQUAL(std::wstring{expected}, actual);
}

static void VerifyFormatArgumentRender(const wchar_t* text, const wchar_t* expected)
{
    const auto actual = validation::GetOutputFormatFromString(text, L"--format").Template.Render(SampleRecord());
    VERIFY_ARE_EQUAL(std::wstring{expected}, actual);
}

static void VerifyParseThrows(const wchar_t* text)
{
    VERIFY_THROWS(OutputTemplate::Parse(text, L"--format"), ArgumentException);
}

static void VerifyRenderThrows(const wchar_t* text)
{
    const auto compiled = OutputTemplate::Parse(text, L"--format");
    const auto record = SampleRecord();
    VERIFY_THROWS(compiled.Render(record), ExecutionException);
}

class WSLCCLIOutputTemplateUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIOutputTemplateUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Test: Literal text passes through untouched and field actions are replaced by the matching
    // record value. This is the shape that the overwhelming majority of format strings use.
    TEST_METHOD(OutputTemplate_FieldSubstitution)
    {
        VerifyRender(L"{{.ID}}", L"abc123");
        VerifyRender(L"id={{.ID}} image={{.Image}}", L"id=abc123 image=ubuntu:24.04");
        VerifyRender(L"no actions here", L"no actions here");
        VerifyRender(L"{{.Platform.os}}", L"linux");
    }

    // Test: Non-string values render the way Go prints them rather than as JSON scalars.
    TEST_METHOD(OutputTemplate_ScalarRendering)
    {
        VerifyRender(L"{{.Size}}", L"0");
        VerifyRender(L"{{.Running}}", L"true");
        VerifyRender(L"{{.Ports}}", L"");
    }

    // Test: A bare dot resolves to the whole record, which is what lets the json function
    // reproduce the document the json format emits.
    TEST_METHOD(OutputTemplate_WholeRecord)
    {
        const auto rendered = OutputTemplate::Parse(L"{{json .}}", L"--format").Render(SampleRecord());
        VERIFY_IS_TRUE(rendered.starts_with(L"{"));
        VERIFY_IS_TRUE(rendered.find(L"abc123") != std::wstring::npos);

        VerifyRender(L"{{json .State}}", L"\"running\"");
    }

    // Test: The text helpers the docker CLI exposes are available and behave the same way.
    TEST_METHOD(OutputTemplate_TextFunctions)
    {
        VerifyRender(L"{{upper .State}}", L"RUNNING");
        VerifyRender(L"{{lower .Image}}", L"ubuntu:24.04");
        VerifyRender(L"{{title .State}}", L"Running");
        VerifyRender(L"{{truncate .ID 3}}", L"abc");
        VerifyRender(L"{{pad .State 1 2}}", L" running  ");
        VerifyRender(L"{{join (split .Names \",\") \" \"}}", L"web api");
        VerifyRender(L"{{len .ID}}", L"6");
    }

    // Test: A pipeline feeds each stage's result in as the final argument of the next stage.
    TEST_METHOD(OutputTemplate_Pipelines)
    {
        VerifyRender(L"{{.State | upper}}", L"RUNNING");
        VerifyRender(L"{{.State | upper | lower}}", L"running");
        VerifyRender(L"{{.State | printf \"%s-ok\"}}", L"running-ok");
        VerifyRender(L"{{join (split .Names \",\") \" \" | upper}}", L"WEB API");
    }

    // Test: printf supports the verbs that formatting output realistically needs.
    TEST_METHOD(OutputTemplate_Printf)
    {
        VerifyRender(L"{{printf \"%s/%s\" .ID .State}}", L"abc123/running");
        VerifyRender(L"{{printf \"%10s\" .ID}}", L"    abc123");
        VerifyRender(L"{{printf \"%-10s\" .ID}}", L"abc123    ");
        VerifyRender(L"{{printf \"%d%%\" 50}}", L"50%");
        VerifyRender(L"{{printf \"%q\" .State}}", L"\"running\"");
    }

    // Test: Conditionals select a branch using Go's rule that the zero value is false.
    TEST_METHOD(OutputTemplate_Conditionals)
    {
        VerifyRender(L"{{if .Running}}up{{else}}down{{end}}", L"up");
        VerifyRender(L"{{if .Ports}}{{.Ports}}{{else}}none{{end}}", L"none");
        VerifyRender(L"{{if .Size}}sized{{else}}empty{{end}}", L"empty");
        VerifyRender(L"{{if .ID}}{{.ID}}{{end}}", L"abc123");
        VerifyRender(L"{{if .Ports}}{{.Ports}}{{end}}", L"");
    }

    // Test: An else branch can continue the chain with another condition.
    TEST_METHOD(OutputTemplate_ElseIfChain)
    {
        VerifyRender(L"{{if .Ports}}ports{{else if .Running}}running{{else}}stopped{{end}}", L"running");
        VerifyRender(L"{{if .Ports}}ports{{else if .Size}}sized{{else}}stopped{{end}}", L"stopped");
    }

    // Test: The comparison and boolean helpers work against record values and literals.
    TEST_METHOD(OutputTemplate_ComparisonFunctions)
    {
        VerifyRender(L"{{if eq .State \"running\"}}yes{{else}}no{{end}}", L"yes");
        VerifyRender(L"{{if ne .State \"running\"}}yes{{else}}no{{end}}", L"no");
        VerifyRender(L"{{if not .Ports}}yes{{else}}no{{end}}", L"yes");
        VerifyRender(L"{{if and .ID .Running}}yes{{else}}no{{end}}", L"yes");
        VerifyRender(L"{{if or .Ports .ID}}yes{{else}}no{{end}}", L"yes");
        VerifyRender(L"{{if lt .Size 1}}yes{{else}}no{{end}}", L"yes");
        VerifyRender(L"{{if ge .Size 0}}yes{{else}}no{{end}}", L"yes");
    }

    // Test: index walks arrays and objects, which is how a value is reached when its key is
    // computed rather than written literally.
    TEST_METHOD(OutputTemplate_Index)
    {
        VerifyRender(L"{{index .Platform \"os\"}}", L"linux");
        VerifyRender(L"{{index (split .Names \",\") 1}}", L"api");
    }

    // Test: a parenthesized term can be followed by a field path, which is walked from that
    // term's result rather than from the record.
    TEST_METHOD(OutputTemplate_ChainedFieldOnParenthesizedTerm)
    {
        VerifyRender(L"{{(index .Sessions 0).Name}}", L"first");
        VerifyRender(L"{{(index .Sessions 1).Name}}", L"second");
        VerifyRenderThrows(L"{{(index .Sessions 0).Missing}}");
    }

    // Test: The trim markers remove the whitespace on the side they point at.
    TEST_METHOD(OutputTemplate_WhitespaceTrimming)
    {
        VerifyRender(L"  {{- .ID}}", L"abc123");
        VerifyRender(L"{{.ID -}}  ", L"abc123");
        VerifyRender(L"a:  {{- .ID -}}   ", L"a:abc123");
    }

    // Test: Literals of each supported kind parse and evaluate correctly.
    TEST_METHOD(OutputTemplate_Literals)
    {
        VerifyRender(L"{{printf \"%d\" 42}}", L"42");
        VerifyRender(L"{{printf \"%d\" -7}}", L"-7");
        VerifyRender(L"{{printf \"%.2f\" 1.5}}", L"1.50");
        VerifyRender(L"{{if true}}yes{{end}}", L"yes");
        VerifyRender(L"{{if false}}yes{{end}}", L"");
        VerifyRender(L"{{print `raw text`}}", L"raw text");
    }

    // Test: Referencing something the record does not have fails loudly instead of quietly
    // emitting an empty column.
    TEST_METHOD(OutputTemplate_UnknownFieldThrows)
    {
        VerifyRenderThrows(L"{{.NotAField}}");
        VerifyRenderThrows(L"{{.Platform.missing}}");
        VerifyRenderThrows(L"{{.State.nested}}");
    }

    // Test: Malformed template text is rejected at parse time, so the failure is reported before
    // any output is written.
    TEST_METHOD(OutputTemplate_SyntaxErrorsThrow)
    {
        VerifyParseThrows(L"{{.ID");
        VerifyParseThrows(L"{{}}");
        VerifyParseThrows(L"{{upper \"unterminated}}");
        VerifyParseThrows(L"{{nosuchfunction .ID}}");
        VerifyParseThrows(L"{{upper}}");
        VerifyParseThrows(L"{{upper .ID .State}}");
        VerifyParseThrows(L"{{if .ID}}yes");
        VerifyParseThrows(L"{{end}}");
        VerifyParseThrows(L"{{else}}");
        VerifyParseThrows(L"{{.ID .State}}");
    }

    // Test: Constructs outside the supported subset are rejected by name rather than failing as
    // a generic parse error.
    TEST_METHOD(OutputTemplate_UnsupportedConstructsThrow)
    {
        VerifyParseThrows(L"{{range .Names}}x{{end}}");
        VerifyParseThrows(L"{{with .ID}}x{{end}}");
        VerifyParseThrows(L"{{define \"x\"}}y{{end}}");
        VerifyParseThrows(L"{{$x := .ID}}");
    }

    // Test: The format argument keeps json and table as keywords and compiles everything else as
    // a template.
    TEST_METHOD(OutputTemplate_FormatArgumentClassification)
    {
        using namespace wsl::windows::wslc::models;

        VERIFY_ARE_EQUAL(FormatType::Json, validation::GetOutputFormatFromString(L"json").Type);
        VERIFY_ARE_EQUAL(FormatType::Table, validation::GetOutputFormatFromString(L"table").Type);
        VERIFY_ARE_EQUAL(FormatType::Table, validation::GetOutputFormatFromString(L"").Type);

        // Casing is significant, so an uppercase keyword is a literal template.
        VERIFY_ARE_EQUAL(FormatType::Template, validation::GetOutputFormatFromString(L"JSON").Type);
        VERIFY_ARE_EQUAL(FormatType::Template, validation::GetOutputFormatFromString(L"xml").Type);
    }

    // Test: Shells on Windows do not expand escape sequences inside quoted arguments, so the
    // format argument turns the two character forms into the whitespace they name.
    TEST_METHOD(OutputTemplate_FormatArgumentUnescapesWhitespace)
    {
        VerifyFormatArgumentRender(L"{{.ID}}\\t{{.State}}", L"abc123\trunning");
        VerifyFormatArgumentRender(L"{{.ID}}\\n{{.State}}", L"abc123\nrunning");
        VerifyFormatArgumentRender(L"{{.ID}}", L"abc123");
    }
};

} // namespace WSLCCLIOutputTemplateUnitTests
