// flirt-tui (docs/design.md, "TUI"): the FLIRT conversion screen in the
// terminal. It runs <its own directory>/flirt-convert as a user would,
// through JobManager (src/ui/job.h), and shows job.cc's display fields as
// they are.
//
// Lifetime: SIGINT, SIGTERM and SIGHUP are blocked before any thread starts
// (FTXUI's too) and waited for by a thread, which ends the screen's loop.
// After the loop, with the terminal restored, a running job gets SIGINT and
// is waited for, so nothing is half-published.

#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iostream>
#include <mutex>
#include <pwd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/names.h"
#include "common/platform.h"
#include "flirt_version.h"
#include "input.h"
#include "model.h"
#include "settings.h"
#include "text.h"
#include "ui/files.h"
#include "ui/job.h"
#include "ui/start_args.h"
#include "view.h"

namespace
{

using namespace flirt::tui;

void usage(std::ostream &out)
{
    out << "usage: flirt-tui [<input>...] [-o <outdir>] [--no-mouse]\n"
        << "\n"
        << "The FLIRT conversion screen in the terminal: choose mvme listfiles (.zip), the\n"
        << "output folder and the options, then Convert. It runs the flirt-convert next to it.\n"
        << "An input directory adds its *.zip files (not recursive, in natural order).\n"
        << "The arguments only fill the screen; nothing starts until you press Convert.\n"
        << "Names must use only letters, digits, _ - . and spaces (folders above them: any):\n"
        << "a listfile with another name is refused when converted, another -o name at once.\n"
        << "In the screen, ? shows the keys. For scripts use flirt-convert.\n"
        << "The mouse works too (click, double-click, wheel); --no-mouse leaves it to the terminal, e.g. to select text.\n";
}

std::string hostname()
{
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0)
        return "";
    return name;
}

std::string home_dir()
{
    const char *home = std::getenv("HOME");
    if (home && *home == '/')
        return home;
    const auto pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}

// An argument as an absolute path, as the shell's working directory sees it.
std::string absolute(const std::string &path)
{
    if (!path.empty() && path[0] == '/')
        return flirt::normalize_path(path);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return {};
    return flirt::normalize_path(std::string(cwd) + "/" + path);
}

// The listfiles an argument names: a file, or a directory's listfiles as
// flirt-convert reads a directory (not recursive, natural order). False with
// the message if there are none.
bool argument_files(const std::string &arg, std::vector<FileEntry> &files, std::string &error)
{
    const auto path = absolute(arg);
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0)
    {
        error = "no such file or directory: " + arg;
        return false;
    }
    if (S_ISDIR(st.st_mode))
    {
        const auto listing = flirt::list_directory(path);
        if (listing.status != 200)
        {
            error = listing.body.value("error", "cannot read directory " + arg);
            return false;
        }
        size_t found = 0;
        for (const auto &e: listing.body["entries"])
            if (e["type"] == "file" && e["listfile"].get<bool>())
            {
                files.push_back({ join_path(listing.body["path"].get<std::string>(), e["name"].get<std::string>()),
                                  e["name"].get<std::string>(), e["id"].get<std::string>(),
                                  e["size"].get<int64_t>(), true });
                ++found;
            }
        if (!found)
        {
            error = "no *.zip files in directory " + arg;
            return false;
        }
        return true;
    }
    files.push_back({ path, base_name(path), std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino),
                      static_cast<int64_t>(st.st_size), true });
    return true;
}

}

