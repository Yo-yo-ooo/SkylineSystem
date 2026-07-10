#include <graphic/fb.h>

static const char cursor[16][17] = {
    "*...............",
    "**..............",
    "*O*.............",
    "*OO*............",
    "*OOO*...........",
    "*OOOO*..........",
    "*OOOOO*.........",
    "*OOOOOO*........",
    "*OOOOOOO*.......",
    "*OOOO*****......",
    "*OO*O*..........",
    "*O*.*O*.........",
    "**..*O*.........",
    "*....*O*........",
    ".....*O*........",
    "......*........."
};

void DrawMousePointer(int32_t mousex,int32_t mousey, FrameBuffer* framebuffer)
{
    // 经典 16x16 鼠标字符画
    

    // 将 BaseAddress 转为 32 位指针，因为我们要写入 32 位 ARGB 颜色
    uint32_t* fb_ptr = (uint32_t*)framebuffer->BaseAddress;
    
    // 获取屏幕宽高用于边界检查（根据你的结构体，可能是 Width 或 PixelsPerScanLine）
    int fb_width = framebuffer->Width;
    int fb_height = framebuffer->Height;

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int px = mousex + x;
            int py = mousey + y;

            // 1. 边界检查，防止鼠标移出屏幕导致内存越界
            if (py >= 0 && py < fb_height && px >= 0 && px < fb_width) {
                
                // 2. 根据字符决定颜色
                if (cursor[y][x] == '*') {
                    // 画黑色轮廓
                    fb_ptr[py * fb_width + px] = 0; 
                } 
                else if (cursor[y][x] == 'O') {
                    // 画白色内部
                    fb_ptr[py * fb_width + px] = 0x00FFFFFF; 
                }
                // 3. 如果是 '.'，什么都不做！
                // 因为主循环里每次画鼠标前都已经用 memcpy 恢复了纯净背景，
                // 这里不画 '.' 就意味着保留了底层的 UIBase 背景。
            }
        }
    }
}