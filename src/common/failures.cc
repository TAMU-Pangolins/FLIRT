#include "failures.h"

#include <cctype>
#include <vector>

namespace flirt
{

namespace
{

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}

void replace_all(std::string &s, const std::string &from, const std::string &to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

// The check IDs of src/flirt/integrity.h (namespace checks); this library
// does not see that header (it needs mesytec-mvlc).
constexpr auto Archive = "I1-archive";
constexpr auto ListfileEntry = "I2-listfile-entry";
constexpr auto AnalysisEntry = "I3-analysis-entry";
constexpr auto InputUnchanged = "I5-input-unchanged";
constexpr auto OutputsNotInputs = "I6-outputs-not-inputs";

// What flirt-tui and flirt-convert call the same thing.
std::string name(FailureFor where, const std::string &tui, const std::string &cli)
{
    return where == FailureFor::Tui ? tui : cli;
}

// Where flirt-convert's messages are.
std::string messages(FailureFor where)
{
    return name(where, "The Log (l) has flirt-convert's messages.", "flirt-convert's messages above say more.");
}

// A failed check's note: "<check>: fail|unavailable: <detail>".
bool parse_check(const std::string &note, std::string &check, std::string &result, std::string &detail)
{
    if (note.size() < 3 || note[0] != 'I' || !std::isdigit(static_cast<unsigned char>(note[1])))
        return false;
    const auto colon = note.find(": ");
    if (colon == std::string::npos)
        return false;
    const auto second = note.find(": ", colon + 2);
    check = note.substr(0, colon);
    result = second == std::string::npos ? note.substr(colon + 2) : note.substr(colon + 2, second - colon - 2);
    detail = second == std::string::npos ? std::string() : note.substr(second + 2);
    return true;
}

Failure failed_check(const std::string &check, const std::string &result, const std::string &detail,
                     FailureFor where)
{
    Failure f;
    f.check = check;
    f.why = plain_check_detail(detail);
    // "unavailable": the bytes could not be compared with the archive's own
    // record (not read to the end, nothing declared): not proven damaged.
    const bool proven = result == "fail";
    const std::string accept = name(where, "tick Advanced → Accept damaged input", "add --accept-damaged-input");
    const std::string copyAgain = "If this file is a copy, copy it again from the DAQ PC. ";
    if (check == Archive)
    {
        f.headline = "the archive cannot be read";
        // Its advice goes to the to-do: "cannot read the ZIP archive or its
        // central directory (<error>): if mvme is still writing ..." (listfile.cc).
        const auto advice = detail.find("): if mvme");
        if (advice != std::string::npos)
            f.why = plain_check_detail(detail.substr(0, advice + 1));
        f.todo = "If mvme is still writing this run, wait until it has finished. Otherwise mvme's listfile "
                 "recovery (mvme_listfile_recovery_gui) may be able to recover the data.";
    }
    else if (check == ListfileEntry)
    {
        f.headline = proven ? "the listfile in the archive is damaged"
                            : "the listfile in the archive could not be checked against the archive's own record";
        f.todo = copyAgain + "To convert what can be read anyway, " + accept + ".";
    }
    else if (check == AnalysisEntry)
    {
        f.headline = proven ? "the analysis file in the archive is damaged"
                            : "the analysis file in the archive could not be checked against the archive's own record";
        f.todo = copyAgain + "To convert with it anyway, " + accept + ".";
    }
    else if (check == InputUnchanged)
    {
        f.headline = "an input file changed while it was converted";
        if (starts_with(detail, "changed during the conversion: "))
            f.why = detail.substr(31) + ": its size, modification time or inode changed";
        f.todo = "Convert it again once nothing writes to the file any more.";
    }
    else if (check == OutputsNotInputs)
    {
        f.headline = "an output would replace an input file";
        f.todo = "Choose another output folder.";
    }
    else
    {
        f.headline = "check " + check + " failed";
        f.todo = messages(where);
    }
    return f;
}

// "refused: <what>": what flirt-convert does not support (README "Input
// integrity checks", the last paragraph).
Failure unsupported(const std::string &what)
{
    Failure f;
    f.why = what;
    const std::string mvme = "Convert this run with mvme.";
    // First: the name is any text (names.h name_refusal).
    if (contains(what, ": name not accepted: "))
    {
        f.headline = "the listfile's name is not accepted";
        f.todo = "Rename the listfile (or a copy of it) to a name with only letters, digits, _ - . and spaces, "
                 "then convert it.";
    }
    else if (contains(what, "is one part of a run split"))
    {
        f.headline = "one part of a run that mvme split into several files";
        f.todo = "Split runs are not supported yet: convert this run with mvme, and turn listfile splitting off "
                 "in mvme for new runs.";
    }
    else if (contains(what, "LZ4-compressed"))
    {
        f.headline = "the listfile is LZ4-compressed";
        f.todo = "Its integrity cannot be checked, so FLIRT does not convert it. " + mvme;
    }
    else if (starts_with(what, "classic .mvmelst") || starts_with(what, "unknown listfile format")
             || starts_with(what, "no listfile found") || starts_with(what, "listfile has no "))
    {
        f.headline = "not an MVLC listfile FLIRT can read";
        f.todo = "FLIRT converts mvme's MVLC listfiles only. " + mvme;
    }
    else if (contains(what, "events that read out data"))
    {
        f.headline = "the run has several VME events";
        f.todo = "Runs with more than one VME event are not supported yet. " + mvme;
    }
    else if (contains(what, "VME config") || contains(what, "VME controller") || contains(what, "VMEConfig")
             || starts_with(what, "crate config") || starts_with(what, "event '"))
    {
        f.headline = "the recording's VME setup is not supported";
        f.todo = mvme;
    }
    else
    {
        f.headline = "the input is not supported";
        f.todo = mvme;
    }
    return f;
}

}

std::string separate_thousands(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size();)
    {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
        {
            out += text[i++];
            continue;
        }
        size_t end = i;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
            ++end;
        const auto digits = text.substr(i, end - i);
        // Alone: a space or '(' before it, a space or ),;: after it (not
        // part of a name, path, hex number, decimal or version), no leading zero.
        const bool alone = (i == 0 || text[i - 1] == ' ' || text[i - 1] == '(')
            && (end == text.size() || std::string(" ),;:").find(text[end]) != std::string::npos)
            && digits[0] != '0';
        if (alone && digits.size() >= 4)
            for (size_t k = 0; k < digits.size(); ++k)
            {
                if (k && (digits.size() - k) % 3 == 0)
                    out += ',';
                out += digits[k];
            }
        else
            out += digits;
        i = end;
    }
    return out;
}

