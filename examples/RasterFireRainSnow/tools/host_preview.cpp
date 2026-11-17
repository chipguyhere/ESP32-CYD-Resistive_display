// host_preview.cpp - Build the scene renderer on a computer and write frames
// as PPM images, to check the look without flashing.
//
//   c++ -O2 -std=c++17 -DSCENE_HOST -I.. host_preview.cpp ../scene.cpp -o /tmp/preview
//   (FLASH=1 in the environment: a time within 3 s of a lightning flash
//   moves to the flash)
//   /tmp/preview <outdir> <panel width: 240 or 320> <seconds> [<seconds> ...]
//
// Steps at 30 fps from the start and writes <outdir>/frame_<ms>.ppm at each
// time given, assembled from the chunks exactly as the panel would get them.

#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <time.h>

float scene_host_flash(void);

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s outdir panelwidth seconds...\n", argv[0]); return 1; }
    const int panelW = atoi(argv[2]);
    std::vector<double> at;
    for (int i = 3; i < argc; i++) at.push_back(atof(argv[i]));
    std::sort(at.begin(), at.end());
    scene_init(panelW, false);
    const int W = scene_width(), H = scene_height();
    std::vector<uint16_t> native((size_t)W * H);   // panel rows of H
    const float dt = 1.0f / 30;
    double t = 0, renderS = 0;
    int frames = 0;
    size_t next = 0;
    while (next < at.size()) {
        scene_step(dt);
        t += dt;
        clock_t c0 = clock();
        for (int c = 0; c < scene_chunks(); c++) scene_render_chunk(c, &native[(size_t)c * SCENE_CHUNK * H], c & 1);
        renderS += (double)(clock() - c0) / CLOCKS_PER_SEC;
        frames++;
        if (getenv("FLASH") && scene_host_flash() > 0.8f && next < at.size() && t + 1e-6 >= at[next] - 3) at[next] = t;
        if (t + 1e-6 < at[next]) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%05d.ppm", argv[1], (int)(at[next] * 1000 + 0.5));
        FILE *f = fopen(path, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint16_t v = native[(size_t)x * H + (H - 1 - y)];
                v = (uint16_t)((v >> 8) | (v << 8));
                unsigned char px[3] = { (unsigned char)((v >> 8) & 0xF8), (unsigned char)((v >> 3) & 0xFC),
                                        (unsigned char)((v << 3) & 0xF8) };
                fwrite(px, 1, 3, f);
            }
        fclose(f);
        printf("%s  phase %s  brightness %.2f\n", path, scene_phase_name(), scene_brightness());
        next++;
    }
    printf("host render: %.2f ms a frame\n", renderS * 1000 / frames);
    return 0;
}
