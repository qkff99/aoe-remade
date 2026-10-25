#include "stdafx.h"
#include "DetailInstancing.h"
#include <regex>

namespace detail_instancing
{
bool Adapt(const char* source, size_t size, std::string& out)
{
    static const std::regex array_declaration(R"(^(\s*)(?:uniform\s+)?float4\s+array\s*\[[^\]]*\]\s*;(.*)$)");
    static const std::regex exdata_declaration(R"(^(\s*)(?:uniform\s+)?float4\s+exdata\s*\[[^\]]*\]\s*;(.*)$)");
    static const std::regex technique(R"(^\s*FXVS\b.*$)");
    static const std::regex entry(R"(^(\s*)([A-Za-z_]\w*)(\s+)main(\s*)\(\s*v_detail\s+([A-Za-z_]\w*)\s*\)(.*)$)");

    std::string result, type;
    result.reserve(size + 2048);
    unsigned arrays = 0, exdata = 0, entries = 0;
    size_t insert_at = std::string::npos;
    const char* const end = source + size;
    for (const char* line = source; line < end;)
    {
        const char* next = static_cast<const char*>(memchr(line, '\n', end - line));
        const char* stop = next ? next : end;
        std::string text(line, stop - line);
        std::string terminator;
        if (!text.empty() && text.back() == '\r')
        {
            text.pop_back();
            terminator = "\r";
        }
        if (next)
            terminator += "\n";
        std::smatch match;
        if (std::regex_match(text, match, array_declaration))
        {
            ++arrays;
            text = match[1].str() + "static float4 array[4];" + match[2].str();
        }
        else if (std::regex_match(text, match, exdata_declaration))
        {
            ++exdata;
            text = match[1].str() + "static float4 exdata[1];" + match[2].str();
        }
        else if (std::regex_match(text, match, entry) && match[2] != "return" && match[2] != "else")
        {
            ++entries;
            type = match[2];
            text = match[1].str() + type + match[3].str() + "aoe_detail_main" + match[4].str() + "(v_detail " +
                match[5].str() + ")" + match[6].str();
        }
        // The stock `FXVS;` macro expands to a technique that references main(), so the wrapper has to precede it.
        else if (entries && insert_at == std::string::npos && std::regex_match(text, technique))
            insert_at = result.size();
        result += text;
        result += terminator;
        line = next ? next + 1 : end;
    }
    if (arrays != 1 || entries != 1 || exdata > 1)
        return false;

    std::string wrapper =
        "\n// AOE detail instancing: the original entry is called with its constant arrays filled from stream 1.\n"
        "uniform float4 aoe_detail_fade; // x: squared camera distance above which blades shrink\n";
    wrapper += type + " main(v_detail v,\n"
        "    float4 aoe_m0 : TEXCOORD1, float4 aoe_m1 : TEXCOORD2, float4 aoe_m2 : TEXCOORD3,\n"
        "    float4 aoe_na : TEXCOORD4, float4 aoe_sh : TEXCOORD5)\n{\n"
        "    float aoe_scale = 1.0f;\n"
        "    if (aoe_sh.z > aoe_detail_fade.x)\n"
        "        aoe_scale = saturate(1.0f - abs(aoe_sh.z - aoe_detail_fade.x) * 0.005f);\n"
        "    if (!(aoe_na.w > 0.0f))\n"
        "        aoe_scale = 0.0f;\n"
        "    array[0] = float4(aoe_m0.xyz * aoe_scale, aoe_m0.w);\n"
        "    array[1] = float4(aoe_m1.xyz * aoe_scale, aoe_m1.w);\n"
        "    array[2] = float4(aoe_m2.xyz * aoe_scale, aoe_m2.w);\n"
        "    array[3] = float4(aoe_sh.x, aoe_sh.x, aoe_sh.x, aoe_sh.y);\n";
    if (exdata)
        wrapper += "    exdata[0] = aoe_na;\n";
    wrapper += "    v.misc.w = 0;\n"
        "    return aoe_detail_main(v);\n}\n";
    if (insert_at == std::string::npos)
        result += wrapper;
    else
        result.insert(insert_at, wrapper);
    out = std::move(result);
    return true;
}
}
