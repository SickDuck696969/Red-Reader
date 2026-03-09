#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <mupdf/fitz.h>
#include <dirent.h>
#include <stdio.h>
#include <vector>
#include <string>

enum AppState { STATE_LIBRARY, STATE_READING };

struct Book {
    std::string filepath;
    SDL_Texture* cover_tex;
    SDL_Rect rect; 
    SDL_Rect src_rect; // Khung dùng để lưu tọa độ phần ảnh đã bị cắt viền trắng
};

// --- HÀM LƯU VÀ ĐỌC TIẾN ĐỘ ---
int load_progress(const std::string& filepath) {
    int page = 0;
    float dummy; 
    std::string save_path = filepath + ".save";
    FILE* f = fopen(save_path.c_str(), "r");
    if (f) {
        if (fscanf(f, "%d %f", &page, &dummy) < 1) page = 0;
        fclose(f);
    }
    return page;
}

void save_progress(const std::string& filepath, int page) {
    std::string save_path = filepath + ".save";
    FILE* f = fopen(save_path.c_str(), "w");
    if (f) {
        fprintf(f, "%d", page);
        fclose(f);
    }
}

// --- HÀM LƯU VÀ ĐỌC CỠ CHỮ TOÀN CỤC ---
float load_global_font_size() {
    float size = 18.0f; 
    FILE* f = fopen("sdmc:/lib/global_setting.dat", "r");
    if (f) {
        fscanf(f, "%f", &size);
        fclose(f);
    }
    if (size < 10.0f || size > 60.0f) size = 18.0f;
    return size;
}

void save_global_font_size(float size) {
    FILE* f = fopen("sdmc:/lib/global_setting.dat", "w");
    if (f) {
        fprintf(f, "%f", size);
        fclose(f);
    }
}

// --- HÀM RENDER TRANG SÁCH ---
void load_page(fz_context *ctx, fz_document *doc, int page_num, SDL_Renderer *renderer, SDL_Texture **page_texture) {
    if (*page_texture) {
        SDL_DestroyTexture(*page_texture);
        *page_texture = NULL;
    }
    fz_try(ctx) {
        fz_matrix transform = fz_scale(1.0f, 1.0f);
        fz_pixmap *pix = fz_new_pixmap_from_page_number(ctx, doc, page_num, transform, fz_device_rgb(ctx), 0);
        int w = fz_pixmap_width(ctx, pix);
        int h = fz_pixmap_height(ctx, pix);
        unsigned char* samples = fz_pixmap_samples(ctx, pix);
        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(samples, w, h, 24, w * 3, 0x0000FF, 0x00FF00, 0xFF0000, 0);
        if (surface) {
            *page_texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
        }
        fz_drop_pixmap(ctx, pix);
    } fz_catch(ctx) {}
}

