#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <mupdf/fitz.h>
#include <dirent.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm> 
#include <cmath>
#include <map>
#include <sys/stat.h> 

enum AppState { STATE_LIBRARY, STATE_READING };

struct Book {
    std::string filepath;
    SDL_Texture* cover_tex;
    SDL_Rect rect; 
    SDL_Rect src_rect;
};

// --- HÀM KIỂM TRA ĐỊNH DẠNG FILE ---
bool is_supported_file(const std::string& fname) {
    size_t pos = fname.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = fname.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".epub" || ext == ".pdf" || ext == ".cbz";
}

bool is_fixed_layout(const std::string& fname) {
    size_t pos = fname.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = fname.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".pdf" || ext == ".cbz";
}

// --- HÀM LƯU/ĐỌC TIẾN ĐỘ ---
int load_progress(const std::string& filepath) {
    size_t pos = filepath.find_last_of('/');
    std::string filename = (pos == std::string::npos) ? filepath : filepath.substr(pos + 1);
    std::string save_path = "sdmc:/lib/save/" + filename + ".save";
    
    int page = 0; float dummy; 
    FILE* f = fopen(save_path.c_str(), "r");
    if (f) { if (fscanf(f, "%d %f", &page, &dummy) < 1) page = 0; fclose(f); }
    return page;
}

void save_progress(const std::string& filepath, int page) {
    size_t pos = filepath.find_last_of('/');
    std::string filename = (pos == std::string::npos) ? filepath : filepath.substr(pos + 1);
    std::string save_path = "sdmc:/lib/save/" + filename + ".save";
    
    FILE* f = fopen(save_path.c_str(), "w");
    if (f) { fprintf(f, "%d", page); fclose(f); }
}

void load_global_settings(float& size, int& is_portrait, int& is_dark_mode, int& bg_index, int& sort_asc, std::string& last_read) {
    size = 18.0f; is_portrait = 0; is_dark_mode = 0; bg_index = 0; sort_asc = 1; last_read = "";
    FILE* f = fopen("sdmc:/lib/global_setting.dat", "r");
    if (f) {
        char path_buf[512] = {0};
        if (fscanf(f, "%f %d %d %d %d\n", &size, &is_portrait, &is_dark_mode, &bg_index, &sort_asc) >= 1) {
            if (fgets(path_buf, sizeof(path_buf), f)) {
                last_read = path_buf;
                while (!last_read.empty() && (last_read.back() == '\n' || last_read.back() == '\r')) {
                    last_read.pop_back();
                }
            }
        }
        fclose(f);
    }
    if (size < 10.0f || size > 60.0f) size = 18.0f;
    if (bg_index < 0 || bg_index > 2) bg_index = 0; 
}

void save_global_settings(float size, int is_portrait, int is_dark_mode, int bg_index, int sort_asc, const std::string& last_read) {
    FILE* f = fopen("sdmc:/lib/global_setting.dat", "w");
    if (f) {
        fprintf(f, "%f %d %d %d %d\n%s", size, is_portrait, is_dark_mode, bg_index, sort_asc, last_read.c_str());
        fclose(f);
    }
}

