#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <dirent.h>
#include <cstring>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#define WIDTH 800
#define HEIGHT 600

// raw process info read from the os
typedef struct ProcessEntry {
    int pid;
    std::string name;
    double cpu_percent;
    uint64_t memory_kb;
    uint64_t disk_read;
    uint64_t disk_write;
    uint64_t cpu_time;  // used to calculate cpu % between refreshes
} ProcessEntry;

// textures and screen positions for one row on screen
typedef struct GProcess {
    SDL_Texture *icon;
    SDL_Texture *name;
    SDL_Texture *stats;
    SDL_Rect icon_pos;
    SDL_Rect name_pos;
    SDL_Rect stats_pos;
} GProcess;

// everything the app needs while it is running
typedef struct AppData {
    TTF_Font *font;
    SDL_Texture *system_image;   // shared folder icon
    SDL_Texture *process_image;  // shared file icon
    std::vector<ProcessEntry*> entries;
    std::vector<GProcess*> graphic_entries;  // one gprocess per visible row
    int sort_mode;       // 0=name  1=cpu  2=memory
    int selected_index;
    Uint32 last_refresh;
    std::vector<int> prev_pids;          // saved from last refresh for cpu math
    std::vector<uint64_t> prev_cpu_time;
    bool has_cpu_sample;  // cpu % is 0 until we have two samples
} AppData;

// compare function uses this because std::sort can't see data_ptr
static int g_sort_mode = 0;

void initialize(SDL_Renderer *renderer, AppData *data_ptr);
void handleEvent(SDL_Event *event, SDL_Renderer *renderer, AppData *data_ptr);
void render(SDL_Renderer *renderer, AppData *data_ptr);
void populateProcesses(SDL_Renderer *renderer, AppData *data_ptr);
void listProcesses(AppData *data_ptr);
void clearGProcesses(std::vector<GProcess*>& graphic_entries);
bool compareProcessEntries(const ProcessEntry *a, const ProcessEntry *b);
bool pointInRect(int x, int y, SDL_Rect& rect);
void scrollList(AppData *data_ptr, int amount);
void quit(AppData *data_ptr);

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // start sdl (same setup as file browser)
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();

    SDL_Renderer *renderer;
    SDL_Window *window;
    SDL_CreateWindowAndRenderer(WIDTH, HEIGHT, 0, &window, &renderer);

    AppData data;
    data.sort_mode = 1;  // start sorted by cpu
    data.selected_index = -1;
    data.last_refresh = 0;
    data.has_cpu_sample = false;
    initialize(renderer, &data);

    SDL_Event event;
    do
    {
        // update process list every 500 ms
        if (SDL_GetTicks() - data.last_refresh >= 500)
        {
            populateProcesses(renderer, &data);
            data.last_refresh = SDL_GetTicks();
        }

        render(renderer, &data);
        // wait up to 500 ms for a key press or mouse click
        SDL_WaitEventTimeout(&event, 500);
        if (event.type != 0)
        {
            handleEvent(&event, renderer, &data);
        }
    } while (event.type != SDL_QUIT);

    // free memory and shut down sdl
    quit(&data);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    return 0;
}

void initialize(SDL_Renderer *renderer, AppData *data_ptr)
{
    // load font and png icons from resrc folder
    data_ptr->font = TTF_OpenFont("resrc/fonts/OpenSans-Regular.ttf", 14);

    SDL_Surface *sys_surf = IMG_Load("resrc/images/directory_icon.png");
    data_ptr->system_image = SDL_CreateTextureFromSurface(renderer, sys_surf);
    SDL_FreeSurface(sys_surf);

    SDL_Surface *proc_surf = IMG_Load("resrc/images/file_icon.png");
    data_ptr->process_image = SDL_CreateTextureFromSurface(renderer, proc_surf);
    SDL_FreeSurface(proc_surf);

    populateProcesses(renderer, data_ptr);
    data_ptr->last_refresh = SDL_GetTicks();
}

