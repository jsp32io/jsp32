#include "app.hpp"
#include "font.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_opengl3.h>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void theme()
{
    ImGui::StyleColorsLight();
    auto &style = ImGui::GetStyle();
    style.WindowPadding = {18,14}; style.FramePadding = {9,5};
    style.ItemSpacing = {8,7}; style.CellPadding = {10,7};
    style.WindowRounding = 0; style.FrameRounding = 3; style.FrameBorderSize = 1;
    style.Colors[ImGuiCol_WindowBg] = {0.975f,0.978f,0.982f,1};
    style.Colors[ImGuiCol_FrameBg] = {0.96f,0.97f,0.985f,1};
    style.Colors[ImGuiCol_FrameBgHovered] = {0.93f,0.95f,0.97f,1};
    style.Colors[ImGuiCol_FrameBgActive] = {0.91f,0.94f,0.97f,1};
    style.Colors[ImGuiCol_Border] = {0.50f,0.56f,0.64f,1};
    style.Colors[ImGuiCol_TableRowBgAlt] = {0.95f,0.955f,0.965f,1};
    style.Colors[ImGuiCol_HeaderHovered] = {0.93f,0.80f,0.72f,1};
    style.Colors[ImGuiCol_HeaderActive] = {0.95f,0.68f,0.53f,1};
    style.Colors[ImGuiCol_Button] = {0.94f,0.45f,0.26f,1};
    style.Colors[ImGuiCol_ButtonHovered] = {0.98f,0.54f,0.34f,1};
    style.Colors[ImGuiCol_ButtonActive] = {0.82f,0.35f,0.19f,1};
    style.Colors[ImGuiCol_CheckMark] = {0.90f,0.40f,0.18f,1};
}
static int run_gui()
{
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_Window *window = SDL_CreateWindow("JSP32", 1100, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_GLContext gl = SDL_GL_CreateContext(window);
        if (!gl) throw std::runtime_error(SDL_GetError());
        SDL_GL_MakeCurrent(window, gl); SDL_GL_SetSwapInterval(1);
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        theme(); jsp32::load_ui_font();
        ImGui_ImplSDL3_InitForOpenGL(window, gl);
        ImGui_ImplOpenGL3_Init("#version 330");
        {
            jsp32::App app(window); bool running = true;
            while (running) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) running = false;
                }
                app.tick();
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
                app.draw(); ImGui::Render();
                int width, height; SDL_GetWindowSizeInPixels(window, &width, &height);
                glViewport(0,0,width,height);
                glClearColor(0.975f,0.978f,0.982f,1); glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                SDL_GL_SwapWindow(window);
            }
        }
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
        SDL_GL_DestroyContext(gl); SDL_DestroyWindow(window); SDL_Quit();
    } catch (const std::exception &error) {
#ifdef _WIN32
        int size = MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        std::wstring message(static_cast<size_t>(size), L'\0');
        if (size > 0) MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), size);
        MessageBoxW(nullptr, size > 0 ? message.c_str() : L"无法启动 JSP32", L"JSP32", MB_OK | MB_ICONERROR);
#else
        std::fprintf(stderr, "JSP32 GUI: %s\n", error.what());
#endif
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return run_gui(); }
#else
int main(int, char **) { return run_gui(); }
#endif
