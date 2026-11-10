#pragma once
#include "Core.h"

namespace da
{
enum class MarkdownKind
{
    Document,
    Paragraph,
    Heading,
    List,
    Item,
    Quote,
    Code,
    Rule,
    Table,
    TableHead,
    TableBody,
    Row,
    Cell,
    Emphasis,
    Strong,
    Strike,
    InlineCode,
    Link,
    Image,
    Text,
    Break
};

struct MarkdownNode
{
    MarkdownKind kind = MarkdownKind::Document;
    std::wstring text, target;
    unsigned value{};
    bool ordered{}, header{}, task{}, checked{};
    std::vector<MarkdownNode> children;
    bool operator==(const MarkdownNode&) const = default;
};

MarkdownNode ParseMarkdown(std::wstring_view text);
std::wstring MarkdownText(const MarkdownNode& node);
std::wstring MarkdownKey(const MarkdownNode& node);
} // namespace da
