#pragma once

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aoe_temporal
{
struct Token
{
    size_t begin, end;
    std::string value;
};

inline std::vector<Token> Tokens(const std::string &source)
{
    std::vector<Token> result;
    for (size_t i = 0; i < source.size();)
    {
        const auto c = static_cast<unsigned char>(source[i]);
        if (std::isspace(c))
        {
            ++i;
            continue;
        }
        if (source[i] == '#')
        {
            const auto end = source.find('\n', i);
            i = end == std::string::npos ? source.size() : end + 1;
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/')
        {
            const auto end = source.find('\n', i);
            i = end == std::string::npos ? source.size() : end + 1;
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
            const auto end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
            continue;
        }
        const size_t begin = i++;
        if (std::isalnum(c) || c == '_')
            while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_'))
                ++i;
        else if (c == '"' || c == '\'')
        {
            while (i < source.size())
            {
                if (source[i] == '\\' && i + 1 < source.size())
                {
                    i += 2;
                    continue;
                }
                if (source[i++] == c)
                    break;
            }
        }
        result.push_back({begin, i, source.substr(begin, i - begin)});
    }
    return result;
}

inline size_t Match(const std::vector<Token> &tokens, size_t first, const char *open, const char *close)
{
    unsigned depth = 0;
    for (size_t i = first; i < tokens.size(); ++i)
    {
        if (tokens[i].value == open)
            ++depth;
        if (tokens[i].value == close && --depth == 0)
            return i;
    }
    return tokens.size();
}

struct Function
{
    size_t begin, end, name, parameters, parameters_end;
};

inline std::vector<Function> Functions(const std::vector<Token> &tokens)
{
    std::vector<Function> result;
    size_t begin = 0;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].value == ";")
        {
            begin = i + 1;
            continue;
        }
        if (tokens[i].value != "{")
            continue;
        const size_t end = Match(tokens, i, "{", "}");
        if (end == tokens.size())
            return {};
        for (size_t p = begin; p < i; ++p)
        {
            if (tokens[p].value != "(" || p == begin)
                continue;
            const size_t parameters_end = Match(tokens, p, "(", ")");
            if (parameters_end >= i)
                break;
            // Global structs, cbuffers and initializers are not functions.
            if (parameters_end + 1 != i && tokens[parameters_end + 1].value != ":")
                break;
            result.push_back({begin, end + 1, p - 1, p, parameters_end});
            break;
        }
        i = end;
        begin = end + 1;
    }
    return result;
}

inline bool Parameters(const std::string &source, const std::vector<Token> &tokens, const Function &function,
                       std::string &declaration, std::string &arguments)
{
    declaration = source.substr(tokens[function.parameters].end,
                                tokens[function.parameters_end].begin - tokens[function.parameters].end);
    if (function.parameters + 1 == function.parameters_end)
        return true;
    size_t begin = function.parameters + 1;
    for (size_t i = begin; i <= function.parameters_end; ++i)
    {
        if (i < function.parameters_end && tokens[i].value != ",")
            continue;
        if (i == begin)
            return false;
        size_t name = i - 1;
        for (size_t p = begin; p < i; ++p)
        {
            if (tokens[p].value == "out" || tokens[p].value == "inout")
                return false;
            if (tokens[p].value == ":" || tokens[p].value == "[")
            {
                name = p - 1;
                break;
            }
        }
        if (name <= begin)
            return false;
        if (!arguments.empty())
            arguments += ", ";
        arguments += tokens[name].value;
        begin = i + 1;
    }
    return !arguments.empty();
}

inline bool SemanticEqual(std::string a, std::string b)
{
    for (auto &c : a)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    for (auto &c : b)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return a == b;
}

inline std::string SemanticMember(const std::vector<Token> &tokens, const std::string &type,
                                  const std::string &semantic)
{
    for (size_t i = 0; i + 2 < tokens.size(); ++i)
    {
        if (tokens[i].value != "struct" || tokens[i + 1].value != type || tokens[i + 2].value != "{")
            continue;
        const size_t end = Match(tokens, i + 2, "{", "}");
        for (size_t p = i + 3; p + 1 < end; ++p)
            if (tokens[p].value == ":" && SemanticEqual(tokens[p + 1].value, semantic))
                return "." + tokens[p - 1].value;
    }
    return {};
}