std::string plain_check_detail(const std::string &detail)
{
    // The entry: "'<entry>': <rest>" (EntryCheck::detail) or "cannot read
    // '<entry>': <error>" (listfile.cc).
    std::string entry, rest = detail;
    if (starts_with(detail, "'"))
    {
        const auto end = detail.find("': ");
        if (end != std::string::npos)
        {
            entry = detail.substr(1, end - 1);
            rest = detail.substr(end + 3);
        }
    }
    else if (starts_with(detail, "cannot read '"))
    {
        const auto end = detail.find("': ", 13);
        if (end != std::string::npos)
        {
            entry = detail.substr(13, end - 13);
            rest = "cannot be read: " + detail.substr(end + 3);
        }
    }
    replace_all(rest, "the central directory declares no", "the archive's index gives no");
    replace_all(rest, "the central directory declares", "the archive's index says");
    replace_all(rest, "its central directory", "its index");
    replace_all(rest, "the central directory", "the archive's index");
    replace_all(rest, "central directory", "index");
    rest = separate_thousands(rest);
    return entry.empty() ? rest : entry + ": " + rest;
}

Failure explain_failure(const std::string &result, const std::string &note, bool diskFull, FailureFor where)
{
    Failure f;
    std::string check, checkResult, detail;
    if (result == "interrupted")
    {
        f.headline = "stopped before it finished";
        f.why = note.empty() ? "nothing was published" : note;
        f.todo = name(where, "Convert again (s) to finish it.",
                      "Run the same command again.");
        return f;
    }
    if (result == "unsupported")
        return unsupported(starts_with(note, "refused: ") ? note.substr(9) : note);
    if (result != "failed")
        return f;

    if (parse_check(note, check, checkResult, detail))
        return failed_check(check, checkResult, detail, where);

    const std::string what = starts_with(note, "error: ") ? note.substr(7) : note;
    f.why = separate_thousands(what);
    const std::string log = messages(where);
    if (diskFull)
    {
        f.headline = "the output disk is full";
        f.todo = "Free space on it, or choose another output folder, and convert again. Nothing was published.";
    }
    else if (starts_with(what, "output directory does not exist"))
    {
        f.headline = "the output folder does not exist";
        f.todo = "Create it, or choose another output folder.";
    }
    else if (starts_with(what, "error writing") || starts_with(what, "cannot create"))
    {
        f.headline = "an output file could not be written";
        f.todo = "Check the output folder's disk (free space, permissions) and convert again. " + log;
    }
    else if (starts_with(what, "no mvme module templates found"))
    {
        f.headline = "mvme's module templates are missing next to FLIRT";
        f.todo = "FLIRT's installation is incomplete: install it again.";
    }
    else if (starts_with(what, "killed by ") && contains(what, "without their report"))
    {
        f.headline = "the conversion ended while it was publishing its files";
        f.todo = "Remove the files named here (without their report they are an incomplete set), then convert "
                 "again.";
    }
    else if (starts_with(what, "killed by ") || starts_with(what, "exit code "))
    {
        f.headline = "the conversion ended unexpectedly";
        f.todo = "Convert it again; if it happens again, report it with flirt-convert's messages.";
    }
    else
    {
        f.headline = "the conversion failed";
        f.todo = log;
    }
    return f;
}

}