int main(int argc, char* argv[]) {
    romfsInit();
    
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    
    SDL_Window* window = SDL_CreateWindow("Switch E-Reader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_Texture* render_target_vert = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 720, 1280);
    SDL_Texture* bg_texture = IMG_LoadTexture(renderer, "sdmc:/lib/bg.jpg");

    fz_context *ctx = fz_new_context(NULL, NULL, 64 * 1024 * 1024);
    fz_register_document_handlers(ctx);

    std::vector<Book> library;

    // --- QUÉT THƯ MỤC LIB VÀ TỰ ĐỘNG CẮT VIỀN TRẮNG ---
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir("sdmc:/lib")) != NULL) {
        int index = 0;
        while ((ent = readdir(dir)) != NULL) {
            std::string fname = ent->d_name;
            if (fname.length() > 5 && fname.substr(fname.length() - 5) == ".epub") {
                Book b;
                b.filepath = "sdmc:/lib/" + fname;
                
                fz_document *temp_doc = NULL;
                fz_try(ctx) {
                    temp_doc = fz_open_document(ctx, b.filepath.c_str());
                    
                    fz_layout_document(ctx, temp_doc, 400.0f, 600.0f, 18.0f);
                    fz_matrix transform = fz_scale(0.4f, 0.4f);
                    fz_pixmap *pix = fz_new_pixmap_from_page_number(ctx, temp_doc, 0, transform, fz_device_rgb(ctx), 0);
                    
                    int w = fz_pixmap_width(ctx, pix);
                    int h = fz_pixmap_height(ctx, pix);
                    unsigned char* samples = fz_pixmap_samples(ctx, pix);

                    // THUẬT TOÁN AUTO-CROP: Quét để tìm tọa độ tấm ảnh lõi (bỏ qua viền trắng)
                    int min_x = w, min_y = h, max_x = 0, max_y = 0;
                    for (int y = 0; y < h; ++y) {
                        for (int x = 0; x < w; ++x) {
                            int idx = (y * w + x) * 3;
                            // Ngưỡng < 240 để loại trừ các màu trắng bệch hoặc trắng hơi xám do đổ bóng
                            if (samples[idx] < 240 || samples[idx+1] < 240 || samples[idx+2] < 240) {
                                if (x < min_x) min_x = x;
                                if (x > max_x) max_x = x;
                                if (y < min_y) min_y = y;
                                if (y > max_y) max_y = y;
                            }
                        }
                    }

                    // Nếu tìm thấy ảnh bên trong, lấy tọa độ mới. Nếu ảnh trắng bóc thì giữ nguyên.
                    if (min_x <= max_x && min_y <= max_y) {
                        b.src_rect = { min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
                    } else {
                        b.src_rect = { 0, 0, w, h };
                    }

                    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(samples, w, h, 24, w * 3, 0x0000FF, 0x00FF00, 0xFF0000, 0);
                    b.cover_tex = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_FreeSurface(surf);
                    fz_drop_pixmap(ctx, pix);
                } fz_catch(ctx) {}
                
                if (temp_doc) fz_drop_document(ctx, temp_doc);
                fz_empty_store(ctx); 
                
                int col = index % 3;
                int row = index / 3;
                b.rect = { 50 + col * 215, 60 + row * 280, 160, 240 }; 
                
                library.push_back(b);
                index++;
            }
        }
        closedir(dir);
    }

    AppState state = STATE_LIBRARY;
    fz_document *active_doc = NULL;
    SDL_Texture* page_texture = NULL;
    
    std::string active_filepath = "";
    int total_pages = 0;
    int current_page = 0;
    
    float global_font_size = load_global_font_size();

    bool running = true;
    bool is_touching = false;
    float start_touch_x = 0.0f;

    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        
        if (kDown & HidNpadButton_Plus) running = false;
        
        if (state == STATE_READING) {
            if (kDown & HidNpadButton_B) {
                save_progress(active_filepath, current_page);
                save_global_font_size(global_font_size); 
                
                if (page_texture) { SDL_DestroyTexture(page_texture); page_texture = NULL; }
                if (active_doc) { fz_drop_document(ctx, active_doc); active_doc = NULL; }
                fz_empty_store(ctx); 
                
                state = STATE_LIBRARY;
            }

            if ((kDown & HidNpadButton_A) || (kDown & HidNpadButton_Right)) {
                if (current_page < total_pages - 1) {
                    current_page++;
                    load_page(ctx, active_doc, current_page, renderer, &page_texture);
                }
            }

            if (kDown & HidNpadButton_Left) {
                if (current_page > 0) {
                    current_page--;
                    load_page(ctx, active_doc, current_page, renderer, &page_texture);
                }
            }

            if ((kDown & HidNpadButton_X) || (kDown & HidNpadButton_Y)) {
                if (kDown & HidNpadButton_X) global_font_size += 2.0f; 
                if (kDown & HidNpadButton_Y) global_font_size -= 2.0f; 
                
                if (global_font_size > 60.0f) global_font_size = 60.0f;
                if (global_font_size < 10.0f) global_font_size = 10.0f;

                float progress_ratio = total_pages > 0 ? (float)current_page / total_pages : 0.0f;
                fz_layout_document(ctx, active_doc, 720.0f, 1280.0f, global_font_size);
                
                total_pages = fz_count_pages(ctx, active_doc);
                current_page = (int)(progress_ratio * total_pages);
                if (current_page >= total_pages) current_page = total_pages - 1;
                
                load_page(ctx, active_doc, current_page, renderer, &page_texture);
            }
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            float logical_x = (1.0f - event.tfinger.y) * 720.0f;
            float logical_y = event.tfinger.x * 1280.0f;

            if (event.type == SDL_FINGERDOWN) {
                is_touching = true;
                start_touch_x = logical_x;

                if (state == STATE_LIBRARY) {
                    for (auto& b : library) {
                        if (logical_x >= b.rect.x && logical_x <= b.rect.x + b.rect.w && logical_y >= b.rect.y && logical_y <= b.rect.y + b.rect.h) {
                            if (active_doc) fz_drop_document(ctx, active_doc);
                            active_filepath = b.filepath;
                            
                            fz_try(ctx) {
                                active_doc = fz_open_document(ctx, b.filepath.c_str());
                                current_page = load_progress(active_filepath);
                                fz_layout_document(ctx, active_doc, 720.0f, 1280.0f, global_font_size);
                                
                                total_pages = fz_count_pages(ctx, active_doc);
                                if (current_page >= total_pages) current_page = total_pages - 1;
                                
                                load_page(ctx, active_doc, current_page, renderer, &page_texture);
                                state = STATE_READING;
                            } fz_catch(ctx) {}
                            break;
                        }
                    }
                }
            } 
            else if (event.type == SDL_FINGERUP && is_touching) {
                is_touching = false;
                
                if (state == STATE_READING) {
                    float dx = logical_x - start_touch_x;
                    
                    if (dx < -100.0f && current_page < total_pages - 1) { 
                        current_page++;
                        load_page(ctx, active_doc, current_page, renderer, &page_texture);
                    } else if (dx > 100.0f && current_page > 0) { 
                        current_page--;
                        load_page(ctx, active_doc, current_page, renderer, &page_texture);
                    }
                }
            }
        }

        SDL_SetRenderTarget(renderer, render_target_vert); 
        
        if (state == STATE_LIBRARY) {
            if (bg_texture) {
                SDL_Rect bg_rect = {0, 0, 720, 1280};
                SDL_RenderCopy(renderer, bg_texture, NULL, &bg_rect);
            } else {
                SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
                SDL_RenderClear(renderer);
            }
            
            for (auto& b : library) {
                // Vẽ Outline viền đen rỗng (Hollow rectangle)
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                for (int i = 0; i < 3; i++) {
                    SDL_Rect outline = { b.rect.x - i - 1, b.rect.y - i - 1, b.rect.w + (i * 2) + 2, b.rect.h + (i * 2) + 2 };
                    SDL_RenderDrawRect(renderer, &outline);
                }

                // Vẽ ảnh bìa sách bằng b.src_rect (chỉ lấy phần lõi không có viền trắng) ép vừa khít vào b.rect
                if (b.cover_tex) SDL_RenderCopy(renderer, b.cover_tex, &b.src_rect, &b.rect);
            }
        } 
        else if (state == STATE_READING) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderClear(renderer);
            if (page_texture) {
                SDL_Rect dest = {0, 0, 720, 1280};
                SDL_RenderCopy(renderer, page_texture, NULL, &dest);
            }
        }

        SDL_SetRenderTarget(renderer, NULL);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        SDL_Rect rot_dest = {280, -280, 720, 1280};
        SDL_RenderCopyEx(renderer, render_target_vert, NULL, &rot_dest, 270.0, NULL, SDL_FLIP_NONE);

        SDL_RenderPresent(renderer);
    }

    if (state == STATE_READING) {
        save_progress(active_filepath, current_page);
        save_global_font_size(global_font_size);
    }

    for (auto& b : library) {
        if (b.cover_tex) SDL_DestroyTexture(b.cover_tex);
    }
    if (page_texture) SDL_DestroyTexture(page_texture);
    if (render_target_vert) SDL_DestroyTexture(render_target_vert);
    if (bg_texture) SDL_DestroyTexture(bg_texture);
    if (active_doc) fz_drop_document(ctx, active_doc);
    
    fz_drop_context(ctx);
    IMG_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    romfsExit();

    return 0;
}