// Alpha of the colour output, or "1" when it is not a float4.
inline std::string ColorAlpha(const std::vector<Token> &tokens, const std::string &type, const std::string &semantic)
{
    const auto colour = [](const std::string &name)
    {
        return SemanticEqual(name, "SV_Target0") || SemanticEqual(name, "SV_Target") ||
               SemanticEqual(name, "COLOR") || SemanticEqual(name, "COLOR0");
    };
    if (!semantic.empty())
        return type == "float4" && colour(semantic.substr(3)) ? "result.material.a" : "1";
    for (size_t i = 0; i + 2 < tokens.size(); ++i)
    {
        if (tokens[i].value != "struct" || tokens[i + 1].value != type || tokens[i + 2].value != "{")
            continue;
        const size_t end = Match(tokens, i + 2, "{", "}");
        for (size_t p = i + 5; p + 1 < end; ++p)
            if (tokens[p].value == ":" && colour(tokens[p + 1].value) && tokens[p - 2].value == "float4")
                return "result.material." + tokens[p - 1].value + ".a";
    }
    return "1";
}

struct Variable
{
    std::string name, declaration;
};

inline bool VertexSource(const std::string &source, const std::string &entry, const std::vector<Variable> &variables,
                         std::string &adapted, bool static_geometry = false, bool frame_animation = false)
{
    const auto tokens = Tokens(source);
    const auto functions = Functions(tokens);
    const Function *main = nullptr;
    std::set<std::string> names;
    for (const auto &function : functions)
    {
        names.insert(tokens[function.name].value);
        if (tokens[function.name].value == entry)
            main = &function;
    }
    if (!main || main->name == main->begin)
        return false;
    const std::string type = tokens[main->name - 1].value;
    std::string member = SemanticMember(tokens, type, "SV_Position");
    if (member.empty())
        member = SemanticMember(tokens, type, "POSITION");
    if (member.empty() && type != "float4")
        return false;
    std::string parameters, arguments;
    if (!Parameters(source, tokens, *main, parameters, arguments))
        return false;
    bool material_history = false;
    for (size_t i = 0; i + 2 < tokens.size(); ++i)
    {
        if (tokens[i].value != "struct" || tokens[i + 1].value != type || tokens[i + 2].value != "{")
            continue;
        const size_t end = Match(tokens, i + 2, "{", "}");
        for (size_t p = i + 3; p + 1 < end; ++p)
            if (tokens[p].value == "vel_prev" && tokens[p + 1].value == ":")
                material_history = true;
    }
    adapted = source + "\ncbuffer AOE_RasterSettings : register(b12) { float4 aoe_raster_jitter; };\n";
    std::string previous_position;
    if (material_history)
        previous_position = "result.material.vel_prev";
    else if (static_geometry)
    {
        adapted += "cbuffer AOE_CameraHistory : register(b13) { float4x4 aoe_camera_reprojection; };\n";
        previous_position = "mul(aoe_camera_reprojection, result.material" + member + ")";
    }
    else
    {
        std::map<std::string, std::string> uniforms;
        adapted += "cbuffer AOE_PreviousDraw : register(b13)\n{\n";
        if (frame_animation)
            adapted += "float4x4 aoe_camera_reprojection;\n";
        for (const auto &variable : variables)
        {
            if (frame_animation && variable.name != "wave" && variable.name != "wind" && variable.name != "dir2D")
                continue;
            uniforms.emplace(variable.name, "aoe_previous_" + variable.name);
            adapted += variable.declaration + ";\n";
        }
        adapted += "};\n";
        // Re-evaluate the actual material vertex functions with their preceding
        // draw's constants. This preserves installed skinning, wind and options.
        for (const auto &function : functions)
        {
            size_t offset = tokens[function.begin].begin;
            for (size_t i = function.begin; i < function.end; ++i)
            {
                const auto &token = tokens[i];
                adapted.append(source, offset, token.begin - offset);
                const bool member_access = i > function.begin && tokens[i - 1].value == ".";
                const bool call = i + 1 < function.end && tokens[i + 1].value == "(";
                auto uniform = uniforms.find(token.value);
                if (!member_access && call && names.count(token.value))
                    adapted += "aoe_previous_function_" + token.value;
                else if (!member_access && uniform != uniforms.end())
                    adapted += uniform->second;
                else
                    adapted += token.value;
                offset = token.end;
            }
            adapted += "\n";
        }
        previous_position = "aoe_previous_function_" + entry + "(" + arguments + ")" + member;
        if (frame_animation)
            previous_position = "mul(aoe_camera_reprojection, " + previous_position + ")";
    }
    std::string semantic;
    if (member.empty())
        semantic = " : SV_Position";
    adapted += "\nstruct AOE_VertexOutput { " + type + " material" + semantic +
               "; float4 aoe_current : AOE_CURRENT; float4 aoe_previous : AOE_PREVIOUS; };\n";
    adapted += "AOE_VertexOutput aoe_temporal_main(" + parameters + ")\n{\n" +
               "AOE_VertexOutput result;\nresult.material = " + entry + "(" + arguments + ");\n" +
               "result.aoe_current = result.material" + member + ";\nresult.aoe_previous = " + previous_position +
               ";\nresult.material" + member + ".xy += aoe_raster_jitter.xy * result.material" + member +
               ".w;\nreturn result;\n}\n";
    return true;
}

