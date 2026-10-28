/* VRAM: the PVR's 8 MB as gpu_pvr has it laid out (the host's layout,
 * modelled on the Dreamcast's: object lists, region array, vertex buffer,
 * frame buffers, then the texture heap), with what the engine named each
 * texture allocation. The scene's is read in-process, the running game's
 * through the shared file (host/embed.h). */
#include "editor.h"
#include "ui.h"
#include "embed.h"
#include "pvr_host.h"
#include <algorithm>
#include <string.h>

namespace {

struct Block {
    uint32_t offset, size;
    int kind, w, h;
    std::string tag, fmt;
};

struct Map {
    std::vector<Block> blocks;
    uint32_t total = 0, used = 0;
    bool game = false;
};

const char* kind_name[PVR_VRAM_KINDS] = {
    "Free", "Object lists", "Region array", "Vertex buffer", "Frame buffers", "Unused", "Textures",
};
const ImU32 kind_col[PVR_VRAM_KINDS] = {
    IM_COL32(44, 46, 52, 255),      /* free */
    IM_COL32(150, 90, 245, 255),    /* OPBs */
    IM_COL32(200, 120, 230, 255),   /* region array */
    IM_COL32(240, 132, 40, 255),    /* vertex buffer */
    IM_COL32(226, 82, 82, 255),     /* frame buffers */
    IM_COL32(88, 88, 96, 255),      /* unused */
    IM_COL32(66, 133, 244, 255),    /* textures */
};

void read_scene(Map& m) {
    m.game = false;
    static std::vector<pvr_host_vram_block> raw(512);
    int n = pvr_host_vram_map(raw.data(), (int)raw.size(), &m.total, &m.used);
    if (n > (int)raw.size()) {
        raw.resize(n + 64);
        n = pvr_host_vram_map(raw.data(), (int)raw.size(), &m.total, &m.used);
    }
    m.blocks.clear();
    for (int i = 0; i < n && i < (int)raw.size(); i++) {
        auto& b = raw[i];
        m.blocks.push_back({ b.offset, b.size, b.kind, b.w, b.h, b.tag, b.fmt });
    }
}

/* The game writes the map with vram_seq odd; a copy taken across a change is
 * taken again (and the last good one kept if it keeps changing) */
bool read_game(Map& m, const DMSEmbed* g) {
    static DMSEmbedVram copy[DMS_EMBED_VRAM_MAX];
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s = g->vram_seq;
        if (s & 1) continue;
        __sync_synchronize();
        uint32_t n = g->vram_count, used = g->vram_used, total = g->vram_total;
        if (n > DMS_EMBED_VRAM_MAX) n = DMS_EMBED_VRAM_MAX;
        memcpy(copy, (const void*)g->vram, n * sizeof(DMSEmbedVram));
        __sync_synchronize();
        if (g->vram_seq != s) continue;
        m.game = true;
        m.total = total;
        m.used = used;
        m.blocks.clear();
        for (uint32_t i = 0; i < n; i++) {
            auto& e = copy[i];
            e.tag[sizeof e.tag - 1] = e.fmt[sizeof e.fmt - 1] = 0;
            m.blocks.push_back({ e.offset, e.size, e.kind < PVR_VRAM_KINDS ? e.kind : PVR_VRAM_UNUSED,
                                 e.w, e.h, e.tag, e.fmt });
        }
        return true;
    }
    return false;
}

Map scene_map, game_map;
int source = 0;          /* 0 follow the game (when one runs), 1 scene, 2 game */

const Map& current() {
    const DMSEmbed* g = play_shared();
    read_scene(scene_map);
    if (g) read_game(game_map, g);
    bool game = g && source != 1;
    return game ? game_map : scene_map;
}

uint32_t heap_free(const Map& m, uint32_t* largest) {
    uint32_t f = 0, big = 0;
    for (auto& b : m.blocks)
        if (b.kind == PVR_VRAM_FREE) { f += b.size; big = std::max(big, b.size); }
    if (largest) *largest = big;
    return f;
}

std::string name_of(const Block& b) {
    if (!b.tag.empty()) return b.tag;
    return b.kind == PVR_VRAM_TEXTURE ? "(untagged pvr_mem_malloc)" : kind_name[b.kind];
}

void block_tooltip(const Block& b) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name_of(b).c_str());
    ImGui::TextColored(ImVec4(0.6f, 0.62f, 0.66f, 1), "%s  %.1f KB  at 0x%06x", kind_name[b.kind], b.size / 1024.0f, b.offset);
    if (b.w || !b.fmt.empty()) ImGui::Text("%dx%d  %s", b.w, b.h, b.fmt.c_str());
    ImGui::EndTooltip();
}

/* The colour of one block: textures alternate two shades so neighbours part */
ImU32 colour_of(const Block& b, int index) {
    if (b.kind != PVR_VRAM_TEXTURE) return kind_col[b.kind];
    if (b.tag.empty()) return ui::WARN;
    return index & 1 ? IM_COL32(52, 112, 214, 255) : IM_COL32(84, 152, 255, 255);
}