int main(int argc, char *argv[])
{
    std::vector<std::string> inputs;
    std::string outputArg;
    bool haveOutput = false;
    bool mouse = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            usage(std::cout);
            return 0;
        }
        if (arg == "-o" || arg == "--output-dir")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: " << arg << " needs a directory\n";
                return 2;
            }
            outputArg = argv[++i];
            haveOutput = true;
            continue;
        }
        if (arg == "--no-mouse")
        {
            mouse = false;
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-')
        {
            std::cerr << "error: unknown option " << arg << "\n";
            usage(std::cerr);
            return 2;
        }
        inputs.push_back(arg);
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        std::cerr << "flirt-tui needs a terminal; for scripts use flirt-convert\n";
        return 2;
    }

    // Stop signals are waited for by a thread; blocked before any thread
    // starts (FTXUI's included), so every thread inherits the mask. SIGWINCH
    // stays with FTXUI.
    sigset_t stops;
    sigemptyset(&stops);
    for (int sig: { SIGINT, SIGTERM, SIGHUP })
        sigaddset(&stops, sig);
    sigprocmask(SIG_BLOCK, &stops, nullptr);
    signal(SIGPIPE, SIG_IGN);
    flirt::SignalWaiter stopSignals(stops);

    // flirt-convert next to this program (both in <prefix>/bin).
    char self[PATH_MAX];
    const auto exe = flirt::self_exe();
    if (exe.empty() || !realpath(exe.c_str(), self))
    {
        std::cerr << "error: cannot find this program's directory: " << std::strerror(errno) << "\n";
        return 1;
    }
    std::string convertExe = self;
    convertExe = convertExe.substr(0, convertExe.rfind('/')) + "/flirt-convert";
    if (access(convertExe.c_str(), X_OK) != 0)
    {
        std::cerr << "error: " << convertExe << " is missing; flirt-tui runs the flirt-convert next to it\n";
        return 1;
    }

    Model model;
    model.version = FLIRT_GIT_DESCRIBE;
    const auto pw = getpwuid(getuid());
    model.user = pw ? std::string(pw->pw_name) : std::string();
    model.host = hostname();
    model.home = home_dir();
    model.mouse = mouse;    // not remembered in the settings file
    // <prefix>/settings/flirt-tui.json beside <prefix>/bin, or FLIRT_TUI_SETTINGS.
    const auto settingsFile = settings_path(self);
    {
        std::string problem;
        apply_settings(model, load_settings(settingsFile, &problem));
        model.goError = problem;    // said once, beside Convert
    }
    // A failed save is said once, where nothing else is said (not over a
    // reason Convert did not start); the next failure says it otherwise.
    bool saveFailureSaid = false;
    auto remember = [&] {
        std::string problem;
        if (!save_settings(settingsFile, to_settings(model), &problem) && !saveFailureSaid && model.goError.empty())
        {
            model.goError = problem;
            saveFailureSaid = true;
        }
    };

    std::vector<FileEntry> files;
    for (const auto &input: inputs)
    {
        std::string error;
        if (!argument_files(input, files, error))
        {
            std::cerr << "error: " << error << "\n";
            return 2;
        }
    }
    add_files(model, files);
    if (haveOutput)
    {
        const auto output = absolute(outputArg);
        if (const auto refusal = flirt::name_refusal(flirt::own_name(output));
            !refusal.empty() && output != "/")
        {
            std::cerr << "error: output folder " << refusal << "\n";
            return 2;
        }
        // Arguments win over the remembered output folder, and replace it.
        model.outputDir = output;
        remember();
    }

    flirt::JobManager jobs(convertExe);
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    // Ctrl-C is a key in raw mode, handled by the screen (the quit dialog).
    screen.ForceHandleCtrlC(false);
    // The mouse, unless --no-mouse leaves it to the terminal (its own
    // selection, e.g. to copy the command).
    screen.TrackMouse(mouse);

    // Other threads post to the screen only while its loop runs: ExitNow()
    // resets FTXUI's task sender in the loop's thread.
    std::mutex gateMutex;
    bool gateClosed = false;
    // A posted closure alone does not redraw (FTXUI 6.1.9
    // ScreenInteractive::HandleTask), so Event::Custom follows it, unless
    // `redraw` is false.
    auto post = [&](std::function<void()> task, bool redraw = true) {
        std::lock_guard<std::mutex> lock(gateMutex);
        if (gateClosed)
            return;
        screen.Post(std::move(task));
        if (redraw)
            screen.PostEvent(ftxui::Event::Custom);     // draws again
    };
    std::string why = "quit";
    bool ranJob = false;
    Actions actions;
    actions.start = [&](const json &body) -> std::string {
        std::string error;
        const auto args = flirt::start_arguments(body, error);
        if (args.empty())
            return error;
        bool busyNow = false;
        if (!jobs.start(args, error, busyNow))
            return error;
        ranJob = true;
        const auto state = jobs.state(0, std::chrono::milliseconds(0));
        model.job = state["job"];
        model.jobVersion = state["version"].get<uint64_t>();
        return {};
    };
    actions.cancel = [&] { jobs.cancel(); };
    actions.quit = [&] {
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            gateClosed = true;
        }
        screen.Exit();
    };
    actions.save = remember;

    auto view = ftxui::Renderer([&] {
        const auto size = ftxui::Terminal::Size();
        // FTXUI's TrackMouse turns on modes 1000, 1003, 1015 and 1006; with
        // 1003 every mouse move is an event and a full redraw, heavy over ssh.
        // In xterm-like terminals the tracking mode is one setting, so 1002
        // here switches it to presses, releases, the wheel and motion only
        // while a button is held (tmux then shows
        // #{mouse_button_flag} 1, #{mouse_standard_flag} 0, #{mouse_all_flag}
        // 0); 1006 (SGR) stays. Written before the frame, every frame: FTXUI
        // turns 1003 on again when it resumes after Ctrl-Z, and still turns
        // every mode off on exit and in its signal handlers. external/ is never
        // patched.
        if (mouse)
            std::cout << "\x1b[?1002h";
        return render(model, size.dimx, size.dimy);
    });
    auto root = ftxui::CatchEvent(view, [&](ftxui::Event event) {
        const auto size = ftxui::Terminal::Size();
        actions.width = size.dimx;
        actions.height = size.dimy;
        const bool used = handle_event(model, event, actions);
        return used;
    });

    std::atomic<bool> stopThreads(false);
    // Each new state of the job, and once a second while one runs (the
    // elapsed time).
    std::thread stateThread([&] {
        uint64_t after = 0;
        while (!stopThreads)
        {
            auto state = jobs.state(after, std::chrono::seconds(1));
            if (stopThreads)
                break;
            const auto version = state["version"].get<uint64_t>();
            const auto &job = state["job"];
            const bool running = job.is_object() && job.value("state", "") != "finished";
            if (version > after || running)
            {
                after = version;
                post([&, job, version] {
                    if (version >= model.jobVersion)
                    {
                        model.job = job;
                        model.jobVersion = version;
                        refresh(model, actions);
                    }
                });
            }
        }
    });
    std::thread signalThread([&] {
        while (!stopThreads)
        {
            const int sig = stopSignals.wait({ 0, 200000000 });
            if (sig != SIGINT && sig != SIGTERM && sig != SIGHUP)
                continue;
            post([&, sig] {
                why = sig == SIGINT ? "SIGINT" : sig == SIGTERM ? "SIGTERM" : "SIGHUP";
                actions.quit();
            });
            break;
        }
    });

    screen.Loop(root);

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        gateClosed = true;
    }
    const auto atExit = jobs.state(0, std::chrono::milliseconds(0));
    if (atExit["job"].is_object() && atExit["job"].value("state", "") != "finished")
        std::cerr << "flirt-tui: stopping the running conversion...\n" << std::flush;
    const bool wasRunning = jobs.stop_and_wait();
    const auto last = jobs.state(0, std::chrono::milliseconds(0));
    stopThreads = true;
    jobs.shutdown();
    stateThread.join();
    signalThread.join();

    // A plain quit says nothing; a signal or an interrupted conversion does.
    if (why != "quit" || wasRunning)
        std::cerr << "flirt-tui stopped (" << why << ")"
                  << (wasRunning ? "; the running conversion was interrupted, nothing half-published" : "") << "\n";
    if (ranJob && last["job"].is_object())
        std::cout << command_intro(model.host) << ": " << last["job"].value("commandLine", "") << "\n";
    return 0;
}
