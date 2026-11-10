#include "Markdown.h"
#define MD4C_USE_UTF16
#include "ThirdParty/md4c/md4c.h"
#include <cwchar>

namespace da
{
namespace
{
std::wstring Entity(std::wstring text)
{
    if (text.size() > 3 && text[1] == L'#')
    {
        bool hex = text[2] == L'x' || text[2] == L'X';
        wchar_t* end{};
        auto scalar = wcstoul(text.c_str() + (hex ? 3 : 2), &end, hex ? 16 : 10);
        if (*end == L';' && scalar > 0 && scalar <= 0x10ffff &&
            !(scalar >= 0xd800 && scalar <= 0xdfff))
        {
            if (scalar <= 0xffff)
                return std::wstring(1, static_cast<wchar_t>(scalar));
            scalar -= 0x10000;
            return {static_cast<wchar_t>(0xd800 + (scalar >> 10)),
                    static_cast<wchar_t>(0xdc00 + (scalar & 1023))};
        }
        return L"\xfffd";
    }
    for (auto const& pair : {std::pair{L"&amp;", L"&"},
                             {L"&lt;", L"<"},
                             {L"&gt;", L">"},
                             {L"&quot;", L"\""},
                             {L"&apos;", L"'"},
                             {L"&nbsp;", L"\xa0"},
                             {L"&copy;", L"\xa9"},
                             {L"&mdash;", L"\x2014"},
                             {L"&ndash;", L"\x2013"}})
        if (text == pair.first)
            return pair.second;
    return text;
}

struct Parser
{
    MarkdownNode root;
    std::vector<MarkdownNode*> stack{&root};
    std::exception_ptr error;
    size_t count{};

    template <class Action> static int Guard(void* context, Action action)
    {
        auto& parser = *static_cast<Parser*>(context);
        try
        {
            action(parser);
            return 0;
        }
        catch (...)
        {
            parser.error = std::current_exception();
            return 1;
        }
    }