void handleEvent(SDL_Event *event, SDL_Renderer *renderer, AppData *data_ptr)
{
    if (event->type == SDL_KEYDOWN)
    {
        // n/c/m keys change how the list is sorted
        if (event->key.keysym.sym == SDLK_n)
        {
            data_ptr->sort_mode = 0;
            populateProcesses(renderer, data_ptr);
        }
        else if (event->key.keysym.sym == SDLK_c)
        {
            data_ptr->sort_mode = 1;
            populateProcesses(renderer, data_ptr);
        }
        else if (event->key.keysym.sym == SDLK_m)
        {
            data_ptr->sort_mode = 2;
            populateProcesses(renderer, data_ptr);
        }
    }
    else if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == SDL_BUTTON_LEFT)
    {
        int x = event->button.x;
        int y = event->button.y;

        // check if click landed on a process row
        for (int i = 0; i < data_ptr->graphic_entries.size(); i++)
        {
            if (pointInRect(x, y, data_ptr->graphic_entries[i]->icon_pos) ||
                pointInRect(x, y, data_ptr->graphic_entries[i]->name_pos) ||
                pointInRect(x, y, data_ptr->graphic_entries[i]->stats_pos))
            {
                data_ptr->selected_index = i;
                ProcessEntry *e = data_ptr->entries[i];
                // print details in terminal (like opening a file in file browser)
                std::cout << "PID: " << e->pid << "  " << e->name
                    << "  CPU: " << e->cpu_percent << "%"
                    << "  Mem: " << e->memory_kb << " KB"
                    << "  Disk R: " << e->disk_read << " W: " << e->disk_write
                    << std::endl;
            }
        }
    }
    else if (event->type == SDL_MOUSEWHEEL)
    {
        // move every row up or down (same as file browser)
        scrollList(data_ptr, 3 * event->wheel.y);
    }
}

void scrollList(AppData *data_ptr, int amount)
{
    for (int i = 0; i < data_ptr->graphic_entries.size(); i++)
    {
        data_ptr->graphic_entries[i]->icon_pos.y += amount;
        data_ptr->graphic_entries[i]->name_pos.y += amount;
        data_ptr->graphic_entries[i]->stats_pos.y += amount;
    }
}