int hovered_block = -1, picked_block = -1;   /* by offset index into the map's blocks */

void draw_map(const Map& m) {
    static int row_kb = 512;
    const uint32_t row_bytes = (uint32_t)row_kb << 10;
    const uint32_t total = m.total ? m.total : 8u << 20;
    const int rows = (int)((total + row_bytes - 1) / row_bytes);
    const float label_w = ImGui::CalcTextSize("0x000000 ").x;
    const float row_h = 12, gap = 2;
    float width = ImGui::GetContentRegionAvail().x - label_w;
    if (width < 64) width = 64;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("map", ImVec2(label_w + width, rows * (row_h + gap)));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bpp = row_bytes / width;   /* bytes per pixel */

    for (int r = 0; r < rows; r++) {
        char a[16];
        snprintf(a, sizeof a, "0x%06x", r * row_bytes);
        dl->AddText(ImVec2(o.x, o.y + r * (row_h + gap) - 1), ui::TEXT_DIM, a);
    }
    float x0 = o.x + label_w;
    auto span = [&](uint32_t from, uint32_t to, ImU32 col, bool outline) {
        while (from < to) {
            uint32_t r = from / row_bytes, row_end = std::min(to, (r + 1) * row_bytes);
            float y = o.y + r * (row_h + gap);
            float a = x0 + (from - r * row_bytes) / bpp, b = x0 + (row_end - r * row_bytes) / bpp;
            if (b - a < 1) b = a + 1;
            if (outline) dl->AddRect(ImVec2(a - 1, y - 1), ImVec2(b + 1, y + row_h + 1), IM_COL32(255, 255, 255, 230), 0, 0, 1.5f);
            else dl->AddRectFilled(ImVec2(a, y), ImVec2(b, y + row_h), col);
            from = row_end;
        }
    };
    for (size_t i = 0; i < m.blocks.size(); i++) {
        auto& b = m.blocks[i];
        span(b.offset, b.offset + b.size, colour_of(b, (int)i), false);
    }

    int under = -1;
    if (hovered) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        int r = (int)((mp.y - o.y) / (row_h + gap));
        float fx = mp.x - x0;
        if (r >= 0 && r < rows && fx >= 0 && fx < width) {
            uint32_t at = r * row_bytes + (uint32_t)(fx * bpp);
            /* blocks are in address order */
            auto it = std::upper_bound(m.blocks.begin(), m.blocks.end(), at,
                                       [](uint32_t v, const Block& b) { return v < b.offset; });
            if (it != m.blocks.begin()) {
                --it;
                if (at < it->offset + it->size) under = (int)(it - m.blocks.begin());
            }
        }
        if (under >= 0) {
            block_tooltip(m.blocks[under]);
            if (ImGui::IsItemClicked()) picked_block = under;
        }
    }
    for (int i : { under, hovered_block, picked_block })
        if (i >= 0 && i < (int)m.blocks.size()) span(m.blocks[i].offset, m.blocks[i].offset + m.blocks[i].size, 0, true);

    ImGui::SetNextItemWidth(120);
    static const int choices[] = { 64, 128, 256, 512 };
    char cur[32];
    snprintf(cur, sizeof cur, "%d KB a row", row_kb);
    if (ImGui::BeginCombo("##rowkb", cur)) {
        for (int c : choices) {
            char l[32];
            snprintf(l, sizeof l, "%d KB a row", c);
            if (ImGui::Selectable(l, c == row_kb)) row_kb = c;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("1 px = %.2f KB.  The host's layout (gpu_pvr), modelled on the Dreamcast's; KOS's differs a little.", bpp / 1024.0f);
}

void legend(const Map& m) {
    uint32_t by_kind[PVR_VRAM_KINDS] = {};
    for (auto& b : m.blocks) by_kind[b.kind] += b.size;
    static const int order[] = { PVR_VRAM_TEXTURE, PVR_VRAM_FREE, PVR_VRAM_FRAME, PVR_VRAM_VERTEX,
                                 PVR_VRAM_OPB, PVR_VRAM_REGION, PVR_VRAM_UNUSED };
    bool first = true;
    for (int k : order) {
        if (!by_kind[k]) continue;
        if (!first) ImGui::SameLine(0, 14);
        first = false;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2), kind_col[k], 2);
        ImGui::Dummy(ImVec2(h - 2, h));
        ImGui::SameLine(0, 2);
        ImGui::Text("%s %.0f KB", kind_name[k], by_kind[k] / 1024.0f);
    }
}