    MarkdownNode& Push(MarkdownKind kind)
    {
        if (stack.size() > 256 || ++count > 200000)
            throw Error(L"Markdown 内容过于复杂。");
        auto& children = stack.back()->children;
        children.emplace_back();
        children.back().kind = kind;
        stack.push_back(&children.back());
        return children.back();
    }
};
} // namespace

MarkdownNode ParseMarkdown(std::wstring_view text)
{
    Parser context;
    MD_PARSER parser{};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* data)
    {
        return Parser::Guard(data,
                             [&](Parser& p)
                             {
                                 using K = MarkdownKind;
                                 if (type == MD_BLOCK_DOC)
                                     return;
                                 K kind = type == MD_BLOCK_P   ? K::Paragraph
                                          : type == MD_BLOCK_H ? K::Heading
                                          : type == MD_BLOCK_UL || type == MD_BLOCK_OL ? K::List
                                          : type == MD_BLOCK_LI                        ? K::Item
                                          : type == MD_BLOCK_QUOTE                     ? K::Quote
                                          : type == MD_BLOCK_CODE                      ? K::Code
                                          : type == MD_BLOCK_HR                        ? K::Rule
                                          : type == MD_BLOCK_TABLE                     ? K::Table
                                          : type == MD_BLOCK_THEAD ? K::TableHead
                                          : type == MD_BLOCK_TBODY ? K::TableBody
                                          : type == MD_BLOCK_TR    ? K::Row
                                                                   : K::Cell;
                                 auto& node = p.Push(kind);
                                 if (type == MD_BLOCK_H)
                                     node.value = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
                                 if (type == MD_BLOCK_OL)
                                 {
                                     node.ordered = true;
                                     node.value = static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start;
                                 }
                                 if (type == MD_BLOCK_LI)
                                 {
                                     auto info = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                                     node.task = info->is_task != 0;
                                     node.checked = node.task && info->task_mark != L' ';
                                 }
                                 if (type == MD_BLOCK_CODE)
                                 {
                                     auto info = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
                                     if (info->lang.size)
                                         node.target.assign(info->lang.text, info->lang.size);
                                 }
                                 if (type == MD_BLOCK_TH || type == MD_BLOCK_TD)
                                 {
                                     node.header = type == MD_BLOCK_TH;
                                     node.value = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
                                 }
                             });
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* data)
    {
        return Parser::Guard(data,
                             [&](Parser& p)
                             {
                                 if (type == MD_BLOCK_LI)
                                 {
                                     // MD4C omits paragraph callbacks in tight list items.
                                     auto& item = *p.stack.back();
                                     std::vector<MarkdownNode> normalized;
                                     for (auto& child : item.children)
                                     {
                                         if (child.kind >= MarkdownKind::Emphasis)
                                         {
                                             if (normalized.empty() ||
                                                 normalized.back().kind != MarkdownKind::Paragraph)
                                             {
                                                 normalized.emplace_back();
                                                 normalized.back().kind = MarkdownKind::Paragraph;
                                             }
                                             normalized.back().children.push_back(std::move(child));
                                         }
                                         else
                                             normalized.push_back(std::move(child));
                                     }
                                     item.children = std::move(normalized);
                                 }
                                 if (type != MD_BLOCK_DOC)
                                     p.stack.pop_back();
                             });
    };
    parser.enter_span = [](MD_SPANTYPE type, void* detail, void* data)
    {
        return Parser::Guard(data,
                             [&](Parser& p)
                             {
                                 using K = MarkdownKind;
                                 auto kind = type == MD_SPAN_STRONG ? K::Strong
                                             : type == MD_SPAN_EM   ? K::Emphasis
                                             : type == MD_SPAN_DEL  ? K::Strike
                                             : type == MD_SPAN_CODE ? K::InlineCode
                                             : type == MD_SPAN_A    ? K::Link
                                                                    : K::Image;
                                 auto& node = p.Push(kind);
                                 if (type == MD_SPAN_A)
                                 {
                                     auto href = static_cast<MD_SPAN_A_DETAIL*>(detail)->href;
                                     node.target.assign(href.text, href.size);
                                 }
                                 if (type == MD_SPAN_IMG)
                                 {
                                     auto src = static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src;
                                     node.target.assign(src.text, src.size);
                                 }
                             });
    };
    parser.leave_span = [](MD_SPANTYPE, void*, void* data)
    {
        return Parser::Guard(data,
                             [](Parser& p)
                             {
                                 p.stack.pop_back();
                             });
    };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* data)
    {
        return Parser::Guard(data,
                             [&](Parser& p)
                             {
                                 auto& node = p.Push(type == MD_TEXT_BR ? MarkdownKind::Break
                                                                        : MarkdownKind::Text);
                                 node.text = type == MD_TEXT_NULLCHAR ? L"\xfffd"
                                             : type == MD_TEXT_SOFTBR ? L"\n"
                                             : type == MD_TEXT_ENTITY ? Entity({value, size})
                                                                      : std::wstring(value, size);
                                 p.stack.pop_back();
                             });
    };
    if (md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &context))
    {
        if (context.error)
            std::rethrow_exception(context.error);
        throw Error(L"无法解析 Markdown 回答。");
    }
    return std::move(context.root);
}

std::wstring MarkdownText(const MarkdownNode& node)
{
    auto result = node.text;
    if (node.kind == MarkdownKind::Break)
        return L"\n";
    for (auto const& child : node.children)
    {
        result += MarkdownText(child);
        if (child.kind == MarkdownKind::Cell)
            result += L"\t";
        else if (child.kind == MarkdownKind::Paragraph || child.kind == MarkdownKind::Heading ||
                 child.kind == MarkdownKind::Row || child.kind == MarkdownKind::Item ||
                 child.kind == MarkdownKind::Code)
            result += L"\n";
    }
    return result;
}

std::wstring MarkdownKey(const MarkdownNode& node)
{
    auto result = std::to_wstring(static_cast<int>(node.kind)) + L":" +
                  std::to_wstring(node.value) + (node.ordered ? L"o" : L"u") +
                  (node.header ? L"h" : L"b") + (node.task ? L"t" : L"n") +
                  (node.checked ? L"x" : L"_");
    for (auto const& text : {node.text, node.target})
        result += std::to_wstring(text.size()) + L":" + text;
    result += L"[";
    for (auto const& child : node.children)
        result += MarkdownKey(child);
    return result + L"]";
}
} // namespace da