void render(SDL_Renderer *renderer, AppData *data_ptr)
{
    // clear screen to light gray
    SDL_SetRenderDrawColor(renderer, 235, 235, 235, 255);
    SDL_RenderClear(renderer);

    SDL_Color color = { 0, 0, 0, 255 };
    // title at top
    SDL_Surface *header_surf = TTF_RenderText_Solid(data_ptr->font,
        "Task Manager (N=name C=cpu M=memory)", color);
    SDL_Texture *header_tex = SDL_CreateTextureFromSurface(renderer, header_surf);
    SDL_FreeSurface(header_surf);
    SDL_Rect header_pos = { 10, 10, 0, 0 };
    SDL_QueryTexture(header_tex, NULL, NULL, &header_pos.w, &header_pos.h);
    SDL_RenderCopy(renderer, header_tex, NULL, &header_pos);
    SDL_DestroyTexture(header_tex);

    // color key for memory bars (stacked so text does not overlap)
    int legend_y = header_pos.y + header_pos.h + 8;
    SDL_SetRenderDrawColor(renderer, 80, 180, 80, 255);
    SDL_Rect g_box = { 10, legend_y, 12, 12 };
    SDL_RenderFillRect(renderer, &g_box);
    SDL_Surface *leg1 = TTF_RenderText_Solid(data_ptr->font,
        "GREEN = low memory (under 50 MB)", color);
    SDL_Texture *t1 = SDL_CreateTextureFromSurface(renderer, leg1);
    SDL_FreeSurface(leg1);
    SDL_Rect p1 = { 28, legend_y, 0, 0 };
    SDL_QueryTexture(t1, NULL, NULL, &p1.w, &p1.h);
    SDL_RenderCopy(renderer, t1, NULL, &p1);
    SDL_DestroyTexture(t1);

    legend_y += 18;
    SDL_SetRenderDrawColor(renderer, 220, 180, 60, 255);
    SDL_Rect y_box = { 10, legend_y, 12, 12 };
    SDL_RenderFillRect(renderer, &y_box);
    SDL_Surface *leg2 = TTF_RenderText_Solid(data_ptr->font,
        "YELLOW = medium memory (50-200 MB)", color);
    SDL_Texture *t2 = SDL_CreateTextureFromSurface(renderer, leg2);
    SDL_FreeSurface(leg2);
    SDL_Rect p2 = { 28, legend_y, 0, 0 };
    SDL_QueryTexture(t2, NULL, NULL, &p2.w, &p2.h);
    SDL_RenderCopy(renderer, t2, NULL, &p2);
    SDL_DestroyTexture(t2);

    legend_y += 18;
    SDL_SetRenderDrawColor(renderer, 200, 70, 70, 255);
    SDL_Rect r_box = { 10, legend_y, 12, 12 };
    SDL_RenderFillRect(renderer, &r_box);
    SDL_Surface *leg3 = TTF_RenderText_Solid(data_ptr->font,
        "RED = high memory (over 200 MB)", color);
    SDL_Texture *t3 = SDL_CreateTextureFromSurface(renderer, leg3);
    SDL_FreeSurface(leg3);
    SDL_Rect p3 = { 28, legend_y, 0, 0 };
    SDL_QueryTexture(t3, NULL, NULL, &p3.w, &p3.h);
    SDL_RenderCopy(renderer, t3, NULL, &p3);
    SDL_DestroyTexture(t3);

    // biggest process sets the scale for bar width
    uint64_t max_mem = 1;
    for (int i = 0; i < data_ptr->entries.size(); i++)
    {
        if (data_ptr->entries[i]->memory_kb > max_mem)
        {
            max_mem = data_ptr->entries[i]->memory_kb;
        }
    }

    for (int i = 0; i < data_ptr->graphic_entries.size(); i++)
    {
        // teal rectangle shows which row you clicked
        if (i == data_ptr->selected_index)
        {
            SDL_SetRenderDrawColor(renderer, 0, 128, 128, 255);
            SDL_Rect highlight = { 5, data_ptr->graphic_entries[i]->icon_pos.y - 2,
                WIDTH - 10, 20 };
            SDL_RenderFillRect(renderer, &highlight);
        }

        // draw icon, name, and stats text for this row
        SDL_RenderCopy(renderer, data_ptr->graphic_entries[i]->icon, NULL,
            &(data_ptr->graphic_entries[i]->icon_pos));
        SDL_RenderCopy(renderer, data_ptr->graphic_entries[i]->name, NULL,
            &(data_ptr->graphic_entries[i]->name_pos));
        SDL_RenderCopy(renderer, data_ptr->graphic_entries[i]->stats, NULL,
            &(data_ptr->graphic_entries[i]->stats_pos));

        ProcessEntry *e = data_ptr->entries[i];
        int row_y = data_ptr->graphic_entries[i]->icon_pos.y + 6;
        int bar_max_w = 90;
        int bar_h = 8;
        int bar_x = WIDTH - bar_max_w - 15;

        // gray background bar, colored fill shows memory amount
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_Rect bar_bg = { bar_x, row_y, bar_max_w, bar_h };
        SDL_RenderFillRect(renderer, &bar_bg);

        int bar_w = (int)((e->memory_kb * bar_max_w) / max_mem);
        if (e->memory_kb > 0 && bar_w < 3)
        {
            bar_w = 3;
        }

        // color based on how much memory (kb)
        if (e->memory_kb < 50000)
        {
            SDL_SetRenderDrawColor(renderer, 80, 180, 80, 255);
        }
        else if (e->memory_kb < 200000)
        {
            SDL_SetRenderDrawColor(renderer, 220, 180, 60, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 200, 70, 70, 255);
        }

        SDL_Rect bar_fill = { bar_x, row_y, bar_w, bar_h };
        SDL_RenderFillRect(renderer, &bar_fill);
    }

    // show the frame on screen
    SDL_RenderPresent(renderer);
}