inline bool PixelSource(const std::string &source, const std::string &entry, std::string &adapted,
                        const std::string &padding = {})
{
    const auto tokens = Tokens(source);
    const auto functions = Functions(tokens);
    for (const auto &function : functions)
    {
        if (tokens[function.name].value != entry || function.name == function.begin)
            continue;
        const std::string type = tokens[function.name - 1].value;
        if (type == "void")
            return false;
        std::string parameters, arguments;
        if (!Parameters(source, tokens, function, parameters, arguments))
            return false;
        if (!parameters.empty())
            parameters += ", ";
        parameters += padding;
        std::string semantic;
        if (function.parameters_end + 2 < function.end && tokens[function.parameters_end + 1].value == ":")
            semantic = " : " + tokens[function.parameters_end + 2].value;
        const std::string legacy_member = SemanticMember(tokens, type, "SV_Target3");
        const bool direct_legacy = SemanticEqual(semantic, " : SV_Target3");
        const bool has_legacy = direct_legacy || !legacy_member.empty();
        adapted = source + "\ncbuffer AOE_MotionSettings : register(b12) { float4 aoe_motion_settings; };\n";
        adapted += "struct AOE_PixelOutput { " + type + " material" + semantic + "; float4 motion : SV_Target4; " +
                   (has_legacy ? "" : "float4 legacy_motion : SV_Target3;") + " };\n";
        adapted += "AOE_PixelOutput aoe_temporal_main(" + parameters +
                   "float4 aoe_current : AOE_CURRENT, float4 aoe_previous : AOE_PREVIOUS)\n{\n" +
                   "AOE_PixelOutput result;\nresult.material = " + entry + "(" + arguments + ");\n" +
                   // Settings .w: bit 0 HUD, +2 no legacy target, +4 colour-blended draw.
                   "float aoe_w = aoe_motion_settings.w;\nbool aoe_blended = aoe_w > 3.5;\n"
                   "aoe_w -= aoe_blended ? 4 : 0;\n"
                   "float aoe_alpha = aoe_blended ? saturate(" + ColorAlpha(tokens, type, semantic) + ") : 1;\n" +
                   "bool valid =aoe_motion_settings.z > 0.5 && all(isfinite(aoe_previous)) && "
                   "all(isfinite(aoe_current)) && aoe_previous.w > 0.00001 && aoe_current.w > 0.00001;\n" +
                   "float2 velocity = valid ? (aoe_current.xy / max(aoe_current.w, 0.00001) - aoe_previous.xy / "
                   "max(aoe_previous.w, 0.00001)) * float2(0.5, -0.5) : 0;\n" +
                   "valid = valid && all(abs(velocity) <= 1);\nvelocity = valid ? velocity : 0;\n" +
                   "result.motion = float4(velocity, valid ? 1 : 0, aoe_alpha);\n";
        // Keep the established auxiliary channels while supplying frame-to-frame
        // raster motion even when the installed material has no SSS MV writer.
        if (has_legacy)
            adapted += "if (aoe_w < 1.5) result.material" + legacy_member +
                       ".xy = valid ? velocity + aoe_motion_settings.xy : 0;\n";
        else
            adapted += "result.legacy_motion = float4(valid ? velocity + aoe_motion_settings.xy : float2(0,0), "
                       "aoe_w > 1.5 ? aoe_w - 2 : aoe_w, 0);\n";
        adapted += "return result;\n}\n";
        return true;
    }
    return false;
}
} // namespace aoe_temporal
