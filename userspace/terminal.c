#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "syscalls.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define FONT_SIZE 16
#define MAX_LINES 100
#define VISIBLE_LINES 35
#define MAX_COLS 100

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
TTF_Font *font = NULL;

char lines[MAX_LINES][MAX_COLS];
int line_count = 0;
int scroll_offset = 0;

char input_buffer[MAX_COLS];
int input_len = 0;

void terminal_print(const char *format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Split by newline
    char *line = strtok(buffer, "\n");
    while (line) {
        if (line_count < MAX_LINES) {
            strncpy(lines[line_count], line, MAX_COLS - 1);
            line_count++;
        } else {
            // Shift up
            for (int i = 0; i < MAX_LINES - 1; i++) {
                strncpy(lines[i], lines[i + 1], MAX_COLS);
            }
            strncpy(lines[MAX_LINES - 1], line, MAX_COLS - 1);
        }
        line = strtok(NULL, "\n");
    }
    
    // Auto scroll to bottom
    if (line_count > VISIBLE_LINES) {
        scroll_offset = line_count - VISIBLE_LINES;
    }
}

void execute_command(char *cmd) {
    terminal_print("$ %s\n", cmd);

    if (strcmp(cmd, "help") == 0) {
        terminal_print("Available commands:\n");
        terminal_print("  help     - Show this help\n");
        terminal_print("  clear    - Clear screen\n");
        terminal_print("  ls       - List files\n");
        terminal_print("  doom     - Launch Doom\n");
        terminal_print("  shutdown - Power off\n");
    } else if (strcmp(cmd, "clear") == 0) {
        line_count = 0;
        scroll_offset = 0;
    } else if (strcmp(cmd, "shutdown") == 0) {
        syscall0(SYS_SHUTDOWN);
    } else if (strcmp(cmd, "doom") == 0) {
        terminal_print("Launching Doom...\n");
        syscall1(SYS_PROC_EXEC, (long)"doom.elf");
    } else if (strcmp(cmd, "ls") == 0) {
        DIR *d;
        struct dirent *dir;
        d = opendir(".");
        if (d) {
            while ((dir = readdir(d)) != NULL) {
                terminal_print("%s\n", dir->d_name);
            }
            closedir(d);
        } else {
            terminal_print("Failed to open directory.\n");
        }
    } else if (strlen(cmd) > 0) {
        terminal_print("Unknown command: %s\n", cmd);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() == -1) {
        printf("TTF_Init failed: %s\n", TTF_GetError());
        return 1;
    }

    window = SDL_CreateWindow("RaeenOS Terminal",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WINDOW_WIDTH, WINDOW_HEIGHT,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        printf("Window creation failed: %s\n", SDL_GetError());
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("Renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }

    font = TTF_OpenFont("FreeMono.ttf", FONT_SIZE);
    if (!font) {
        // Fallback to absolute path if needed, usually initrd root is /
        font = TTF_OpenFont("/FreeMono.ttf", FONT_SIZE);
        if (!font) {
             printf("Failed to load font: %s\n", TTF_GetError());
             return 1;
        }
    }

    SDL_StartTextInput();
    terminal_print("Welcome to RaeenOS Terminal!\nType 'help' for commands.\n");

    int running = 1;
    SDL_Event e;

    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_TEXTINPUT) {
                if (input_len < MAX_COLS - 1) {
                    strcat(input_buffer, e.text.text);
                    input_len += strlen(e.text.text);
                }
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_BACKSPACE && input_len > 0) {
                    input_buffer[--input_len] = '\0';
                } else if (e.key.keysym.sym == SDLK_RETURN) {
                    execute_command(input_buffer);
                    input_buffer[0] = '\0';
                    input_len = 0;
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_Color textColor = {255, 255, 255, 255};
        
        int y = 0;
        int start_line = scroll_offset;
        int end_line = start_line + VISIBLE_LINES;
        if (end_line > line_count) end_line = line_count;

        for (int i = start_line; i < end_line; i++) {
            SDL_Surface *surface = TTF_RenderText_Solid(font, lines[i], textColor);
            if (surface) {
                SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
                SDL_Rect dest = {10, y, surface->w, surface->h};
                SDL_RenderCopy(renderer, texture, NULL, &dest);
                SDL_FreeSurface(surface);
                SDL_DestroyTexture(texture);
                y += FONT_SIZE;
            }
        }

        // Render Input Line
        char prompt[MAX_COLS + 5];
        snprintf(prompt, sizeof(prompt), "$ %s_", input_buffer);
        SDL_Surface *surface = TTF_RenderText_Solid(font, prompt, textColor);
        if (surface) {
            SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_Rect dest = {10, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, texture, NULL, &dest);
            SDL_FreeSurface(surface);
            SDL_DestroyTexture(texture);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
