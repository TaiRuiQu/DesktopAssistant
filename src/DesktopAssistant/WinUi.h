#pragma once
#include "Core.h"
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.System.h>

namespace da
{
namespace ux = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace windowing = winrt::Microsoft::UI::Windowing;
namespace foundation = winrt::Windows::Foundation;

// C++/WinRT Application host and metadata for WinUI's built-in XAML controls.
struct XamlApplication : ux::ApplicationT<XamlApplication, ux::Markup::IXamlMetadataProvider>
{
    explicit XamlApplication(std::function<void()> launch) : launch_(std::move(launch)) {}

    void OnLaunched(ux::LaunchActivatedEventArgs const&)
    {
        launch_();
    }

    ux::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
        return Metadata().GetXamlType(type);
    }

    ux::Markup::IXamlType GetXamlType(winrt::hstring const& name)
    {
        return Metadata().GetXamlType(name);
    }

    winrt::com_array<ux::Markup::XmlnsDefinition> GetXmlnsDefinitions()
    {
        return Metadata().GetXmlnsDefinitions();
    }

private:
    std::function<void()> launch_;
    ux::Markup::IXamlMetadataProvider metadata_{nullptr};

    ux::Markup::IXamlMetadataProvider Metadata()
    {
        if (!metadata_)
            metadata_ = ux::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return metadata_;
    }
};
} // namespace da
