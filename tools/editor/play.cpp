/* Play: the project's own game, built for Linux and run as a process of its
 * own (a crash there is not the editor's), drawing into the Game tab through
 * a shared file (host/embed.h) and taking the pad from it. */
#include "editor.h"
#include "ui.h"
#include "embed.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static pid_t game_pid;
static DMSEmbed* shm;
static std::string shm_path;
static SDL_Texture* game_tex;
static uint32_t shown_frame;
static bool building, game_focused, want_focus;

PlayState play_state() {
    if (game_pid) return shm && shm->paused ? PlayState::Paused : PlayState::Running;
    return building ? PlayState::Building : PlayState::Stopped;
}

const DMSEmbed* play_shared() { return game_pid ? shm : nullptr; }
bool play_has_keyboard() { return game_pid && game_focused; }

static bool map_shared() {
    if (shm) return true;
    shm_path = std::string(SDL_GetBasePath()) + "dms-editor-game.shm";
    int fd = open(shm_path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(DMSEmbed))) { if (fd >= 0) close(fd); return false; }
    void* m = mmap(nullptr, sizeof(DMSEmbed), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    shm = (DMSEmbed*)m;
    return true;
}

static void launch() {
    std::string name = ed.project.substr(0, ed.project.size() - 1);
    name = name.substr(name.find_last_of('/') + 1);
    std::string exe = ed.root + "build/" + name + "/host/" + name;
    if (!map_shared()) { ed_log("Play: cannot make %s", shm_path.c_str()); return; }
    memset(shm, 0, sizeof(*shm));
    shm->magic = DMS_EMBED_MAGIC;
    shown_frame = 0;

    int out[2];
    if (pipe(out)) return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]);
        close(out[1]);
        if (chdir(ed.project.c_str())) _exit(127);
        setenv("DMS_EMBED", shm_path.c_str(), 1);
        setenv("DMS_SCENE", ed.scene_rel.c_str(), 1);   /* a game may start there */
        execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(out[1]);
    if (pid < 0) { close(out[0]); ed_log("Play: cannot start %s", exe.c_str()); return; }
    game_pid = pid;
    want_focus = true;
    ed_log("Playing %s", name.c_str());
    int fd = out[0];
    std::thread([fd] {
        FILE* f = fdopen(fd, "r");
        char line[1024];
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            ed_log_async(line);
        }
        if (f) fclose(f);
    }).detach();
}

void play_start() {
    if (game_pid || building) return;
    if (ed.dirty) ed_save();
    building = true;
    ed_run("host", [](bool ok) {
        building = false;
        if (ok) launch();
        else ed_log("Play: the Linux build failed (see above)");
    });
}

void play_stop() {
    if (!game_pid) return;
    shm->quit = 1;
    shm->paused = 0;
    for (int i = 0; i < 50; i++) {
        if (waitpid(game_pid, nullptr, WNOHANG) == game_pid) { game_pid = 0; break; }
        SDL_Delay(10);
    }
    if (game_pid) {
        kill(game_pid, SIGKILL);
        waitpid(game_pid, nullptr, 0);
        game_pid = 0;
    }
    game_focused = false;
    ed_log("Stopped");
}

void play_pause() {
    if (game_pid) shm->paused = !shm->paused;
}

void play_step() {
    if (!game_pid) return;
    shm->paused = 1;
    shm->step = 1;
}