// --- THUẬT TOÁN RENDER & FALLBACK CHỐNG ĐEN MÀN HÌNH ---
void load_page(fz_context *ctx, fz_document *doc, int page_num, SDL_Renderer *renderer, SDL_Texture **page_texture, bool dark_mode, float target_w, float target_h, float requested_multiplier, float& out_actual_scale) {
    if (*page_texture) { SDL_DestroyTexture(*page_texture); *page_texture = NULL; }
    
    std::vector<float> multipliers;
    if (requested_multiplier > 1.0f) multipliers.push_back(requested_multiplier);
    multipliers.push_back(1.0f);
    multipliers.push_back(0.5f);
    
    for (float current_mult : multipliers) {
        bool success = false;
        fz_try(ctx) {
            fz_page *page = fz_load_page(ctx, doc, page_num);
            fz_rect bounds = fz_bound_page(ctx, page);
            fz_drop_page(ctx, page);

            float bw = std::max(bounds.x1 - bounds.x0, 100.0f);
            float bh = std::max(bounds.y1 - bounds.y0, 100.0f);

            float scale_x = (target_w * current_mult) / bw;
            float scale_y = (target_h * current_mult) / bh;
            float scale = std::min(scale_x, scale_y);
            
            // Giới hạn an toàn tuyệt đối cho VRAM Switch (4096px)
            float max_safe_scale_w = 4096.0f / bw;
            float max_safe_scale_h = 4096.0f / bh;
            scale = std::min({scale, max_safe_scale_w, max_safe_scale_h});

            fz_matrix transform = fz_scale(scale, scale);
            fz_pixmap *pix = fz_new_pixmap_from_page_number(ctx, doc, page_num, transform, fz_device_rgb(ctx), 0);
            
            int w = fz_pixmap_width(ctx, pix);
            int h = fz_pixmap_height(ctx, pix);
            
            if (w > 0 && h > 0) {
                unsigned char* samples = fz_pixmap_samples(ctx, pix);
                SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(samples, w, h, 24, w * 3, 0x0000FF, 0x00FF00, 0xFF0000, 0);
                if (surface) {
                    if (dark_mode) {
                        Uint8* p = (Uint8*)surface->pixels;
                        int total_bytes = w * h * 3;
                        for (int i = 0; i < total_bytes; i++) p[i] = 255 - p[i];
                    }
                    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); 
                    *page_texture = SDL_CreateTextureFromSurface(renderer, surface);
                    SDL_FreeSurface(surface);
                }
            }
            fz_drop_pixmap(ctx, pix);
            
            if (*page_texture != NULL) {
                success = true;
                out_actual_scale = scale; // Truyền chính xác tỷ lệ đã sử dụng ra ngoài
            }
        } fz_catch(ctx) { success = false; }
        
        if (success) break;
        else fz_empty_store(ctx); // Xả sạch rác RAM để thử độ nét thấp hơn
    }
}

void update_layout(fz_context* ctx, fz_document* doc, bool is_portrait, float font_size, int& current_page, int& total_pages, float exact_progress, SDL_Renderer* renderer, SDL_Texture** page_texture, bool dark_mode, float requested_multiplier, float& active_scale) {
    float layout_w = is_portrait ? 720.0f : 800.0f;
    float layout_h = is_portrait ? 1270.0f : 670.0f; 
    
    fz_layout_document(ctx, doc, layout_w, layout_h, font_size);
    total_pages = fz_count_pages(ctx, doc);
    
    current_page = (int)(exact_progress * (total_pages > 1 ? total_pages - 1 : 0) + 0.5f);
    if (current_page >= total_pages) current_page = total_pages - 1;
    if (current_page < 0) current_page = 0;
    
    load_page(ctx, doc, current_page, renderer, page_texture, dark_mode, layout_w, layout_h, requested_multiplier, active_scale);
}

