#include "pch_engine.h"
#include "CustomEffectBase.h"

namespace ShaderLab::Effects
{
    std::wstring BuildCustomEffectXml(std::wstring_view displayName, std::wstring_view description,
                                      std::wstring_view category, const std::vector<std::wstring>& inputNames)
    {
        std::wstring xml = std::format(
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='{}'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='{}'/>\r\n"
            L"  <Property name='Description' type='string' value='{}'/>\r\n"
            L"  <Inputs>\r\n",
            displayName, category, description);
        for (const auto& name : inputNames)
            xml += std::format(L"    <Input name='{}'/>\r\n", name);
        xml += L"  </Inputs>\r\n"
               L"</Effect>\r\n";
        return xml;
    }

    std::vector<std::wstring> NumberedInputNames(UINT32 count)
    {
        std::vector<std::wstring> names;
        names.reserve(count);
        for (UINT32 i = 0; i < count; ++i)
            names.push_back(std::format(L"I{}", i));
        return names;
    }
}