void play_poll() {
    if (!game_pid) return;
    int status;
    if (waitpid(game_pid, &status, WNOHANG) == game_pid) {
        game_pid = 0;
        game_focused = false;
        if (WIFSIGNALED(status)) ed_log("The game crashed (signal %d: %s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
        else ed_log("The game ended (exit %d)", WEXITSTATUS(status));
    }
}

/* The keyboard as the pad, as the Linux build's own window has it */
static void send_pad() {
    uint32_t b = 0;
    auto key = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
    if (game_focused) {
        if (key(ImGuiKey_W)) b |= CONT_DPAD_UP;
        if (key(ImGuiKey_S)) b |= CONT_DPAD_DOWN;
        if (key(ImGuiKey_A)) b |= CONT_DPAD_LEFT;
        if (key(ImGuiKey_D)) b |= CONT_DPAD_RIGHT;
        if (key(ImGuiKey_Space)) b |= CONT_A;
        if (key(ImGuiKey_C)) b |= CONT_B;
        if (key(ImGuiKey_X)) b |= CONT_X;
        if (key(ImGuiKey_R)) b |= CONT_Y;
        if (key(ImGuiKey_Enter)) b |= CONT_START;
    }
    shm->buttons = b;
    shm->joyx = game_focused ? 100 * (key(ImGuiKey_RightArrow) - key(ImGuiKey_LeftArrow)) : 0;
    shm->joyy = game_focused ? 100 * (key(ImGuiKey_DownArrow) - key(ImGuiKey_UpArrow)) : 0;
    shm->rtrig = game_focused && key(ImGuiKey_LeftShift) ? 255 : 0;
    shm->ltrig = game_focused && key(ImGuiKey_Z) ? 255 : 0;
}

void game_window(void* renderer) {
    if (want_focus) { ImGui::SetNextWindowFocus(); want_focus = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(WIN_GAME);
    ImGui::PopStyleVar();
    if (!open) { game_focused = false; ImGui::End(); return; }

    ImVec2 at = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + avail.x, at.y + avail.y), IM_COL32(12, 12, 14, 255));
    float w = avail.x, h = w * 0.75f;
    if (h > avail.y) { h = avail.y; w = h / 0.75f; }
    ImVec2 p(at.x + (avail.x - w) * 0.5f, at.y + (avail.y - h) * 0.5f);

    if (game_pid && shm) {
        if (!game_tex) {
            game_tex = SDL_CreateTexture((SDL_Renderer*)renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 480);
            SDL_SetTextureScaleMode(game_tex, SDL_SCALEMODE_NEAREST);
        }
        if (shm->frame != shown_frame) {
            SDL_UpdateTexture(game_tex, nullptr, shm->rgb, 640 * 3);
            shown_frame = shm->frame;
        }
        ImGui::SetCursorScreenPos(p);
        if (shown_frame) ImGui::Image((ImTextureID)(intptr_t)game_tex, ImVec2(w, h));
        else ImGui::Dummy(ImVec2(w, h));
        if (ImGui::IsItemClicked()) game_focused = true;
        if (!ImGui::IsWindowFocused()) game_focused = false;
        if (game_focused && ImGui::IsKeyPressed(ImGuiKey_Escape)) game_focused = false;
        send_pad();

        /* What it is doing, over the top */
        char info[128];
        snprintf(info, sizeof(info), "%.0f fps   frame %u%s", shm->fps, shm->frame, shm->paused ? "   PAUSED" : "");
        dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 16 + ImGui::CalcTextSize(info).x, p.y + 30), IM_COL32(0, 0, 0, 140), 4);
        dl->AddText(ImVec2(p.x + 12, p.y + 11), shm->paused ? ui::WARN : IM_COL32(230, 230, 235, 230), info);
        if (!shown_frame) {
            const char* t = "Starting...";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), ui::TEXT_DIM, t);
        }
        const char* hint = game_focused ? "WASD d-pad   arrows stick   Space A   C B   X X   R Y   Enter Start   Shift/Z triggers   Esc releases"
                                        : "Click the game to give it the keyboard";
        ImVec2 hs = ImGui::CalcTextSize(hint);
        dl->AddRectFilled(ImVec2(p.x + (w - hs.x) * 0.5f - 8, p.y + h - 30), ImVec2(p.x + (w + hs.x) * 0.5f + 8, p.y + h - 8), IM_COL32(0, 0, 0, 140), 4);
        dl->AddText(ImVec2(p.x + (w - hs.x) * 0.5f, p.y + h - 27), game_focused ? IM_COL32(200, 205, 215, 220) : ui::WARN, hint);
        if (game_focused) dl->AddRect(p, ImVec2(p.x + w, p.y + h), ui::ACCENT, 0, 0, 2.0f);
    } else {
        const char* t = play_state() == PlayState::Building ? "Building for Linux..." : "Press " ICON_PLAY " to run the game here";
        if (ui::font_big_icons) ImGui::PushFont(ui::font_big_icons, 0.0f);
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(at.x + (avail.x - ts.x) * 0.5f, at.y + (avail.y - ts.y) * 0.5f), ui::TEXT_DIM, t);
        if (ui::font_big_icons) ImGui::PopFont();
    }
    ImGui::End();
}

void play_shutdown() {
    play_stop();
    if (game_tex) SDL_DestroyTexture(game_tex);
    if (shm) munmap(shm, sizeof(*shm));
    if (!shm_path.empty()) unlink(shm_path.c_str());
}
