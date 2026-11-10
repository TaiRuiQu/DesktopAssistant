#include "MarkdownView.h"
#include <winrt/Windows.UI.Text.h>

namespace da
{
namespace
{
using namespace ux::Documents;
using K = MarkdownKind;
using Inlines = winrt::Windows::Foundation::Collections::IVector<Inline>;

controls::Border ThemedBorder(const wchar_t* background = L"CardBackgroundFillColorDefaultBrush")
{
    return ux::Markup::XamlReader::Load(
               L"<Border xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
               L"BorderBrush=\"{ThemeResource CardStrokeColorDefaultBrush}\" "
               L"Background=\"{ThemeResource " +
               std::wstring(background) + L"}\"/>")
        .as<controls::Border>();
}

void AppendText(Inlines const& inlines, std::wstring_view text)
{
    Run run;
    run.Text(winrt::hstring(text));
    inlines.Append(run);
}

void InlineNode(Inlines const& inlines, MarkdownNode const& node)
{
    if (node.kind == K::Text)
        AppendText(inlines, node.text);
    else if (node.kind == K::Break)
        inlines.Append(LineBreak());
    else
    {
        Span span;
        if (node.kind == K::Strong)
        {
            span.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        }
        else if (node.kind == K::Emphasis)
            span.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
        else if (node.kind == K::Strike)
            span.TextDecorations(winrt::Windows::UI::Text::TextDecorations::Strikethrough);
        else if (node.kind == K::InlineCode)
        {
            span.FontFamily(ux::Media::FontFamily(L"Consolas, Microsoft YaHei UI"));
            span.FontSize(14);
        }
        else if (node.kind == K::Link)
        {
            Hyperlink link;
            if (node.target.starts_with(L"https://") || node.target.starts_with(L"http://") ||
                node.target.starts_with(L"mailto:"))
                try
                {
                    link.NavigateUri(foundation::Uri(node.target));
                }
                catch (...)
                {
                }

            for (auto const& child : node.children)
                InlineNode(link.Inlines(), child);
            inlines.Append(link);
            return;
        }
        for (auto const& child : node.children)
            InlineNode(span.Inlines(), child);
        inlines.Append(span);
    }
}

controls::RichTextBlock TextBody()
{
    controls::RichTextBlock body;
    body.FontFamily(ux::Media::FontFamily(L"Microsoft YaHei UI"));
    body.FontSize(15);
    body.LineHeight(27);

    body.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    body.TextWrapping(ux::TextWrapping::Wrap);
    body.IsTextSelectionEnabled(true);
    return body;
}

ux::FrameworkElement Table(MarkdownNode const& node)
{
    std::vector<MarkdownNode const*> rows;
    for (auto const& section : node.children)
        for (auto const& row : section.children)
            rows.push_back(&row);
    controls::Grid grid;
    size_t columns = 0;
    for (auto row : rows)
        columns = std::max(columns, row->children.size());
    grid.MinWidth(static_cast<double>(std::max<size_t>(420, columns * 140)));
    for (size_t c = 0; c < columns; ++c)
    {
        controls::ColumnDefinition column;
        column.Width({1, ux::GridUnitType::Star});
        grid.ColumnDefinitions().Append(column);
    }
    for (size_t r = 0; r < rows.size(); ++r)
    {
        controls::RowDefinition row;
        row.Height({1, ux::GridUnitType::Auto});
        grid.RowDefinitions().Append(row);
        for (size_t c = 0; c < rows[r]->children.size(); ++c)
        {
            auto const& cell = rows[r]->children[c];
            auto body = TextBody();
            Paragraph paragraph;
            for (auto const& child : cell.children)
                InlineNode(paragraph.Inlines(), child);
            body.Blocks().Append(paragraph);
            if (cell.header)
            {
                body.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            body.TextAlignment(cell.value == 3   ? ux::TextAlignment::Right
                               : cell.value == 2 ? ux::TextAlignment::Center
                                                 : ux::TextAlignment::Left);
            auto border = ThemedBorder(cell.header ? L"SubtleFillColorSecondaryBrush"
                                                   : L"CardBackgroundFillColorDefaultBrush");
            border.Padding({12, 10, 12, 10});
            border.BorderThickness(
                {0, 0, c + 1 < columns ? 1. : 0., r + 1 < rows.size() ? 1. : 0.});
            border.Child(body);
            controls::Grid::SetRow(border, static_cast<int>(r));
            controls::Grid::SetColumn(border, static_cast<int>(c));
            grid.Children().Append(border);
        }
    }
    controls::ScrollViewer scroll;
    scroll.HorizontalScrollBarVisibility(controls::ScrollBarVisibility::Auto);
    scroll.VerticalScrollBarVisibility(controls::ScrollBarVisibility::Disabled);
    scroll.HorizontalScrollMode(controls::ScrollMode::Enabled);
    scroll.HorizontalContentAlignment(ux::HorizontalAlignment::Stretch);
    scroll.Content(grid);
    auto frame = ThemedBorder();
    frame.BorderThickness({1, 1, 1, 1});
    frame.CornerRadius({8, 8, 8, 8});
    frame.Child(scroll);
    return frame;
}

ux::FrameworkElement Code(MarkdownNode const& node)
{
    auto body = TextBody();
    body.FontFamily(ux::Media::FontFamily(L"Consolas, Microsoft YaHei UI"));
    body.FontSize(14);
    body.TextWrapping(ux::TextWrapping::NoWrap);
    Paragraph paragraph;
    auto text = MarkdownText(node);
    if (!text.empty() && text.back() == L'\n')
        text.pop_back();
    AppendText(paragraph.Inlines(), text);
    body.Blocks().Append(paragraph);
    controls::StackPanel content;
    content.Spacing(8);
    if (!node.target.empty())
    {
        controls::TextBlock language;
        language.Text(node.target);
        language.FontFamily(ux::Media::FontFamily(L"Microsoft YaHei UI"));
        language.FontSize(11);

        content.Children().Append(language);
    }
    controls::ScrollViewer scroll;
    scroll.HorizontalScrollBarVisibility(controls::ScrollBarVisibility::Auto);
    scroll.VerticalScrollBarVisibility(controls::ScrollBarVisibility::Disabled);
    scroll.Content(body);
    content.Children().Append(scroll);
    auto frame = ThemedBorder();
    frame.CornerRadius({10, 10, 10, 10});
    frame.Padding({14, 12, 14, 12});
    frame.Child(content);
    return frame;
}

void Render(std::vector<MarkdownVisual>& output,
            MarkdownNode const& node,
            unsigned depth = 0,
            std::wstring marker = {},
            bool quote = false)
{
    if (node.kind == K::Document || node.kind == K::Quote)
    {
        for (auto const& child : node.children)
            Render(output, child, depth, {}, quote || node.kind == K::Quote);
        return;
    }
    if (node.kind == K::List)
    {
        auto number = node.value;
        for (auto const& item : node.children)
        {
            auto label = item.task      ? (item.checked ? L"☑" : L"☐")
                         : node.ordered ? std::to_wstring(number++) + L"."
                                        : L"•";
            bool first = true;
            for (auto const& child : item.children)
            {
                Render(output, child, depth + 1, first ? label : L"", quote);
                first = false;
            }
        }
        return;
    }
    Paragraph paragraph;
    paragraph.FontFamily(ux::Media::FontFamily(L"Microsoft YaHei UI"));
    paragraph.FontSize(16);
    paragraph.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    paragraph.FontStyle(winrt::Windows::UI::Text::FontStyle::Normal);

    paragraph.Margin({depth * 28. + (quote ? 16. : 0.), 0, 0, depth ? 12. : 20.});
    paragraph.LineHeight(29);
    if (!marker.empty())
    {
        Run bullet;
        bullet.Text(marker + L"  ");

        paragraph.Inlines().Append(bullet);
    }
    if (quote)

        if (node.kind == K::Heading)
        {
            paragraph.FontSize(node.value == 1 ? 23 : node.value == 2 ? 20 : 18);
            paragraph.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        }
    if (node.kind == K::Table || node.kind == K::Code || node.kind == K::Rule)
    {
        InlineUIContainer container;
        if (node.kind == K::Table)
            container.Child(Table(node));
        else if (node.kind == K::Code)
            container.Child(Code(node));
        else
        {
            auto rule = ThemedBorder(L"DividerStrokeColorDefaultBrush");
            rule.Height(1);
            container.Child(rule);
        }
        paragraph.Inlines().Append(container);
    }
    else
        for (auto const& child : node.children)
            InlineNode(paragraph.Inlines(), child);
    output.push_back(
        {MarkdownKey(node) + std::to_wstring(depth) + marker + (quote ? L"quote" : L""),
         paragraph});
}
} // namespace

std::vector<MarkdownVisual> RenderMarkdown(MarkdownNode const& document)
{
    std::vector<MarkdownVisual> result;
    Render(result, document);
    return result;
}

void ResizeMarkdown(controls::RichTextBlock const& body)
{
    auto width = std::isfinite(body.Width()) ? body.Width() : body.ActualWidth();
    if (width <= 0)
        return;
    for (auto block : body.Blocks())
        if (auto paragraph = block.try_as<Paragraph>())
            for (auto item : paragraph.Inlines())
                if (auto container = item.try_as<InlineUIContainer>())
                    container.Child().as<ux::FrameworkElement>().Width(
                        std::max(32., width - paragraph.Margin().Left - 4));
}
} // namespace da
