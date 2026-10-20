#include "flirt/sampling_config.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

#include "mdpp-sampling/mdpp_decode.h"

namespace flirt
{

namespace
{

// A variable or a line part: its text, or why it cannot be evaluated.
struct Value
{
    QString text;
    std::string unknown;    // non-empty: unknown, with the reason

    bool known() const { return unknown.empty(); }
};

// Symbol tables, innermost first: the script's, the module's, the event's and
// the VMEConfig's (collect_symbol_tables, vme_config_scripts.cc:62-81). Each
// script gets fresh copies (parse_and_return_symbols, :40-55).
using Table = std::map<QString, Value>;
using Tables = std::vector<Table>;

Table to_table(const VariableTable &vars)
{
    Table result;
    for (const auto &[name, value]: vars)
        result[name] = Value{ value, {} };
    return result;
}

// lookup_variable (vme_script_variables.cc:49-56): innermost table first.
const Value *lookup(const QString &name, const Tables &tables)
{
    for (const auto &table: tables)
    {
        auto it = table.find(name);
        if (it != table.end())
            return &it->second;
    }
    return nullptr;
}

// handle_multiline_comment (vme_script.cc:956-993)
QString strip_multiline_comment(const QString &line, bool &inComment)
{
    QString result;
    int i = 0;

    while (i < line.size())
    {
        auto two = line.mid(i, 2);

        if (inComment)
        {
            if (two == "*/") { inComment = false; i += 2; }
            else ++i;
        }
        else if (two == "/*") { inComment = true; i += 2; }
        else result.append(line.at(i++));
    }

    return result;
}

// read_atomic_variable_reference / read_atomic_expression (vme_script.cc:999-1061).
bool read_until_close(std::istringstream &in, std::string &out, char open, char close)
{
    unsigned depth = 0;
    char c;

    while (in >> c)
    {
        out.push_back(c);
        if (c == open)
            ++depth;
        else if (c == close && (open == '{' || --depth == 0))
            return true;
    }
    return false;
}

// split_into_atomic_parts (vme_script.cc:1078-1178). False if a variable
// reference or expression is unterminated (a ParseError in mvme).
bool split_into_atomic_parts(const std::string &line, std::vector<std::string> &result)
{
    std::string part;
    std::istringstream in(line);
    in.unsetf(std::ios_base::skipws);

    while (!in.eof())
    {
        switch (in.peek())
        {
            case ' ':
            case '\t':
                in.get();
                if (!part.empty())
                {
                    result.push_back(part);
                    part.clear();
                }
                break;

            case '"':
                {
                    std::string quoted;
                    in >> std::quoted(quoted);
                    part.append(quoted);
                    auto c = in.peek();
                    if (c == ' ' || c == '\t')
                    {
                        result.push_back(part);
                        part.clear();
                    }
                }
                break;

            case '$':
                part.push_back(static_cast<char>(in.get()));
                if (in.peek() == '(')
                {
                    if (!read_until_close(in, part, '(', ')'))
                        return false;
                }
                else if (in.peek() == '{')
                {
                    if (!read_until_close(in, part, '{', '}'))
                        return false;
                }
                break;

            default:
                {
                    auto c = in.get();
                    if (c != std::istringstream::traits_type::eof())
                        part.push_back(static_cast<char>(c));
                }
        }
    }

    if (!part.empty())
        result.push_back(part);
    return true;
}

// expand_variables (vme_script.cc:1180-1227), then evaluate_expressions
// (:1644-1720), which this evaluator does not do: a part still holding
// "$(" is unknown.
void expand(Value &part, const Tables &tables)
{
    if (!part.known())
        return;

    const auto line = part.text.toStdString();
    std::istringstream in(line);
    in.unsetf(std::ios_base::skipws);
    QString result, name;
    bool inside = false;
    char c;

    while (in >> c)
    {
        if (!inside)
        {
            if (c == '$' && in.peek() == '{')
            {
                in.get();
                inside = true;
            }
            else
                result.push_back(c);
        }
        else if (c == '}')
        {
            const Value *var = lookup(name, tables);
            if (!var)
            {
                part.unknown = "undefined variable '" + name.toStdString() + "'";
                return;
            }
            if (!var->known())
            {
                part.unknown = "variable '" + name.toStdString() + "' is unknown (" + var->unknown + ")";
                return;
            }
            result += var->text;
            inside = false;
            name.clear();
        }
        else
            name.push_back(c);
    }

    if (inside)
    {
        part.unknown = "unterminated variable reference '${" + name.toStdString() + "'";
        return;
    }

    part.text = result;
    if (part.text.contains("$("))
        part.unknown = "expression '" + part.text.toStdString() + "' is not evaluated";
}

// parseBinaryLiteral and parseValue<u32> (vme_script.cc:101-194).
std::optional<uint32_t> parse_value(const QString &str)
{
    const QString lower = str.toLower();

    if (lower.startsWith("0b"))
    {
        uint32_t result = 0;
        unsigned shift = 0;
        for (int i = lower.size() - 1; i >= 2; --i)
        {
            const QChar c = lower.at(i);
            if (c == '\'')
                continue;
            if ((c != '0' && c != '1') || shift > 31)
                return {};
            result |= static_cast<uint32_t>(c == '1') << shift++;
        }
        return result;
    }

    bool ok = false;
    qulonglong val = 0;

    if (lower.startsWith("0x"))
        val = str.toULongLong(&ok, 0);
    else if (!str.contains('.'))
        val = str.toULongLong(&ok, 10);
    else
    {
        const auto fval = std::round(str.toFloat(&ok));
        if (fval < 0.0)
            return {};
        val = static_cast<qulonglong>(fval);
    }

    if (!ok || val > std::numeric_limits<uint32_t>::max())
        return {};
    return static_cast<uint32_t>(val);
}

// parseAddressMode, parseDataWidth, parseAddress (vme_script.cc:48-97).
bool valid_address_mode(const QString &str)
{
    for (auto name: { "a16", "a24", "a32", "cr" })
        if (str.compare(QString(name), Qt::CaseInsensitive) == 0)
            return true;
    bool ok = false;
    const auto value = str.toUInt(&ok, 0);
    return ok && value < std::numeric_limits<uint8_t>::max();
}

bool valid_data_width(const QString &str)
{
    return str.compare("d16", Qt::CaseInsensitive) == 0 || str.compare("d32", Qt::CaseInsensitive) == 0;
}

std::optional<uint32_t> parse_address(const QString &str)
{
    bool ok = false;
    const auto value = str.toUInt(&ok, 0);
    return ok ? std::optional<uint32_t>(value) : std::nullopt;
}

std::string hex(uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

class ScriptEvaluator
{
    public:
        ScriptEvaluator(const std::vector<uint32_t> &watched, std::vector<RegisterWrite> &writes)
            : watched_(watched), writes_(writes)
        { }

        // vme_script::parse (vme_script.cc:1803-1960) for the supported subset.
        void run(const ScriptConfig &script, Tables tables)
        {
            script_ = script.name.toStdString();

            struct Line { std::vector<std::string> parts; int number; bool ok; };
            std::vector<Line> lines;

            // pre_parse (vme_script.cc:1332-1386)
            bool inComment = false;
            int number = 0;
            for (auto text: script.text.split('\n'))
            {
                ++number;
                if (text.endsWith('\r'))
                    text.chop(1);
                text = strip_multiline_comment(text, inComment);
                const int hash = text.indexOf('#');
                if (hash >= 0)
                    text.resize(hash);
                text = text.trimmed();
                if (text.isEmpty())
                    continue;
                Line line{ {}, number, true };
                line.ok = split_into_atomic_parts(text.toStdString(), line.parts);
                if (!line.parts.empty())
                    lines.push_back(line);
            }

            for (size_t i = 0; i < lines.size(); ++i)
            {
                // Blocks: their contents are skipped (vme_script.cc:1840-1910,
                // find_index_of_next_command :1388-1408 compares exactly).
                const auto &first = lines[i].parts[0];
                const char *blockEnd = first == "meta_block_begin" ? "meta_block_end"
                    : first == "mvlc_custom_begin" ? "mvlc_custom_end"
                    : first == "mvlc_stack_begin" ? "mvlc_stack_end" : nullptr;
                if (blockEnd)
                {
                    while (i < lines.size() && lines[i].parts[0] != blockEnd)
                        ++i;
                    continue;
                }

                line_ = lines[i].number;
                std::vector<Value> parts;
                for (size_t p = 0; p < lines[i].parts.size(); ++p)
                {
                    Value v{ QString::fromStdString(lines[i].parts[p]), {} };
                    if (!lines[i].ok && p > 0)
                        v.unknown = "malformed line (unterminated variable reference or expression)";
                    parts.push_back(v);
                }
                evaluate(parts, tables);
            }
        }

    private:
        void evaluate(std::vector<Value> parts, Tables &tables)
        {
            for (auto &part: parts)
                expand(part, tables);

            if (parts[0].known())
                parts[0].text = parts[0].text.toLower();

            // The second split and expansion, except for `set` (vme_script.cc:1918-1929).
            if (!(parts[0].known() && parts[0].text == "set"))
            {
                std::vector<Value> reparsed;
                for (const auto &part: parts)
                {
                    std::vector<std::string> split;
                    if (!part.known())
                        reparsed.push_back(part);
                    else if (!split_into_atomic_parts(part.text.toStdString(), split))
                        reparsed.push_back({ part.text, "malformed part '" + part.text.toStdString() + "'" });
                    else
                        for (const auto &s: split)
                            reparsed.push_back({ QString::fromStdString(s), {} });
                }
                if (reparsed.empty())
                    return;
                parts = reparsed;
                for (auto &part: parts)
                    expand(part, tables);
            }

            const Value &p0 = parts[0];

            if (p0.known() && p0.text.isEmpty())
                return;

            if (!p0.known())
            {
                // Might be `<addr> <value>` with an unknown address.
                if (parts.size() == 2)
                    unknownWrite(std::nullopt, "address unknown: " + p0.unknown);
                return;
            }

            // set: the innermost table (vme_script.cc:1931-1945).
            if (p0.text == "set")
            {
                if (parts.size() < 2 || !parts[1].known())
                    return;
                if (parts.size() != 3)
                    tables[0][parts[1].text] = { {}, "malformed 'set' in '" + script_ + "' line "
                                                      + std::to_string(line_) };
                else if (!parts[2].known())
                    tables[0][parts[1].text] = { {}, parts[2].unknown + " in '" + script_ + "' line "
                                                      + std::to_string(line_) };
                else
                    tables[0][parts[1].text] = parts[2];
                return;
            }

            // The short form of write (handle_single_line_command, vme_script.cc:1414-1464).
            if (p0.text.at(0).isDigit() && parts.size() == 2)
            {
                const auto address = parse_value(p0.text);
                if (!address)
                    unknownWrite(std::nullopt, "invalid address '" + p0.text.toStdString() + "'");
                else
                    write(*address, parts[1]);
                return;
            }

            // parseWrite (vme_script.cc:240-256); writeabs is ignored.
            if (p0.text.toLower() == "write")
            {
                std::optional<uint32_t> address;
                if (parts.size() >= 4 && parts[3].known())
                    address = parse_address(parts[3].text);

                if (parts.size() != 5 || !parts[1].known() || !parts[2].known()
                    || !valid_address_mode(parts[1].text) || !valid_data_width(parts[2].text))
                    unknownWrite(address, "malformed write");
                else if (!address)
                    unknownWrite(std::nullopt, parts[3].known() ? "invalid address '" + parts[3].text.toStdString() + "'"
                                                                : "address unknown: " + parts[3].unknown);
                else
                    write(*address, parts[4]);
            }
        }

        bool isWatched(uint32_t address) const
        {
            return std::find(watched_.begin(), watched_.end(), address) != watched_.end();
        }

        void write(uint32_t address, const Value &value)
        {
            if (!isWatched(address))
                return;
            if (!value.known())
                return unknownWrite(address, value.unknown);
            const auto parsed = parse_value(value.text);
            if (!parsed)
                return unknownWrite(address, "invalid value '" + value.text.toStdString() + "'");
            writes_.push_back({ address, parsed, script_, line_, {} });
        }

        // An address that cannot be evaluated counts for every watched register.
        void unknownWrite(std::optional<uint32_t> address, const std::string &reason)
        {
            if (address && !isWatched(*address))
                return;
            for (auto a: watched_)
                if (!address || a == *address)
                    writes_.push_back({ a, std::nullopt, script_, line_, reason });
        }

        const std::vector<uint32_t> &watched_;
        std::vector<RegisterWrite> &writes_;
        std::string script_;
        int line_ = 0;
};

std::string origin(const RegisterWrite &w)
{
    return "'" + w.script + "' line " + std::to_string(w.line);
}

}

std::vector<RegisterWrite> evaluate_register_writes(const VMEConfig &vmeConfig, size_t eventIndex,
                                                    size_t moduleIndex, const std::vector<uint32_t> &watched)
{
    const auto &event = vmeConfig.events.at(eventIndex);
    const auto &module = event.modules.at(moduleIndex);
    std::vector<RegisterWrite> writes;
    ScriptEvaluator evaluator(watched, writes);

    // vmeconfig_to_crateconfig.cc:427-438: reset, then the init scripts, each
    // parsed with fresh symbol tables (vme_config_scripts.cc:40-81).
    for (const auto &script: module.initScripts)
        evaluator.run(script, { to_table(script.variables), to_table(module.variables),
                                to_table(event.variables), to_table(vmeConfig.variables) });

    return writes;
}

const DataSourceConfig *mdpp_sample_decoder(const AnalysisConfig &analysis, int eventIndex, int moduleIndex)
{
    if (eventIndex < 0 || static_cast<size_t>(eventIndex) >= analysis.dataSources.size())
        return nullptr;
    // The first in a2 order.
    for (const auto &src: analysis.dataSources[eventIndex])
        if (src.kind == DataSourceConfig::Kind::MdppSampleDecoder && src.moduleIndex == moduleIndex)
            return &src;
    return nullptr;
}

std::string why_no_waveforms(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                             const SamplingSetting &s)
{
    const auto &module = vmeConfig.events.at(s.eventIndex).modules.at(s.moduleIndex);

    if (!module.enabled)
        return "the module is disabled";
    // As a2 picks the decoder (a2.cc:779); mvme's UI sets a new decoder's
    // type to the module's (ui_eventwidget.cc:3229, analysis_ui_p.cc:790).
    if (!mesytec::mvme::mdpp_sampling::get_decoder_function(s.moduleType.c_str()))
        return "mvme's MDPP sample decoder does not support module type '" + s.moduleType + "'";
    if (s.sampling && !*s.sampling)
        return "sampling is off";
    // event_server.cc:500-507: no EventData message for such an event.
    if (static_cast<size_t>(s.eventIndex) >= analysis.dataSources.size()
        || analysis.dataSources[s.eventIndex].empty())
        return "its event has no data sources in the analysis, so mvme sends no event data for it";
    return {};
}

std::vector<std::vector<int>> waveform_tree_modules(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                                                    const std::vector<SamplingSetting> &settings)
{
    std::vector<std::vector<int>> result(vmeConfig.events.size());

    for (const auto &s: settings)
        if (why_no_waveforms(vmeConfig, analysis, s).empty())
            result.at(s.eventIndex).push_back(s.moduleIndex);

    for (auto &modules: result)
        std::sort(modules.begin(), modules.end());
    return result;
}

bool is_waveform_module(const AnalysisConfig &analysis, int eventIndex, int moduleIndex)
{
    if (eventIndex < 0 || static_cast<size_t>(eventIndex) >= analysis.waveformModules.size())
        return false;
    const auto &modules = analysis.waveformModules[eventIndex];
    return std::find(modules.begin(), modules.end(), moduleIndex) != modules.end();
}

const DataSourceConfig *reused_decoder(const VMEConfig &vmeConfig, const AnalysisConfig &analysis,
                                       int eventIndex, int moduleIndex)
{
    if (eventIndex < 0 || static_cast<size_t>(eventIndex) >= analysis.dataSources.size())
        return nullptr;
    const auto type = vmeConfig.events.at(eventIndex).modules.at(moduleIndex).typeName.toStdString();
    for (const auto &src: analysis.dataSources[eventIndex])
        if (src.kind == DataSourceConfig::Kind::MdppSampleDecoder && src.moduleIndex == moduleIndex
            && src.moduleTypeName == type)
            return &src;
    return nullptr;
}

std::vector<SamplingSetting> sampling_settings(const VMEConfig &vmeConfig, const AnalysisConfig &analysis)
{
    std::vector<SamplingSetting> result;

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        const auto &modules = vmeConfig.events[ei].modules;

        for (size_t mi = 0; mi < modules.size(); ++mi)
        {
            const auto &module = modules[mi];
            const bool mdpp = module.typeName.startsWith("mdpp");
            const bool decoded = mdpp_sample_decoder(analysis, ei, mi) != nullptr;

            if (!(module.enabled && mdpp) && !decoded)
                continue;

            SamplingSetting s;
            s.eventIndex = static_cast<int>(ei);
            s.moduleIndex = static_cast<int>(mi);
            s.module = module.name.toStdString();
            s.moduleType = module.typeName.toStdString();

            if (!mdpp)
            {
                // Other module families use 0x6044 for other things.
                s.samplingFrom = s.numberSamplesFrom = "module type '" + s.moduleType
                    + "' is not an MDPP: its sampling setting is not read";
                result.push_back(s);
                continue;
            }

            if (!module.enabled)
            {
                // vmeconfig_to_crateconfig.cc:423-424
                s.samplingFrom = s.numberSamplesFrom = "module disabled in the VME config: mvme does not initialize it";
                result.push_back(s);
                continue;
            }

            const auto writes = evaluate_register_writes(vmeConfig, ei, mi,
                                                         { MdppOutputFormatRegister, MdppNumberSamplesRegister });

            // Sampling: bit 4 of the last write to 0x6044.
            const RegisterWrite *last = nullptr;
            std::vector<const RegisterWrite *> samples;
            for (const auto &w: writes)
            {
                if (w.address == MdppOutputFormatRegister)
                    last = &w;
                else
                    samples.push_back(&w);
            }

            if (!last)
            {
                s.sampling = false;
                s.samplingFrom = "0x6044 not written (register default 0: off)";
            }
            else if (!last->value)
                s.samplingFrom = "0x6044 unknown in " + origin(*last) + ": " + last->reason;
            else
            {
                s.sampling = (*last->value >> 4) & 1u;
                s.samplingFrom = "0x6044 = " + hex(*last->value) + " in '" + last->script + "'";
            }

            // Number of samples: the one distinct value written to 0x6148.
            auto unknown = std::find_if(samples.begin(), samples.end(),
                                        [](const RegisterWrite *w) { return !w->value; });
            std::vector<uint32_t> distinct;
            for (auto w: samples)
                if (w->value && std::find(distinct.begin(), distinct.end(), *w->value) == distinct.end())
                    distinct.push_back(*w->value);

            if (samples.empty())
                s.numberSamplesFrom = "0x6148 not written";
            else if (unknown != samples.end())
                s.numberSamplesFrom = "0x6148 unknown in " + origin(**unknown) + ": " + (*unknown)->reason;
            else if (distinct.size() > 1)
            {
                s.numberSamplesFrom = "several values written to 0x6148:";
                for (size_t i = 0; i < samples.size(); ++i)
                    s.numberSamplesFrom += (i ? ", " : " ") + std::to_string(*samples[i]->value)
                        + " (" + origin(*samples[i]) + ")";
            }
            else
            {
                s.numberSamples = distinct[0];
                s.numberSamplesFrom = "0x6148 = " + std::to_string(distinct[0]) + " in '"
                    + samples.back()->script + "'";
            }

            result.push_back(s);
        }
    }

