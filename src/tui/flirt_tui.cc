// flirt-tui (docs/design.md, "TUI"): the FLIRT conversion screen in the
// terminal. It runs <its own directory>/flirt-convert as a user would,
// through JobManager (src/ui/job.h), and shows job.cc's display fields as
// they are.
//
// Lifetime: SIGINT, SIGTERM and SIGHUP are blocked before any thread starts
// (FTXUI's too) and waited for by a thread, which ends the screen's loop.
// After the loop, with the terminal restored, a running job gets SIGINT and
// is waited for, so nothing is half-published.
//
// The Waveforms tab reads through <its own directory>/flirt-waveforms
// (flirt::WaveformReader), from one worker thread that keeps only the latest
// request; it stops when the tab shows no file (box 1 back on Listfiles) and at
// exit. The play is stepped by a ticker thread that posts ticks through the
// same gate.

#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <condition_variable>
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
#include "ui/waveforms.h"
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
    // Over ssh the waveform view draws at most 100 traces at once and plays
    // at most 10 events a second.
    const char *sshConnection = std::getenv("SSH_CONNECTION");
    model.ssh = sshConnection && *sshConnection;
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
    flirt::WaveformReader waveReader(convertExe.substr(0, convertExe.rfind('/')) + "/flirt-waveforms");
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
    // `redraw` is false (the play's ticks, which draw only when they change
    // something).
    auto post = [&](std::function<void()> task, bool redraw = true) {
        std::lock_guard<std::mutex> lock(gateMutex);
        if (gateClosed)
            return;
        screen.Post(std::move(task));
        if (redraw)
            screen.PostEvent(ftxui::Event::Custom);     // draws again
    };
    // From the loop's thread: draws again, unless the loop is ending.
    auto redraw = [&] {
        std::lock_guard<std::mutex> lock(gateMutex);
        if (!gateClosed)
            screen.PostEvent(ftxui::Event::Custom);
    };

    // The play: a ticker thread wakes every 10 ms (at 20 ms the ticks' phase
    // against 50/s steps gave about 40 events a second) while the model plays
    // (mirrored in `playing` after each event and task) and posts a tick; one
    // tick waits at a time.
    std::mutex tickMutex;
    std::condition_variable tickCv;
    std::atomic<bool> playing(false), tickPending(false), tickerQuit(false);
    auto mirror_play = [&] {
        const bool now = model.playing;
        if (playing.exchange(now) != now && now)
        {
            // Under the ticker's mutex: it cannot miss the wake-up.
            { std::lock_guard<std::mutex> lock(tickMutex); }
            tickCv.notify_all();
        }
    };
    auto steady_ms = [] {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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

    // The waveform view's worker: asks the reader for the latest request only
    // (older ones not sent yet are dropped) and posts each answer.
    std::mutex waveMutex;
    std::condition_variable waveCv;
    json wavePending;
    bool waveQuit = false;
    std::thread waveThread;
    auto waveWorker = [&] {
        for (;;)
        {
            json request;
            {
                std::unique_lock<std::mutex> lock(waveMutex);
                waveCv.wait(lock, [&] { return waveQuit || !wavePending.is_null(); });
                if (waveQuit)
                    return;
                request.swap(wavePending);
            }
            auto answer = waveReader.ask(request);
            // The reader's own errors carry no id or file: the request's.
            if (!answer.is_object())
                answer = { {"error", "flirt-waveforms answered with something that is not JSON"} };
            if (answer.find("id") == answer.end() || answer["id"].is_null())
                answer["id"] = request["id"];
            if (answer.find("file") == answer.end())
                answer["file"] = request["file"];
            post([&, answer] {
                apply_waveform_answer(model, answer);
                // The next step at once if it is due (a slow read slows the play).
                play_tick(model, steady_ms());
                send_waveform_request(model, actions);
                mirror_play();
            });
        }
    };
    actions.waveforms = [&](const json &request) {
        {
            std::lock_guard<std::mutex> lock(waveMutex);
            wavePending = request;
        }
        if (!waveThread.joinable())
            waveThread = std::thread(waveWorker);
        waveCv.notify_all();
    };
    // In the loop's thread, and after it: no flirt-waveforms outlives the view.
    actions.closeWaveforms = [&] {
        {
            std::lock_guard<std::mutex> lock(waveMutex);
            waveQuit = true;
            wavePending = nullptr;
        }
        waveCv.notify_all();
        waveReader.stop();      // a waiting ask() returns at once
        if (waveThread.joinable())
            waveThread.join();
        waveReader.stop();      // one started meanwhile
        std::lock_guard<std::mutex> lock(waveMutex);
        waveQuit = false;
    };

    auto view = ftxui::Renderer([&] {
        const auto size = ftxui::Terminal::Size();
        // FTXUI's TrackMouse turns on modes 1000, 1003, 1015 and 1006; with
        // 1003 every mouse move is an event and a full redraw, heavy over ssh.
        // In xterm-like terminals the tracking mode is one setting, so 1002
        // here switches it to presses, releases, the wheel and motion only
        // while a button is held (the play bar's drag; tmux then shows
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
        mirror_play();
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
                        // A run's waveforms file into the Waveforms tab once it is published.
                        refresh(model, actions);
                    }
                });
            }
        }
    });
    std::thread tickerThread([&] {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(tickMutex);
                tickCv.wait(lock, [&] { return tickerQuit.load() || playing.load(); });
                if (tickerQuit)
                    return;
            }
            if (!tickPending.exchange(true))
                post([&] {
                    tickPending = false;
                    const bool changed = play_tick(model, steady_ms());
                    send_waveform_request(model, actions);
                    mirror_play();
                    if (changed)
                        redraw();
                }, false);
            std::unique_lock<std::mutex> lock(tickMutex);
            tickCv.wait_for(lock, std::chrono::milliseconds(10), [&] { return tickerQuit.load(); });
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
    actions.closeWaveforms();
    const auto atExit = jobs.state(0, std::chrono::milliseconds(0));
    if (atExit["job"].is_object() && atExit["job"].value("state", "") != "finished")
        std::cerr << "flirt-tui: stopping the running conversion...\n" << std::flush;
    const bool wasRunning = jobs.stop_and_wait();
    const auto last = jobs.state(0, std::chrono::milliseconds(0));
    stopThreads = true;
    jobs.shutdown();
    stateThread.join();
    signalThread.join();
    {
        std::lock_guard<std::mutex> lock(tickMutex);
        tickerQuit = true;
    }
    tickCv.notify_all();
    tickerThread.join();

    // A plain quit says nothing; a signal or an interrupted conversion does.
    if (why != "quit" || wasRunning)
        std::cerr << "flirt-tui stopped (" << why << ")"
                  << (wasRunning ? "; the running conversion was interrupted, nothing half-published" : "") << "\n";
    if (ranJob && last["job"].is_object())
        std::cout << command_intro(model.host) << ": " << last["job"].value("commandLine", "") << "\n";
    return 0;
}
