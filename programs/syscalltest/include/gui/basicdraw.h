#pragma once
#ifndef _BASIC_DRAW_H_
#define _BASIC_DRAW_H_

#include <stdint.h>
#include <stddef.h>
#include <gui/fb.h>

class BasicDraw
{
private:
    FrameBuffer *FrameBuf;
public:
    BasicDraw(FrameBuffer *fb) : FrameBuf(fb) {}

    /*
    * @brief 用指定颜色清屏
    * @param Color 32位ARGB值
    */
    void ClearScreen(uint32_t Color);
    /**
     * @brief 绘制单个像素
     * @param X 像素 X 坐标
     * @param Y 像素 Y 坐标
     * @param Color 32位 ARGB 颜色值
     * @note 已修复边界判断：使用 >= 而非 >
     */
    void PutPixel(uint64_t X,uint64_t Y,uint32_t Color);

    /**
     * @brief 绘制水平直线
     * @param X 起始 X 坐标
     * @param Y 行 Y 坐标
     * @param Width 线宽（像素数）
     * @param Color 32位 ARGB 颜色值
     */
    void DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color); 
    /**
     * @brief 绘制垂直直线
     * @param X 列 X 坐标
     * @param Y 起始 Y 坐标
     * @param Height 线高（像素数）
     * @param Color 32位 ARGB 颜色值
     */
    void DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color); 
    /**
     * @brief 绘制矩形（支持填充和描边）
     * @param X 左上角 X 坐标
     * @param Y 左上角 Y 坐标
     * @param W 矩形宽度
     * @param H 矩形高度
     * @param Color 32位 ARGB 颜色值
     * @param Fill true=填充，false=描边
     */
    void DrawRect(uint64_t X, uint64_t Y, uint64_t Width, uint64_t Height, uint32_t Color, bool Fill);
    /**
     * @brief 使用 Bresenham 算法绘制任意直线
     * @param x0 起点 X 坐标
     * @param y0 起点 Y 坐标
     * @param x1 终点 X 坐标
     * @param y1 终点 Y 坐标
     * @param Color 32位 ARGB 颜色值
     */
    void DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color); // 万能画线
    /**
     * @brief 绘制经典风格窗口（带标题栏）
     * @param X 左上角 X 坐标
     * @param Y 左上角 Y 坐标
     * @param W 窗口宽度
     * @param H 窗口高度
     * @param Title 窗口标题（暂未使用）
     */
    void DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title);
    /**
     * @brief 使用 Bresenham 圆算法绘制圆（8方向对称优化）
     * @param xc 圆心 X 坐标
     * @param yc 圆心 Y 坐标
     * @param r 圆半径
     * @param Color 32位 ARGB 颜色值
     */
    void DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color);
    /**
     * @brief 绘制圆角矩形（支持填充和描边）
     * @param X 左上角 X 坐标
     * @param Y 左上角 Y 坐标
     * @param W 矩形宽度
     * @param H 矩形高度
     * @param R 圆角半径
     * @param Color 32位 ARGB 颜色值
     * @param Fill true=填充，false=描边
     * @note 使用 Bresenham 圆算法绘制四个圆角
     */
    void DrawRoundedRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint64_t R, uint32_t Color, bool Fill);
    /**
     * @brief 绘制桌面
     */
    void DrawProportionalUI();

    void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

    ~BasicDraw(){}
};

#endif