// like populateDirectory in the file browser
void populateProcesses(SDL_Renderer *renderer, AppData *data_ptr)
{
    clearGProcesses(data_ptr->graphic_entries);

    // delete old process data before reading again
    for (int i = 0; i < data_ptr->entries.size(); i++)
    {
        delete data_ptr->entries[i];
    }
    data_ptr->entries.clear();

    listProcesses(data_ptr);

    // turn each process into textures we can draw
    SDL_Color color = { 0, 0, 0, 255 };
    char stats_str[128];
    for (int i = 0; i < data_ptr->entries.size(); i++)
    {
        GProcess *g_entry = new GProcess();

        char name_str[64];
        snprintf(name_str, 64, "%s (%d)", data_ptr->entries[i]->name.c_str(),
            data_ptr->entries[i]->pid);
        SDL_Surface *txt_surf = TTF_RenderText_Solid(data_ptr->font, name_str, color);
        g_entry->name = SDL_CreateTextureFromSurface(renderer, txt_surf);
        SDL_FreeSurface(txt_surf);

        snprintf(stats_str, 128, "CPU:%.1f%%  %lu KB  R:%lu W:%lu",
            data_ptr->entries[i]->cpu_percent,
            (unsigned long)data_ptr->entries[i]->memory_kb,
            (unsigned long)data_ptr->entries[i]->disk_read,
            (unsigned long)data_ptr->entries[i]->disk_write);
        SDL_Surface *stats_surf = TTF_RenderText_Solid(data_ptr->font, stats_str, color);
        g_entry->stats = SDL_CreateTextureFromSurface(renderer, stats_surf);
        SDL_FreeSurface(stats_surf);

        // low pid often means system process, use folder icon vs file icon
        if (data_ptr->entries[i]->pid < 200)
        {
            g_entry->icon = data_ptr->system_image;
        }
        else
        {
            g_entry->icon = data_ptr->process_image;
        }

        // set where each texture goes on screen (below the legend)
        g_entry->icon_pos.w = 18;
        g_entry->icon_pos.h = 18;
        g_entry->icon_pos.x = 10;
        g_entry->icon_pos.y = 95 + (20 * i);

        SDL_QueryTexture(g_entry->name, NULL, NULL, &(g_entry->name_pos.w), &(g_entry->name_pos.h));
        g_entry->name_pos.x = 34;
        g_entry->name_pos.y = 95 + (20 * i);

        SDL_QueryTexture(g_entry->stats, NULL, NULL, &(g_entry->stats_pos.w), &(g_entry->stats_pos.h));
        g_entry->stats_pos.x = 260;
        g_entry->stats_pos.y = 95 + (20 * i);

        data_ptr->graphic_entries.push_back(g_entry);
    }
}