void texture_table(const Map& m) {
    std::vector<int> rows;
    static char filter[64];
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##vf", ICON_SEARCH "  Filter", filter, sizeof filter);
    for (size_t i = 0; i < m.blocks.size(); i++) {
        auto& b = m.blocks[i];
        if (b.kind != PVR_VRAM_TEXTURE) continue;
        if (*filter && name_of(b).find(filter) == std::string::npos && b.fmt.find(filter) == std::string::npos) continue;
        rows.push_back((int)i);
    }
    ImGuiTableFlags f = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("tex", 5, f, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 200.0f)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("KB", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                            ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount) {
        int col = ss->Specs[0].ColumnIndex;
        bool desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            const Block &x = m.blocks[a], &y = m.blocks[b];
            int c = 0;
            switch (col) {
            case 0: c = name_of(x).compare(name_of(y)); break;
            case 1: c = x.w * x.h - y.w * y.h; break;
            case 2: c = x.fmt.compare(y.fmt); break;
            case 3: c = x.size < y.size ? -1 : x.size > y.size; break;
            default: c = x.offset < y.offset ? -1 : x.offset > y.offset; break;
            }
            return desc ? c > 0 : c < 0;
        });
    }
    hovered_block = -1;
    for (int i : rows) {
        auto& b = m.blocks[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        if (ImGui::Selectable(name_of(b).c_str(), picked_block == i, ImGuiSelectableFlags_SpanAllColumns))
            picked_block = picked_block == i ? -1 : i;
        if (ImGui::IsItemHovered()) { hovered_block = i; block_tooltip(b); }
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (b.w) ImGui::Text("%dx%d", b.w, b.h); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b.fmt.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", b.size / 1024.0f);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("0x%06x", b.offset);
    }
    ImGui::EndTable();
}

}

void vram_meter(float width) {
    const Map& m = current();
    char tip[256];
    uint32_t big, f = heap_free(m, &big);
    int tex = 0;
    for (auto& b : m.blocks) tex += b.kind == PVR_VRAM_TEXTURE;
    ui::meter(m.game ? "Game VRAM" : "Scene VRAM", m.used / 1048576.0f, (m.total ? m.total : 8u << 20) / 1048576.0f, "MB", width);
    if (ImGui::IsItemClicked()) ImGui::SetWindowFocus(WIN_VRAM);
    snprintf(tip, sizeof tip,
             "%s: frame buffers, TA buffers and %d textures.\nTexture heap: %.0f KB free (the biggest gap %.0f KB).\n"
             "Click for the VRAM tab.",
             m.game ? "The running game's VRAM" : "VRAM as the editor has this scene loaded", tex, f / 1024.0f, big / 1024.0f);
    ui::tooltip(tip);
}

void panel_vram() {
    if (!ImGui::Begin(WIN_VRAM)) { ImGui::End(); return; }
    const DMSEmbed* g = play_shared();
    const Map& m = current();

    ui::segment_begin();
    if (ui::segment_button("vscene", ICON_SCENE, "The scene, as the editor has it loaded", !m.game, true, false)) source = 1;
    ImGui::BeginDisabled(!g);
    if (ui::segment_button("vgame", ICON_GAMEPAD, g ? "The running game" : "The running game (play it first)", m.game, false, true)) source = 2;
    ImGui::EndDisabled();
    ui::segment_end();
    ImGui::SameLine(0, 12);
    uint32_t big, f = heap_free(m, &big), tex_bytes = 0;
    int tex = 0, untagged = 0;
    for (auto& b : m.blocks)
        if (b.kind == PVR_VRAM_TEXTURE) { tex++; tex_bytes += b.size; untagged += b.tag.empty(); }
    if (ui::font_bold) ImGui::PushFont(ui::font_bold, 0.0f);
    ImGui::Text("%s VRAM", m.game ? "Game" : "Scene");
    if (ui::font_bold) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::Text("%.2f / %.0f MB used   %d textures %.0f KB   heap free %.0f KB (biggest gap %.0f KB)",
                m.used / 1048576.0f, m.total / 1048576.0f, tex, tex_bytes / 1024.0f, f / 1024.0f, big / 1024.0f);
    if (untagged) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.94f, 0.67f, 0.24f, 1), "%d untagged", untagged);
        ui::tooltip("pvr_mem_malloc() with no DC_VRAM_TAG before it (dms/dc_vram_tag.h)");
    }
    if (m.game && m.blocks.size() >= DMS_EMBED_VRAM_MAX) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.94f, 0.67f, 0.24f, 1), "(first %d blocks)", DMS_EMBED_VRAM_MAX);
    }
    if (m.blocks.empty()) {
        ImGui::TextDisabled(m.game ? "Waiting for the game's map" : "gpu_pvr is not running");
        ImGui::End();
        return;
    }
    if (picked_block >= (int)m.blocks.size()) picked_block = -1;
    legend(m);
    /* Wide: the map and the table side by side; narrow: one over the other */
    float w = ImGui::GetContentRegionAvail().x;
    if (w > 900) {
        ImGui::BeginChild("vmap", ImVec2(w * 0.55f, 0));
        draw_map(m);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("vtab", ImVec2(0, 0));
        texture_table(m);
        ImGui::EndChild();
    } else {
        draw_map(m);
        ImGui::Spacing();
        texture_table(m);
    }
    ImGui::End();
}
