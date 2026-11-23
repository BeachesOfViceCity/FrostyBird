#include "runtime.hpp"
#include "cues.hpp"
#include "graphics.hpp"
#include "menu_layout.hpp"
#include "music.hpp"
#include "screen_vert.h"
#include "matrix_frag.h"
#include <pango/pangocairo.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <format>
#include <memory>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace fb {
namespace {
constexpr unsigned mint = 0x9bd7ad, bright = 0xeff3ef, muted = 0xa0afa4, dark = 0x0a1510;
void color(cairo_t* c, unsigned h, double a = 1) {
    cairo_set_source_rgba(c, (h >> 16) / 255., ((h >> 8) & 255) / 255., (h & 255) / 255., a);
}
void box(cairo_t* c, int x, int y, int w, int h, unsigned col, double a = 1) {
    color(c, col, a);
    cairo_rectangle(c, x, y, w, h);
    cairo_fill(c);
}
void text_at(cairo_t* c, int x, int y, int w, int h, std::string_view s, int size, unsigned col,
             bool mono = false, bool bold = false, PangoAlignment align = PANGO_ALIGN_LEFT,
             PangoEllipsizeMode ellipsis = PANGO_ELLIPSIZE_END) {
    auto p = pango_cairo_create_layout(c);
    auto f = pango_font_description_new();
    pango_font_description_set_family(f, mono ? "DejaVu Sans Mono, monospace" : "DejaVu Sans, sans-serif");
    pango_font_description_set_absolute_size(f, size * PANGO_SCALE);
    pango_font_description_set_weight(f, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(p, f);
    pango_font_description_free(f);
    pango_layout_set_width(p, w * PANGO_SCALE);
    pango_layout_set_height(p, h * PANGO_SCALE);
    pango_layout_set_wrap(p, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(p, align);
    pango_layout_set_ellipsize(p, ellipsis);
    auto valid = g_utf8_make_valid(s.data(), s.size());
    pango_layout_set_text(p, valid, -1);
    g_free(valid);
    color(c, col);
    cairo_move_to(c, x, y);
    pango_cairo_show_layout(c, p);
    g_object_unref(p);
}
struct Picker {
    std::mutex m;
    std::optional<std::string> picked;
};
void SDLCALL selection(void* user, const char* const* paths, int) {
    std::unique_ptr<std::shared_ptr<Picker>> ref(static_cast<std::shared_ptr<Picker>*>(user));
    std::lock_guard lock((*ref)->m);
    (*ref)->picked = paths ? (paths[0] ? paths[0] : "") : "\n";
}
struct Job {
    pid_t pid{};
    int fd = -1;
    std::string buffer, err;
    void start(const fs::path& game) {
        int p[2];
        if (pipe2(p, O_CLOEXEC))
            throw Error("Cannot open progress pipe");
        posix_spawn_file_actions_t a;
        posix_spawn_file_actions_init(&a);
        posix_spawn_file_actions_adddup2(&a, p[1], 1);
        posix_spawn_file_actions_adddup2(&a, p[1], 2);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        auto exe = current_binary().string(), path = game.string();
        char op[] = "--worker";
        char* av[]{exe.data(), op, path.data(), nullptr};
        int rc = posix_spawn(&pid, exe.c_str(), &a, &attr, av, environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&a);
        close(p[1]);
        if (rc) {
            close(p[0]);
            pid = 0;
            throw Error("Cannot start installer worker");
        }
        fd = p[0];
        fcntl(fd, F_SETFL, O_NONBLOCK);
        err.clear();
        buffer.clear();
    }
    int tick(float& progress, std::string& status) {
        if (!pid)
            return -1;
        char b[4096];
        ssize_t n;
        while ((n = ::read(fd, b, sizeof b)) > 0)
            buffer.append(b, n);
        size_t end;
        while ((end = buffer.find('\n')) != buffer.npos) {
            auto s = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            auto sep = s.find('\t');
            if (sep != s.npos && sep && s.find_first_not_of("0123456789") == sep) {
                progress = std::clamp(std::stoi(s.substr(0, sep)) * .001f, 0.f, 1.f);
                status = s.substr(sep + 1);
            } else if (!s.empty())
                err = s;
        }
        int rc;
        auto exited = waitpid(pid, &rc, WNOHANG);
        if (exited == 0 || (exited < 0 && errno == EINTR))
            return -1;
        close(fd);
        fd = -1;
        pid = 0;
        return exited > 0 && WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
    }
    ~Job() {
        if (pid) {
            kill(-pid, SIGTERM);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            };
        }
        if (fd >= 0)
            close(fd);
    }
};
struct Button {
    int x, y, w, h, id;
};
struct Screen {
    int page, focus, hover;
    bool preview, muted, audio, picking, valid;
    float progress;
    std::string_view path, space, status, error;
};

void paint(cairo_t* c, cairo_surface_t* logo, const Screen& s, std::vector<Button>& buttons) {
    using namespace tb::layout;
    buttons.clear();
    auto text = [&](int x, int y, int w, int h, std::string_view value, int size, unsigned tint = bright,
                    bool mono = false, bool bold = false, PangoAlignment align = PANGO_ALIGN_LEFT,
                    PangoEllipsizeMode ellipsis = PANGO_ELLIPSIZE_END) {
        text_at(c, x, y, w, h, value, size, tint, mono, bold, align, ellipsis);
    };
    auto center = [&](int y, int h, std::string_view value, int size, unsigned tint = bright,
                      bool mono = false, bool bold = false) {
        text(240, y, 800, h, value, size, tint, mono, bold, PANGO_ALIGN_CENTER);
    };
    auto button = [&](int x, int y, int w, int h, int id, std::string_view value, bool primary = false,
                      bool enabled = true) {
        if (enabled)
            buttons.push_back({x, y, w, h, id});
        bool hot = enabled && s.hover == id;
        if (primary || hot)
            box(c, x, y, w, h, primary ? (hot ? 0xd5e9d9 : 0xb7d8bf) : 0x25382d, enabled ? 1. : .25);
        if (enabled && s.focus == id)
            box(c, x + 12, y + h - 3, w - 24, 1, primary ? 0x355941 : mint);
        text(x, y + (h - 21) / 2, w, 27, value, 16, enabled ? (primary ? dark : bright) : muted, false,
             primary, PANGO_ALIGN_CENTER);
    };
    auto brand = [&](int x, int y) {
        if (cairo_surface_status(logo) == CAIRO_STATUS_SUCCESS) {
            cairo_save(c);
            cairo_translate(c, x, y);
            cairo_scale(c, 35. / cairo_image_surface_get_height(logo),
                        35. / cairo_image_surface_get_height(logo));
            cairo_set_source_surface(c, logo, 0, 0);
            cairo_paint(c);
            cairo_restore(c);
        }
        text(x + 47, y + 5, 170, 31, "Frosty Bird", 19, bright, false, true);
    };
    auto sound = [&] {
        if (s.audio)
            button(24, 655, 137, 37, 3, s.muted ? "M   Sound off" : "M   Sound on");
    };

    // Tarta Bird's installation screen leaves the wizard behind. The entire
    // frame is the scene, with one progress line anchored at the bottom center.
    if (s.page >= 3) {
        brand(33, 30);
        sound();
        if (s.preview)
            text(1013, 49, 192, 25, "Preview", 15, muted, true, false, PANGO_ALIGN_RIGHT);
        if (!s.error.empty()) {
            center(292, 52, "Setup could not finish", 36, bright, false, true);
            center(362, 92, s.error, 18, 0xe9b8a6);
            button(565, 479, 150, 43, 0, "Try again", true);
        } else if (s.page == 4) {
            center(312, 62, s.preview ? "Preview complete" : "Ready to play", 43, bright, false, true);
            center(390, 44, s.preview ? "No files were installed." : "Launching The Matrix Awakens…", 19,
                   muted);
        } else {
            const char* headline = s.progress < .26f   ? "Setting up Frosty Bird"
                                   : s.progress < .84f ? "Compiling shaders"
                                                       : "Building Vulkan pipelines";
            center(523, 44, headline, 26, bright, false, true);
            center(575, 29, s.status, 15, 0xc0d6c5, true);
            center(636, 26, std::format("{:3.0f}%", s.progress * 100), 15, mint, true);
        }
        return;
    }

    box(c, 0, 0, rail, 720, 0x0b1710, .94);
    box(c, rail - 1, 0, 1, 720, 0x335141);
    brand(31, 37);
    const char* stages[] = {"Welcome", "Game files", "Install"};
    for (int i = 0; i < 3; ++i) {
        int y = 251 + i * 49;
        if (s.page == i)
            box(c, 29, y + 4, 2, 24, mint);
        text(45, y + 5, 35, 24, std::format("{:02}", i + 1), 13, s.page == i ? mint : muted, true);
        text(83, y, 168, 31, stages[i], 19, s.page == i ? bright : muted, false, s.page == i);
    }
    sound();
    text(left, 67, width, 28, s.preview ? "Setup preview" : "Setup for Linux", 14, muted, true);
    box(c, left, 111, width, 1, 0x335141);

    if (s.page == 0) {
        text(left - 4, 198, width + 4, 175, "THE MATRIX\nAWAKENS", 68, bright, false, true);
        text(left, 421, 630, 68, "Play the original Xbox Series release on your Linux PC.", 23, muted);
        text(left, 520, 650, 32, "Bring your own game files. No Steam or system Wine required.", 16, muted);
    } else if (s.page == 1) {
        text(left - 2, 198, width, 74, "Locate your game", 47, bright, false, true);
        text(left, 288, 720, 78,
             "Choose the folder containing Mount and EmbeddedXvd, or drop its Mount folder here.", 21, muted);
        box(c, left, 389, width, 57, 0x17281d, .96);
        text(left + 15, 406, width - 175, 28,
             s.preview        ? "Example package"
             : s.path.empty() ? "No folder selected"
                              : s.path,
             16, bright, true, false, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_MIDDLE);
        if (!s.preview)
            button(right - 145, 396, 134, 42, 2, s.picking ? "Opening…" : "Browse…", false, !s.picking);
        if (!s.error.empty())
            text(left, 475, width, 78, s.error, 17, 0xe9b8a6);
        else if (s.valid)
            text(left, 475, width, 35, "✓   Game files found", 17, mint);
    } else {
        text(left - 2, 198, width, 74, "Ready to install", 47, bright, false, true);
        text(left, 289, 756, 79,
             "Setup prepares movie playback and compiles the recorded shaders on this PC. Your game files "
             "stay in place.",
             21, muted);
        box(c, left, 398, width, 1, 0x335141);
        text(left, 424, width, 28, "Install location", 15, muted);
        text(left, 457, width, 36, s.path, 18, bright, true, false, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_MIDDLE);
        text(left, 515, width, 30, s.space, 17, mint);
    }
    box(c, rail, footerTop, 1280 - rail, 720 - footerTop, dark, .88);
    box(c, left, footerTop, width, 1, 0x335141);
    if (s.page > 0)
        button(877, 642, 149, 43, 1, "Back", false, !s.picking);
    button(1056, 642, 152, 43, 0,
           s.page == 2               ? "Install"
           : s.page == 1 && !s.valid ? "Select folder"
                                     : "Next",
           true, !s.picking);
}
} // namespace
bool wizard(const fs::path& root, const Home& home, fs::path source, bool mute, bool preview, bool noLaunch) {
    SDL_SetAppMetadata("Frosty Bird", "1", "org.frostybird.Setup");
    SDL_SetHint(SDL_HINT_APP_ID, "frostybird");
    if (!SDL_Init(SDL_INIT_VIDEO))
        throw Error(SDL_GetError());
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    tb::Graphics gfx;
    gfx.open(
        {{std::begin(screen_vert), std::end(screen_vert)}, {std::begin(matrix_frag), std::end(matrix_frag)}});
    auto logo = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>(
        cairo_image_surface_create_from_png((root / "assets/frostybird.png").c_str()), cairo_surface_destroy);
    tb::Music music;
    music.open(root / "assets/installer_frosty.ogg");
    Cues cues;
    cues.open(music.device());
    cues.level(mute ? 0.f : .44f);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    auto picker = std::make_shared<Picker>();
    bool picking = false, valid = false, dirty = true, done = false;
    int page = 0, focus = 0, hover = -1, pressed = -1;
    float progress = 0, painted = 0, install_mix = 0;
    std::string status, error, space;
    const auto destination = preview ? "~/…/frostybird" : home.data.string();
    std::string selected;
    Job job;
    tb::Opening opening;
    tb::OpeningPose shown;
    std::vector<Button> buttons;
    double start = SDL_GetTicks() / 1000., finished = 0, last = 0, phase_started = 0, duck_until = 0;
    auto now = [&] { return SDL_GetTicks() / 1000. - start; };
    auto cue = [&](Cue key) {
        cues.play(key);
        if (!mute && key != Cue::tick)
            duck_until = now() + (key == Cue::complete ? .9 : .32);
    };
    auto check = [&] {
        valid = preview;
        error.clear();
        selected = source.string();
        if (source.empty() || preview)
            return;
        try {
            source = verify(source).mount;
            selected = source.string();
            valid = true;
        } catch (const std::exception& e) {
            error = e.what();
        }
    };
    check();
    auto browse = [&] {
        if (preview || picking)
            return;
        picking = true;
        SDL_ShowOpenFolderDialog(selection, new std::shared_ptr<Picker>(picker), gfx.window(),
                                 source.empty() ? nullptr : source.c_str(), false);
    };
    auto action = [&](int id) {
        opening.finish();
        dirty = true;
        if (id == 3) {
            mute = !mute;
            cues.level(mute ? 0.f : .44f);
            if (!mute)
                cue(Cue::tick);
            return;
        }
        if (picking)
            return;
        if (id == 2 && page == 1) {
            cue(Cue::accept);
            browse();
            return;
        }
        if (id == 1 && page > 0 && page < 3) {
            cue(Cue::back);
            --page;
            return;
        }
        if (id != 0)
            return;
        if (page == 0) {
            cue(Cue::accept);
            page = 1;
            return;
        }
        if (page == 1) {
            if (!valid) {
                cue(Cue::accept);
                browse();
                return;
            }
            try {
                space = preview ? "Preview • no disk changes" : storage_summary(root, home);
                error.clear();
                page = 2;
                cue(Cue::accept);
            } catch (const std::exception& e) {
                error = e.what();
                cue(Cue::error);
            }
            return;
        }
        if (page == 3 && !error.empty()) {
            cue(Cue::back);
            page = 1;
            check();
            return;
        }
        if (page == 2) {
            page = 3;
            progress = painted = 0;
            phase_started = now();
            error.clear();
            status = "Checking the release";
            focus = 0;
            hover = pressed = -1;
            cue(Cue::link);
            if (!preview)
                try {
                    job.start(source);
                } catch (const std::exception& e) {
                    error = e.what();
                    cue(Cue::error);
                }
        }
    };
    auto hit = [&](float mx, float my) {
        auto [x, y] = gfx.logicalPoint(mx, my);
        if (page < 3) {
            if (shown.light <= 0)
                return -1;
            bool rail = x < tb::layout::rail;
            if (rail)
                x -= shown.railX;
            else if (y >= tb::layout::footerTop) {
                y -= shown.footerY;
                if (y < tb::layout::footerTop)
                    return -1;
            } else
                x -= shown.choicesX;
            if (rail != (x < tb::layout::rail))
                return -1;
        }
        for (auto b : buttons)
            if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
                return b.id;
        return -1;
    };
    if (!SDL_ShowWindow(gfx.window()))
        throw Error(SDL_GetError());
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                return false;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                dirty = true;
                break;
            case SDL_EVENT_DROP_FILE:
                if (page == 1 && !preview && ev.drop.data) {
                    source = ev.drop.data;
                    check();
                    cue(valid ? Cue::accept : Cue::error);
                    dirty = true;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                auto h = hit(ev.motion.x, ev.motion.y);
                if (h != hover) {
                    if (h >= 0 && h != focus)
                        cue(Cue::tick);
                    hover = h;
                    dirty = true;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    pressed = hit(ev.button.x, ev.button.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int h = hit(ev.button.x, ev.button.y);
                    if (h >= 0 && h == pressed)
                        action(h);
                    pressed = -1;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!ev.key.repeat) {
                    switch (ev.key.key) {
                    case SDLK_M:
                        action(3);
                        break;
                    case SDLK_F11:
                        SDL_SetWindowFullscreen(gfx.window(),
                                                !(SDL_GetWindowFlags(gfx.window()) & SDL_WINDOW_FULLSCREEN));
                        break;
                    case SDLK_ESCAPE:
                        if (page == 0 || page == 3)
                            return false;
                        action(1);
                        break;
                    case SDLK_LEFT:
                        action(1);
                        break;
                    case SDLK_RIGHT:
                        action(0);
                        break;
                    case SDLK_TAB:
                        if (!buttons.empty()) {
                            auto i = std::ranges::find(buttons, focus, &Button::id);
                            const bool back = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                            auto n = buttons.size();
                            auto at = i == buttons.end() ? (back ? 0 : n - 1) : size_t(i - buttons.begin());
                            focus = buttons[(at + (back ? n - 1 : 1)) % n].id;
                            cue(Cue::tick);
                            dirty = true;
                        }
                        break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        action(focus);
                        if (page != 1)
                            focus = 0;
                        break;
                    default:
                        break;
                    }
                }
                break;
            default:
                break;
            }
        }
        {
            std::lock_guard guard(picker->m);
            if (picker->picked) {
                auto s = *picker->picked;
                picker->picked.reset();
                picking = false;
                if (s == "\n") {
                    error = "Folder picker unavailable. Drag the folder onto this window.";
                    cue(Cue::error);
                } else if (!s.empty()) {
                    source = s;
                    check();
                    cue(valid ? Cue::accept : Cue::error);
                }
                dirty = true;
            }
        }
        double t = now(), dt = std::clamp(t - last, 0., .15);
        last = t;
        if (page == 3 && error.empty()) {
            auto old = status;
            auto prev = progress;
            int rc = -1;
            if (preview) {
                progress = std::min(1.f, float((t - phase_started) / 7.));
                status = progress < .26f ? "Checking the release"
                         : progress < .84f
                             ? std::format("{} of 5400 shader variants", int(5400 * (progress - .26f) / .58f))
                             : "Preparing pipelines for this GPU";
                if (progress == 1)
                    rc = 0;
            } else
                rc = job.tick(progress, status);
            if (rc == 0) {
                page = 4;
                finished = now();
                painted = 1.f;
                cue(Cue::complete);
                dirty = true;
            } else if (rc > 0) {
                error = job.err.empty() ? "Setup did not complete. Retry to resume." : job.err;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.c_str());
                cue(Cue::error);
                dirty = true;
            }
            dirty |= old != status || prev != progress;
        }
        const bool menu = page < 3;
        install_mix = std::clamp(install_mix + (menu ? -1.f : 1.f) * float(dt * 2.4), 0.f, 1.f);
        if (page == 3)
            painted = std::min(progress, painted + float(dt * .52));
        float fade = page == 4 ? std::clamp(float((t - finished - 1.55) / .7), 0.f, 1.f) : 0;
        float duck = t < duck_until ? .72f : 1.f;
        music.update(mute ? 0 : .30f * (1 - fade) * duck);
        cues.level(mute ? 0 : .44f * (1 - fade));
        shown = opening.pose(t);
        const bool bar = !menu && error.empty() && (page == 3 || page == 4);
        tb::Frame frame{float(t),
                        install_mix,
                        bar ? painted : -1,
                        fade,
                        menu,
                        shown,
                        page == 4 ? float(t - finished) : -1.f};
        Screen screen{page,     focus,
                      hover,    preview,
                      mute,     music.available() || cues.available(),
                      picking,  valid,
                      progress, page == 2 ? destination : selected,
                      space,    status,
                      error};
        if (gfx.present(frame, dirty, [&](cairo_t* c) { paint(c, logo.get(), screen, buttons); })) {
            dirty = false;
            opening.firstPresentation(t);
        }
        if (fade >= 1)
            done = true;
        SDL_Delay(5);
    }
    return !preview && !noLaunch;
}
} // namespace fb
