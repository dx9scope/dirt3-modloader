#pragma once

#include <cstdint>
#include <vector>

#include "dirt3_modloader_api.h"

namespace overlay {

// Screen-space vertex: pixel coordinates, color as 0xAARRGGBB
struct Vertex {
    float x, y;
    uint32_t color;
};

// Immediate-mode triangle list, rebuilt every frame
class DrawList {
public:
    std::vector<Vertex> verts;

    void Clear() { verts.clear(); }

    void Tri(float x0, float y0, uint32_t c0,
             float x1, float y1, uint32_t c1,
             float x2, float y2, uint32_t c2) {
        verts.push_back({ x0, y0, c0 });
        verts.push_back({ x1, y1, c1 });
        verts.push_back({ x2, y2, c2 });
    }

    // Quad given in winding order p0 -> p1 -> p2 -> p3
    void Quad(float x0, float y0, uint32_t c0,
              float x1, float y1, uint32_t c1,
              float x2, float y2, uint32_t c2,
              float x3, float y3, uint32_t c3) {
        Tri(x0, y0, c0, x1, y1, c1, x2, y2, c2);
        Tri(x0, y0, c0, x2, y2, c2, x3, y3, c3);
    }

    void Rect(float x, float y, float w, float h, uint32_t c) {
        Quad(x, y, c, x + w, y, c, x + w, y + h, c, x, y + h, c);
    }

    // Horizontal gradient (left -> right)
    void RectH(float x, float y, float w, float h, uint32_t cl, uint32_t cr) {
        Quad(x, y, cl, x + w, y, cr, x + w, y + h, cr, x, y + h, cl);
    }

    // Vertical gradient (top -> bottom)
    void RectV(float x, float y, float w, float h, uint32_t ct, uint32_t cb) {
        Quad(x, y, ct, x + w, y, ct, x + w, y + h, cb, x, y + h, cb);
    }

    void Frame(float x, float y, float w, float h, float t, uint32_t c) {
        Rect(x, y, w, t, c);
        Rect(x, y + h - t, w, t, c);
        Rect(x, y + t, t, h - 2 * t, c);
        Rect(x + w - t, y + t, t, h - 2 * t, c);
    }
};

// Called on the render thread right before the game presents a frame.
// Fill `list` with triangles; leave it empty to draw nothing.
using DrawCallback = void (*)(DrawList& list, float screenW, float screenH, float dt);

// Installs D3D9 / D3D11 present hooks asynchronously (waits for the game window).
bool Start(const ModloaderAPI* api, DrawCallback callback);

// "DX9", "DX11" or "none" (for logging)
const char* ActiveBackendName();

} // namespace overlay
