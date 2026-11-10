#pragma once
#include "Markdown.h"
#include "WinUi.h"
#include <winrt/Microsoft.UI.Xaml.Documents.h>

namespace da
{
struct MarkdownVisual
{
    std::wstring key;
    ux::Documents::Paragraph paragraph{nullptr};
};

std::vector<MarkdownVisual> RenderMarkdown(const MarkdownNode& document);
void ResizeMarkdown(const controls::RichTextBlock& body);
} // namespace da