int main(int argc, char* argv[]) {
    romfsInit();
    mkdir("sdmc:/lib/save", 0777);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG); 
    
    SDL_Window* window = SDL_CreateWindow("Red Reader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_Texture* render_target_vert = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 720, 1280);
    
    SDL_Texture* bg_textures[3] = {NULL, NULL, NULL};
    bg_textures[0] = IMG_LoadTexture(renderer, "romfs:/bg1.jpg");
    bg_textures[1] = IMG_LoadTexture(renderer, "romfs:/bg2.jpg");
    bg_textures[2] = IMG_LoadTexture(renderer, "romfs:/bg3.jpg");
    if (!bg_textures[0]) bg_textures[0] = IMG_LoadTexture(renderer, "romfs:/bg.jpg");

    SDL_Texture* app_icon_tex = IMG_LoadTexture(renderer, "romfs:/icon.png");
    if (!app_icon_tex) app_icon_tex = IMG_LoadTexture(renderer, "romfs:/icon.jpg");

    float global_font_size;
    int is_port_int, is_dark_int, current_bg_index, sort_asc_int;
    std::string last_read_filepath;
    
    load_global_settings(global_font_size, is_port_int, is_dark_int, current_bg_index, sort_asc_int, last_read_filepath);
    bool is_portrait = (is_port_int != 0);
    bool is_dark_mode = (is_dark_int != 0);
    bool sort_asc = (sort_asc_int != 0);

    fz_context *ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
    fz_register_document_handlers(ctx);
    fz_disable_icc(ctx); 

    std::vector<Book> library;

    int total_files = 0;
    DIR *dir = opendir("sdmc:/lib");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (is_supported_file(ent->d_name)) total_files++;
        }
        closedir(dir);
    }

    if (total_files > 0 && (dir = opendir("sdmc:/lib")) != NULL) {
        int index = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            std::string fname = ent->d_name;
            if (is_supported_file(fname)) {
                
                SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                SDL_RenderClear(renderer);
                if (bg_textures[current_bg_index]) {
                    SDL_Rect bg_rect = {0, 0, 1280, 720};
                    SDL_RenderCopy(renderer, bg_textures[current_bg_index], NULL, &bg_rect);
                }
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
                SDL_Rect overlay = {0, 0, 1280, 720};
                SDL_RenderFillRect(renderer, &overlay);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                if (app_icon_tex) {
                    SDL_Rect icon_dest = { 1280/2 - 100, 720/2 - 140, 200, 200 };
                    SDL_RenderCopy(renderer, app_icon_tex, NULL, &icon_dest);
                }

                float progress = (float)index / total_files;
                SDL_Rect bg_bar = { 1280/2 - 200, 720/2 + 100, 400, 8 };
                SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
                SDL_RenderFillRect(renderer, &bg_bar);
                
                SDL_Rect fg_bar = { 1280/2 - 200, 720/2 + 100, (int)(400 * progress), 8 };
                SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); 
                SDL_RenderFillRect(renderer, &fg_bar);
                SDL_RenderPresent(renderer); 

                Book b; 
                b.filepath = "sdmc:/lib/" + fname;
                b.cover_tex = NULL;
                b.rect = {0, 0, 0, 0};
                b.src_rect = {0, 0, 0, 0};
                
                fz_document *temp_doc = NULL;
                fz_try(ctx) {
                    temp_doc = fz_open_document(ctx, b.filepath.c_str());
                    fz_layout_document(ctx, temp_doc, 400.0f, 600.0f, 18.0f);
                    
                    fz_page *page = fz_load_page(ctx, temp_doc, 0);
                    fz_rect bounds = fz_bound_page(ctx, page);
                    fz_drop_page(ctx, page);
                    
                    float bw = bounds.x1 - bounds.x0;
                    float bh = bounds.y1 - bounds.y0;
                    if (bw <= 0) bw = 100.0f;
                    if (bh <= 0) bh = 100.0f;
                    
                    float scale_x = 360.0f / bw;
                    float scale_y = 520.0f / bh;
                    float scale = std::min(scale_x, scale_y);

                    fz_pixmap *pix = fz_new_pixmap_from_page_number(ctx, temp_doc, 0, fz_scale(scale, scale), fz_device_rgb(ctx), 0);
                    
                    int w = fz_pixmap_width(ctx, pix);
                    int h = fz_pixmap_height(ctx, pix);
                    unsigned char* samples = fz_pixmap_samples(ctx, pix);

                    int min_x = w, min_y = h, max_x = 0, max_y = 0;
                    for (int y = 0; y < h; ++y) {
                        for (int x = 0; x < w; ++x) {
                            int idx = (y * w + x) * 3;
                            if (samples[idx] < 240 || samples[idx+1] < 240 || samples[idx+2] < 240) {
                                if (x < min_x) min_x = x; 
                                if (x > max_x) max_x = x;
                                if (y < min_y) min_y = y; 
                                if (y > max_y) max_y = y;
                            }
                        }
                    }
                    b.src_rect = (min_x <= max_x && min_y <= max_y) ? SDL_Rect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1} : SDL_Rect{0, 0, w, h};

                    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(samples, w, h, 24, w * 3, 0x0000FF, 0x00FF00, 0xFF0000, 0);
                    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); 
                    b.cover_tex = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_FreeSurface(surf);
                    fz_drop_pixmap(ctx, pix);
                } fz_catch(ctx) {}
                
                if (temp_doc) fz_drop_document(ctx, temp_doc);
                fz_empty_store(ctx); 
                
                library.push_back(b);
                index++; 
            }
        }
        closedir(dir);

        std::sort(library.begin(), library.end(), [sort_asc](const Book& a, const Book& b) {
            std::string nameA = a.filepath.substr(10); 
            std::string nameB = b.filepath.substr(10);
            if (sort_asc) return nameA < nameB;
            return nameA > nameB;
        });

        // Tái tạo lưới thư viện 5 Cột tràn viền
        for (size_t i = 0; i < library.size(); i++) {
            int col = i % 5;
            int row = i / 5;
            library[i].rect = { 60 + col * 235, 60 + row * 310, 180, 260 };
        }

        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderClear(renderer);
        if (bg_textures[current_bg_index]) {
            SDL_Rect bg_rect = {0, 0, 1280, 720};
            SDL_RenderCopy(renderer, bg_textures[current_bg_index], NULL, &bg_rect);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
        SDL_Rect overlay = {0, 0, 1280, 720};
        SDL_RenderFillRect(renderer, &overlay);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        if (app_icon_tex) {
            SDL_Rect icon_dest = { 1280/2 - 100, 720/2 - 140, 200, 200 };
            SDL_RenderCopy(renderer, app_icon_tex, NULL, &icon_dest);
        }
        SDL_Rect bg_bar = { 1280/2 - 200, 720/2 + 100, 400, 8 };
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderFillRect(renderer, &bg_bar);
        SDL_Rect fg_bar = { 1280/2 - 200, 720/2 + 100, 400, 8 }; 
        SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); 
        SDL_RenderFillRect(renderer, &fg_bar);
        SDL_RenderPresent(renderer);
    }

    AppState state = STATE_LIBRARY;
    fz_document *active_doc = NULL;
    SDL_Texture* page_texture = NULL;
    std::string active_filepath = "";
    int total_pages = 0, current_page = 0;
    float exact_progress = 0.0f;
    float active_scale = 1.0f; // Thêm biến toàn cục để theo dõi Tỷ lệ nét đang thực sự dùng

    float scroll_y = 0.0f;
    float target_scroll_y = 0.0f;

    std::map<SDL_FingerID, SDL_FPoint> fingers;
    float zoom_level = 1.0f;
    float base_zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;
    float pan_start_x = 0.0f, pan_start_y = 0.0f;
    float pan_base_x = 0.0f, pan_base_y = 0.0f;
    float initial_pinch_dist = 0.0f;
    Uint32 last_tap_time = 0;

    int selected_book_index = 0; 
    bool running = true, is_touching = false;
    float start_touch_x = 0.0f, start_touch_y = 0.0f;
    float start_scroll_y = 0.0f;
    bool has_swiped = false;

    auto open_book = [&](const Book& b) {
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderClear(renderer);
        if (bg_textures[current_bg_index]) {
            SDL_Rect bg_rect = {0, 0, 1280, 720};
            SDL_RenderCopy(renderer, bg_textures[current_bg_index], NULL, &bg_rect);
        }
        
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210); 
        SDL_Rect overlay = {0, 0, 1280, 720};
        SDL_RenderFillRect(renderer, &overlay);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        if (b.cover_tex) {
            SDL_Rect center_rect = { 1280/2 - 135, 720/2 - 195, 270, 390 };
            for (int j = 0; j < 3; j++) {
                SDL_Rect outline = { center_rect.x - j - 1, center_rect.y - j - 1, center_rect.w + (j * 2) + 2, center_rect.h + (j * 2) + 2 };
                SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255); 
                SDL_RenderDrawRect(renderer, &outline);
            }
            SDL_RenderCopy(renderer, b.cover_tex, &b.src_rect, &center_rect);
        }
        
        SDL_Rect load_line = { 1280/2 - 135, 720/2 + 215, 270, 4 };
        SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255);
        SDL_RenderFillRect(renderer, &load_line);
        SDL_RenderPresent(renderer);

        if (active_doc) fz_drop_document(ctx, active_doc);
        active_filepath = b.filepath;
        last_read_filepath = b.filepath; 
        
        zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f;
        fingers.clear();
        
        fz_try(ctx) {
            active_doc = fz_open_document(ctx, b.filepath.c_str());
            current_page = load_progress(active_filepath);
            
            float target_w = is_portrait ? 720.0f : 800.0f;
            float target_h = is_portrait ? 1270.0f : 670.0f; 
            float requested_multiplier = is_fixed_layout(active_filepath) ? 2.0f : 1.0f;

            fz_layout_document(ctx, active_doc, target_w, target_h, global_font_size);
            total_pages = fz_count_pages(ctx, active_doc);
            
            if (current_page >= total_pages) current_page = total_pages - 1;
            if (current_page < 0) current_page = 0;
            exact_progress = total_pages > 1 ? (float)current_page / (total_pages - 1) : 0.0f;
            
            load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
            state = STATE_READING;
        } fz_catch(ctx) {}
    };

    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        
        if (kDown & HidNpadButton_Plus) running = false;
        
        if (state == STATE_LIBRARY) {
            if (kDown & HidNpadButton_L) {
                current_bg_index++;
                if (current_bg_index > 2) current_bg_index = 0; 
            }

            if (kDown & HidNpadButton_Y) {
                sort_asc = !sort_asc; 
                std::sort(library.begin(), library.end(), [sort_asc](const Book& a, const Book& b) {
                    std::string nameA = a.filepath.substr(10); 
                    std::string nameB = b.filepath.substr(10);
                    if (sort_asc) return nameA < nameB;
                    return nameA > nameB;
                });
                for (size_t i = 0; i < library.size(); i++) {
                    int col = i % 5; int row = i / 5;
                    library[i].rect.x = 60 + col * 235; library[i].rect.y = 60 + row * 310;
                }
                selected_book_index = 0; target_scroll_y = 0.0f; scroll_y = 0.0f;
                save_global_settings(global_font_size, is_portrait ? 1 : 0, is_dark_mode ? 1 : 0, current_bg_index, sort_asc ? 1 : 0, last_read_filepath);
            }

            if (!library.empty()) {
                if (kDown & (HidNpadButton_Left | HidNpadButton_StickLLeft)) if (selected_book_index > 0) selected_book_index--;
                if (kDown & (HidNpadButton_Right | HidNpadButton_StickLRight)) if (selected_book_index < (int)library.size() - 1) selected_book_index++;
                if (kDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) selected_book_index = std::max(0, selected_book_index - 5);
                if (kDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) selected_book_index = std::min((int)library.size() - 1, selected_book_index + 5);

                int selected_row = selected_book_index / 5;
                int book_top = 60 + selected_row * 310;
                int book_bottom = book_top + 260;

                if (book_bottom > target_scroll_y + 720 - 60) target_scroll_y = book_bottom - 720 + 60;
                else if (book_top < target_scroll_y + 60) target_scroll_y = book_top - 60;

                int max_row = library.empty() ? 0 : (library.size() - 1) / 5;
                int max_scroll = (60 + max_row * 310 + 260 + 60) - 720;
                if (max_scroll < 0) max_scroll = 0;
                if (target_scroll_y < 0) target_scroll_y = 0;
                if (target_scroll_y > max_scroll) target_scroll_y = max_scroll;

                if (kDown & HidNpadButton_A) open_book(library[selected_book_index]);
                if (kDown & HidNpadButton_X) {
                    if (!last_read_filepath.empty()) {
                        for (const auto& book : library) if (book.filepath == last_read_filepath) { open_book(book); break; }
                    }
                }
            }
        }
        else if (state == STATE_READING) {
            float target_w = is_portrait ? 720.0f : 800.0f;
            float target_h = is_portrait ? 1270.0f : 670.0f;
            float requested_multiplier = is_fixed_layout(active_filepath) ? 2.0f : 1.0f;

            if (kDown & HidNpadButton_B) {
                save_progress(active_filepath, current_page);
                save_global_settings(global_font_size, is_portrait ? 1 : 0, is_dark_mode ? 1 : 0, current_bg_index, sort_asc ? 1 : 0, last_read_filepath);
                if (page_texture) { SDL_DestroyTexture(page_texture); page_texture = NULL; }
                if (active_doc) { fz_drop_document(ctx, active_doc); active_doc = NULL; }
                fz_empty_store(ctx); 
                state = STATE_LIBRARY;
            }

            if (kDown & HidNpadButton_Minus) {
                is_dark_mode = !is_dark_mode;
                load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
            }

            if (kDown & HidNpadButton_R) {
                is_portrait = !is_portrait;
                zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; 
                update_layout(ctx, active_doc, is_portrait, global_font_size, current_page, total_pages, exact_progress, renderer, &page_texture, is_dark_mode, requested_multiplier, active_scale);
            }

            if (kDown & HidNpadButton_L) {
                if (current_page > 0) {
                    current_page = 0;
                    zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; 
                    exact_progress = 0.0f; 
                    load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
                }
            }

            if ((kDown & HidNpadButton_Right) || (kDown & HidNpadButton_A)) {
                if (current_page < total_pages - 1) {
                    current_page++;
                    zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; 
                    exact_progress = (float)current_page / (total_pages > 1 ? total_pages - 1 : 1);
                    load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
                }
            }
            if (kDown & HidNpadButton_Left) {
                if (current_page > 0) {
                    current_page--;
                    zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; 
                    exact_progress = (float)current_page / (total_pages > 1 ? total_pages - 1 : 1);
                    load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
                }
            }

            if ((kDown & HidNpadButton_X) || (kDown & HidNpadButton_Y)) {
                if (kDown & HidNpadButton_X) global_font_size += 2.0f; 
                if (kDown & HidNpadButton_Y) global_font_size -= 2.0f; 
                if (global_font_size > 60.0f) global_font_size = 60.0f;
                if (global_font_size < 10.0f) global_font_size = 10.0f;
                zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; 
                update_layout(ctx, active_doc, is_portrait, global_font_size, current_page, total_pages, exact_progress, renderer, &page_texture, is_dark_mode, requested_multiplier, active_scale);
            }
        }

        if (state == STATE_LIBRARY) {
            scroll_y += (target_scroll_y - scroll_y) * 0.15f; 
            if (std::abs(target_scroll_y - scroll_y) < 0.5f) scroll_y = target_scroll_y;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            float logical_x = 0.0f, logical_y = 0.0f;
            if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERUP || event.type == SDL_FINGERMOTION) {
                logical_x = event.tfinger.x * 1280.0f;
                logical_y = event.tfinger.y * 720.0f;
                if (state == STATE_READING && is_portrait) {
                    logical_x = (1.0f - event.tfinger.y) * 720.0f;
                    logical_y = event.tfinger.x * 1280.0f;
                }
            }

            if (event.type == SDL_FINGERDOWN) {
                fingers[event.tfinger.fingerId] = {logical_x, logical_y};

                bool can_zoom = is_fixed_layout(active_filepath);

                if (state == STATE_READING) {
                    if (fingers.size() == 1) {
                        Uint32 current_time = SDL_GetTicks();
                        if (current_time - last_tap_time < 300 && can_zoom) {
                            zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f;
                            last_tap_time = 0;
                        } else {
                            last_tap_time = current_time;
                            start_touch_x = logical_x; start_touch_y = logical_y;
                            pan_start_x = logical_x; pan_start_y = logical_y;
                            pan_base_x = pan_x; pan_base_y = pan_y;
                            has_swiped = false;
                            is_touching = true;
                        }
                    } else if (fingers.size() == 2 && can_zoom) {
                        auto it = fingers.begin();
                        SDL_FPoint f1 = it->second; it++;
                        SDL_FPoint f2 = it->second;
                        initial_pinch_dist = std::sqrt(std::pow(f1.x - f2.x, 2) + std::pow(f1.y - f2.y, 2));
                        base_zoom = zoom_level;
                        is_touching = false; 
                    }
                } else if (state == STATE_LIBRARY) {
                    if (fingers.size() == 1) {
                        start_touch_x = logical_x; start_touch_y = logical_y;
                        start_scroll_y = target_scroll_y; 
                        has_swiped = false;
                        is_touching = true;
                    }
                }
            }
            else if (event.type == SDL_FINGERMOTION) {
                if (fingers.find(event.tfinger.fingerId) != fingers.end()) {
                    fingers[event.tfinger.fingerId] = {logical_x, logical_y};
                }

                bool can_zoom = is_fixed_layout(active_filepath);

                if (state == STATE_READING) {
                    if (fingers.size() == 2 && can_zoom) {
                        auto it = fingers.begin();
                        SDL_FPoint f1 = it->second; it++;
                        SDL_FPoint f2 = it->second;
                        float new_dist = std::sqrt(std::pow(f1.x - f2.x, 2) + std::pow(f1.y - f2.y, 2));
                        if (initial_pinch_dist > 0) {
                            zoom_level = base_zoom * (new_dist / initial_pinch_dist);
                            if (zoom_level < 1.0f) { zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f; } 
                            if (zoom_level > 5.0f) zoom_level = 5.0f; 
                        }
                    } else if (fingers.size() == 1 && is_touching) {
                        float dx = logical_x - start_touch_x;
                        float dy = logical_y - start_touch_y;
                        if (std::abs(dx) > 15.0f || std::abs(dy) > 15.0f) has_swiped = true;
                        
                        if (zoom_level > 1.0f && can_zoom) {
                            pan_x = pan_base_x + (logical_x - pan_start_x);
                            pan_y = pan_base_y + (logical_y - pan_start_y);
                        }
                    }
                } else if (state == STATE_LIBRARY && is_touching) {
                    float dx = logical_x - start_touch_x;
                    float dy = logical_y - start_touch_y;
                    if (std::abs(dx) > 15.0f || std::abs(dy) > 15.0f) has_swiped = true;

                    if (has_swiped) {
                        target_scroll_y = start_scroll_y - dy;
                        int max_row = library.empty() ? 0 : (library.size() - 1) / 5;
                        int max_scroll = (60 + max_row * 310 + 260 + 60) - 720;
                        if (max_scroll < 0) max_scroll = 0;
                        if (target_scroll_y < 0) target_scroll_y = 0;
                        if (target_scroll_y > max_scroll) target_scroll_y = max_scroll;
                        scroll_y = target_scroll_y; 
                    }
                }
            }
            else if (event.type == SDL_FINGERUP) {
                fingers.erase(event.tfinger.fingerId);

                if (state == STATE_LIBRARY && is_touching && fingers.size() == 0) {
                    is_touching = false;
                    if (!has_swiped) {
                        for (size_t i = 0; i < library.size(); i++) {
                            auto& b = library[i];
                            if (logical_x >= b.rect.x && logical_x <= b.rect.x + b.rect.w && 
                                logical_y >= (b.rect.y - scroll_y) && logical_y <= (b.rect.y - scroll_y) + b.rect.h) {
                                selected_book_index = i; 
                                open_book(b);
                                break;
                            }
                        }
                    }
                } 
                else if (state == STATE_READING && is_touching && fingers.size() == 0) {
                    is_touching = false;
                    if (has_swiped && zoom_level <= 1.0f) {
                        float target_w = is_portrait ? 720.0f : 800.0f;
                        float target_h = is_portrait ? 1270.0f : 670.0f;
                        float requested_multiplier = is_fixed_layout(active_filepath) ? 2.0f : 1.0f;

                        float dx = logical_x - start_touch_x;
                        if (dx < -100.0f && current_page < total_pages - 1) { 
                            current_page++;
                            zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f;
                            exact_progress = (float)current_page / (total_pages > 1 ? total_pages - 1 : 1);
                            load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
                        } else if (dx > 100.0f && current_page > 0) { 
                            current_page--;
                            zoom_level = 1.0f; pan_x = 0.0f; pan_y = 0.0f;
                            exact_progress = (float)current_page / (total_pages > 1 ? total_pages - 1 : 1);
                            load_page(ctx, active_doc, current_page, renderer, &page_texture, is_dark_mode, target_w, target_h, requested_multiplier, active_scale);
                        }
                    }
                }
            }
        }

        SDL_SetRenderTarget(renderer, NULL); 
        
        if (state == STATE_LIBRARY) {
            if (bg_textures[current_bg_index]) {
                SDL_Rect bg_rect = {0, 0, 1280, 720};
                SDL_RenderCopy(renderer, bg_textures[current_bg_index], NULL, &bg_rect);
            } else {
                SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                SDL_RenderClear(renderer);
            }
            
            for (size_t i = 0; i < library.size(); i++) {
                auto& b = library[i];

                if (b.rect.y - scroll_y + b.rect.h < 0 || b.rect.y - scroll_y > 720) continue;

                SDL_Rect draw_rect = { b.rect.x, b.rect.y - (int)scroll_y, b.rect.w, b.rect.h };

                // KHÔI PHỤC MÀU VIỀN VÀNG TRUYỀN THỐNG
                if ((int)i == selected_book_index) {
                    SDL_SetRenderDrawColor(renderer, 255, 235, 50, 255); 
                } 
                else if (b.filepath == last_read_filepath) {
                    SDL_SetRenderDrawColor(renderer, 50, 220, 50, 255); 
                } 
                else {
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                }
                
                for (int j = 0; j < 3; j++) {
                    SDL_Rect outline = { draw_rect.x - j - 1, draw_rect.y - j - 1, draw_rect.w + (j * 2) + 2, draw_rect.h + (j * 2) + 2 };
                    SDL_RenderDrawRect(renderer, &outline);
                }
                if (b.cover_tex) SDL_RenderCopy(renderer, b.cover_tex, &b.src_rect, &draw_rect);
            }
        } 
        else if (state == STATE_READING) {
            int tex_w = 0, tex_h = 0;
            if (page_texture) {
                SDL_QueryTexture(page_texture, NULL, NULL, &tex_w, &tex_h);
            }

            // TÍNH TOÁN LẠI TỌA ĐỘ SCREEN DỰA VÀO ĐỘ NÉT THỰC TẾ
            if (active_scale <= 0.0001f) active_scale = 1.0f; // Chống lỗi chia cho 0

            if (is_portrait) {
                SDL_SetRenderTarget(renderer, render_target_vert);
                SDL_SetRenderDrawColor(renderer, is_dark_mode ? 20 : 255, is_dark_mode ? 20 : 255, is_dark_mode ? 20 : 255, 255);
                SDL_RenderClear(renderer);
                
                if (page_texture) {
                    // Cắt nghĩa: Kích thước thật của trang PDF = Cỡ ảnh VRAM / Tỷ lệ nhân đã dùng
                    float bw = tex_w / active_scale;
                    float bh = tex_h / active_scale;
                    
                    // Tính xem cần thu phóng bao nhiêu để vừa khít cái viền đọc sách
                    float fit_scale = std::min(720.0f / bw, 1270.0f / bh);
                    int base_w = (int)(bw * fit_scale);
                    int base_h = (int)(bh * fit_scale);
                    
                    int scaled_w = (int)(base_w * zoom_level);
                    int scaled_h = (int)(base_h * zoom_level);
                    
                    SDL_Rect dest = { (720 - scaled_w) / 2 + (int)pan_x, (1270 - scaled_h) / 2 + (int)pan_y, scaled_w, scaled_h };
                    SDL_RenderCopy(renderer, page_texture, NULL, &dest);
                }

                if (zoom_level <= 1.0f) {
                    SDL_Rect bar_bg = {0, 1276, 720, 4}; 
                    SDL_SetRenderDrawColor(renderer, is_dark_mode ? 60 : 200, is_dark_mode ? 60 : 200, is_dark_mode ? 60 : 200, 255);
                    SDL_RenderFillRect(renderer, &bar_bg);
                    
                    SDL_Rect bar_fg = {0, 1276, (int)(720 * exact_progress), 4}; 
                    SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); 
                    SDL_RenderFillRect(renderer, &bar_fg);
                }

                SDL_SetRenderTarget(renderer, NULL);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                SDL_Rect rot_dest = {280, -280, 720, 1280};
                SDL_RenderCopyEx(renderer, render_target_vert, NULL, &rot_dest, 270.0, NULL, SDL_FLIP_NONE);
            } 
            else {
                SDL_SetRenderDrawColor(renderer, is_dark_mode ? 20 : 255, is_dark_mode ? 20 : 255, is_dark_mode ? 20 : 255, 255);
                SDL_RenderClear(renderer);
                
                if (page_texture) {
                    float bw = tex_w / active_scale;
                    float bh = tex_h / active_scale;
                    
                    float fit_scale = std::min(800.0f / bw, 670.0f / bh);
                    int base_w = (int)(bw * fit_scale);
                    int base_h = (int)(bh * fit_scale);
                    
                    int scaled_w = (int)(base_w * zoom_level);
                    int scaled_h = (int)(base_h * zoom_level);
                    
                    SDL_Rect dest = { 240 + (800 - scaled_w) / 2 + (int)pan_x, 20 + (670 - scaled_h) / 2 + (int)pan_y, scaled_w, scaled_h };
                    SDL_RenderCopy(renderer, page_texture, NULL, &dest);
                }

                if (zoom_level <= 1.0f) {
                    SDL_Rect bar_bg = {240, 712, 800, 4}; 
                    SDL_SetRenderDrawColor(renderer, is_dark_mode ? 60 : 200, is_dark_mode ? 60 : 200, is_dark_mode ? 60 : 200, 255);
                    SDL_RenderFillRect(renderer, &bar_bg);
                    
                    SDL_Rect bar_fg = {240, 712, (int)(800 * exact_progress), 4}; 
                    SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); 
                    SDL_RenderFillRect(renderer, &bar_fg);
                }
            }
        }

        SDL_RenderPresent(renderer);
    }

    if (state == STATE_READING) save_progress(active_filepath, current_page);
    save_global_settings(global_font_size, is_portrait ? 1 : 0, is_dark_mode ? 1 : 0, current_bg_index, sort_asc ? 1 : 0, last_read_filepath);

    for (auto& b : library) if (b.cover_tex) SDL_DestroyTexture(b.cover_tex);
    if (page_texture) SDL_DestroyTexture(page_texture);
    if (render_target_vert) SDL_DestroyTexture(render_target_vert);
    if (app_icon_tex) SDL_DestroyTexture(app_icon_tex);
    
    for (int i = 0; i < 3; i++) {
        if (bg_textures[i]) SDL_DestroyTexture(bg_textures[i]);
    }
    if (active_doc) fz_drop_document(ctx, active_doc);
    
    fz_drop_context(ctx);
    IMG_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    romfsExit();

    return 0;
}