    return result;
}

WaveformConfigResult compare_waveform_config(const SamplingSetting &s, const ModuleWaveforms &m,
                                             const ModuleTraceCounts *traces, bool waveformsRequested)
{
    WaveformConfigResult r;

    if (traces)
    {
        r.traces = traces->traces;
        if (s.numberSamples)
            r.tracesWithConfiguredLength = *s.numberSamples < traces->tracesPerLength.size()
                ? traces->tracesPerLength[*s.numberSamples] : 0;
    }

    if (!waveformsRequested)
    {
        r.result = "skipped";
        r.message = "--no-waveforms";
        return r;
    }

    const bool none = traces && traces->traces == 0;
    const bool some = traces && traces->traces > 0;

    auto set = [&r](const char *result, const std::string &message)
    {
        r.result = result;
        r.message = message;
        return r;
    };

    if (!s.sampling)
        return set("note", "sampling unknown (" + s.samplingFrom + ")"
                   + (m.tree ? "" : "; no waveforms: " + m.whyNoTree));

    if (*s.sampling)
    {
        if (!m.tree)
            return set("warn", "sampling is on in the VME config, but FLIRT cannot decode this module's "
                               "samples (" + m.whyNoTree + "), so its waveforms are not extracted");
        if (none)
            return set("warn", "sampling is on in the VME config (" + s.samplingFrom
                               + "), but no traces were decoded for this module");
        if (!some)
            return set("pass", "sampling on (" + s.samplingFrom + ")");

        if (!s.numberSamples)
            return set("pass", "sampling on, " + std::to_string(traces->traces) + " traces; number of samples "
                               "not compared (" + s.numberSamplesFrom + ")");

        const auto n = std::to_string(*s.numberSamples);
        const auto with = *r.tracesWithConfiguredLength;
        const auto of = std::to_string(with) + " of " + std::to_string(traces->traces) + " traces";
        if (2 * with < traces->traces)
        {
            const auto &per = traces->tracesPerLength;
            const auto common = std::max_element(per.begin(), per.end()) - per.begin();
            return set("warn", "sampling on with " + n + " samples (" + s.numberSamplesFrom + "), but only "
                               + of + " have " + n + " samples (most common length: "
                               + std::to_string(common) + ")");
        }
        return set("pass", "sampling on, " + n + " samples; " + of + " have " + n + " samples");
    }

    // Sampling off: no tree; traces can only come from an analysis decoder.
    if (some)
    {
        const bool written = s.samplingFrom.rfind("0x6044 not written", 0) != 0;
        return set("warn", std::to_string(traces->traces) + " traces decoded by the analysis's MDPP sample decoder '"
                           + m.analysisSource + "', but "
                           + (written ? "sampling is off in the VME config (" + s.samplingFrom + ")"
                                      : std::string("the VME config never enables sampling (0x6044 not written)"))
                           + "; they are not in the waveforms file");
    }
    return set("pass", "sampling off: " + s.samplingFrom);
}

}