// read running processes from /proc (linux/ubuntu)
void listProcesses(AppData *data_ptr)
{
    char path[64];
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir)
    {
        return;
    }

    struct dirent *de;
    while ((de = readdir(proc_dir)) != NULL)
    {
        if (de->d_name[0] < '1' || de->d_name[0] > '9')
        {
            continue;
        }

        int pid = atoi(de->d_name);
        ProcessEntry *entry = new ProcessEntry();
        entry->pid = pid;
        entry->cpu_percent = 0;
        entry->memory_kb = 0;
        entry->disk_read = 0;
        entry->disk_write = 0;
        entry->cpu_time = 0;

        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE *comm_fp = fopen(path, "r");
        if (comm_fp)
        {
            char buf[64];
            if (fgets(buf, sizeof(buf), comm_fp))
            {
                entry->name = buf;
                if (!entry->name.empty() && entry->name.back() == '\n')
                {
                    entry->name.pop_back();
                }
            }
            fclose(comm_fp);
        }
        else
        {
            entry->name = "unknown";
        }

        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *stat_fp = fopen(path, "r");
        if (stat_fp)
        {
            unsigned long utime = 0, stime = 0;
            fscanf(stat_fp,
                "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu",
                &utime, &stime);
            entry->cpu_time = utime + stime;
            fclose(stat_fp);
        }

        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *status_fp = fopen(path, "r");
        if (status_fp)
        {
            char line[256];
            while (fgets(line, sizeof(line), status_fp))
            {
                if (strncmp(line, "VmRSS:", 6) == 0)
                {
                    sscanf(line, "VmRSS: %lu", &entry->memory_kb);
                    break;
                }
            }
            fclose(status_fp);
        }

        snprintf(path, sizeof(path), "/proc/%d/io", pid);
        FILE *io_fp = fopen(path, "r");
        if (io_fp)
        {
            char line[256];
            while (fgets(line, sizeof(line), io_fp))
            {
                if (strncmp(line, "read_bytes:", 11) == 0)
                {
                    sscanf(line, "read_bytes: %lu", &entry->disk_read);
                }
                else if (strncmp(line, "write_bytes:", 12) == 0)
                {
                    sscanf(line, "write_bytes: %lu", &entry->disk_write);
                }
            }
            fclose(io_fp);
        }

        data_ptr->entries.push_back(entry);
    }
    closedir(proc_dir);

    // compare cpu time now vs last refresh to get cpu %
    uint64_t total_delta = 0;
    if (data_ptr->has_cpu_sample)
    {
        for (int i = 0; i < data_ptr->entries.size(); i++)
        {
            for (int j = 0; j < data_ptr->prev_pids.size(); j++)
            {
                if (data_ptr->prev_pids[j] == data_ptr->entries[i]->pid)
                {
                    if (data_ptr->entries[i]->cpu_time >= data_ptr->prev_cpu_time[j])
                    {
                        total_delta += data_ptr->entries[i]->cpu_time - data_ptr->prev_cpu_time[j];
                    }
                    break;
                }
            }
        }
    }

    if (data_ptr->has_cpu_sample && total_delta > 0)
    {
        for (int i = 0; i < data_ptr->entries.size(); i++)
        {
            for (int j = 0; j < data_ptr->prev_pids.size(); j++)
            {
                if (data_ptr->prev_pids[j] == data_ptr->entries[i]->pid)
                {
                    if (data_ptr->entries[i]->cpu_time >= data_ptr->prev_cpu_time[j])
                    {
                        uint64_t proc_delta = data_ptr->entries[i]->cpu_time - data_ptr->prev_cpu_time[j];
                        // this process used proc_delta out of all cpu time since last refresh
                        data_ptr->entries[i]->cpu_percent =
                            100.0 * proc_delta / total_delta;
                    }
                    break;
                }
            }
        }
    }

    g_sort_mode = data_ptr->sort_mode;
    std::sort(data_ptr->entries.begin(), data_ptr->entries.end(), compareProcessEntries);

    // only keep top 80 so the screen does not get overloaded
    if (data_ptr->entries.size() > 80)
    {
        for (int i = 80; i < data_ptr->entries.size(); i++)
        {
            delete data_ptr->entries[i];
        }
        data_ptr->entries.resize(80);
    }

    // save for next refresh
    data_ptr->prev_pids.clear();
    data_ptr->prev_cpu_time.clear();
    for (int i = 0; i < data_ptr->entries.size(); i++)
    {
        data_ptr->prev_pids.push_back(data_ptr->entries[i]->pid);
        data_ptr->prev_cpu_time.push_back(data_ptr->entries[i]->cpu_time);
    }

    data_ptr->has_cpu_sample = true;
}

// used by std::sort - higher cpu/memory sorts to the top
bool compareProcessEntries(const ProcessEntry *a, const ProcessEntry *b)
{
    if (g_sort_mode == 1)
    {
        return a->cpu_percent > b->cpu_percent;
    }
    else if (g_sort_mode == 2)
    {
        return a->memory_kb > b->memory_kb;
    }
    return a->name < b->name;
}

// like clearGFiles in file browser - destroy row textures before rebuild
void clearGProcesses(std::vector<GProcess*>& graphic_entries)
{
    for (int i = 0; i < graphic_entries.size(); i++)
    {
        SDL_DestroyTexture(graphic_entries[i]->name);
        SDL_DestroyTexture(graphic_entries[i]->stats);
        delete graphic_entries[i];
    }
    graphic_entries.clear();
}

// check if mouse click is inside a rectangle
bool pointInRect(int x, int y, SDL_Rect& rect)
{
    if (x > rect.x && x < rect.x + rect.w &&
        y > rect.y && y < rect.y + rect.h)
    {
        return true;
    }
    return false;
}

void quit(AppData *data_ptr)
{
    // free everything we allocated
    clearGProcesses(data_ptr->graphic_entries);
    for (int i = 0; i < data_ptr->entries.size(); i++)
    {
        delete data_ptr->entries[i];
    }
    data_ptr->entries.clear();
    SDL_DestroyTexture(data_ptr->system_image);
    SDL_DestroyTexture(data_ptr->process_image);
    TTF_CloseFont(data_ptr->font);
}
