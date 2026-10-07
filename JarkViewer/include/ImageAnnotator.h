#pragma once

// 图像标注：在图像上叠加矩形/椭圆/箭头/直线/画笔/马赛克/文字，支持撤销重做。
//
// 这里只有纯逻辑（形状模型 + 绘制 + 撤销栈），不依赖窗口：
// 编辑器窗口（EditorWindow.h）负责交互，命令行 --probe --annotate 可直接验证渲染结果。

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "TextRenderer.h"

namespace jark {

    enum class AnnoTool {
        Rect = 0,     // 矩形
        Ellipse = 1,  // 椭圆
        Arrow = 2,    // 箭头
        Line = 3,     // 直线
        Pen = 4,      // 自由画笔
        Mosaic = 5,   // 马赛克
        Text = 6,     // 文字
        Crop = 7,     // 裁剪（不产生标注，用于框选后裁掉其它区域）
    };

    struct AnnoStyle {
        uint32_t color = 0xFFFF3B30; // 0xAARRGGBB
        int width = 6;               // 线宽（图像像素）
        int fontSize = 40;           // 文字字号（图像像素）
        bool filled = false;         // 矩形/椭圆是否填充
    };

    struct Annotation {
        AnnoTool tool = AnnoTool::Rect;
        AnnoStyle style;
        std::vector<cv::Point> points; // 画笔为轨迹点，其余为首尾两点
        std::string text;              // 文字内容（UTF-8）
        int mosaicBlock = 12;          // 马赛克块大小（图像像素）

        bool empty() const { return points.empty() && text.empty(); }
    };

    // 可绘制的画布：CV_8UC3 或 CV_8UC4
    bool isAnnoCanvas(const cv::Mat& canvas);

    // 形状在画布上的包围盒（含线宽/字号影响的外扩），可能超出画布
    cv::Rect annotationBounds(const Annotation& anno);

    // 把单个标注画到画布上（马赛克会直接修改该区域像素）。
    // textDrawer 只用于文字标注，调用方持有以便复用字体缓存。
    void drawAnnotation(cv::Mat& canvas, const Annotation& anno, TextRenderer& textDrawer);

    // 标注文档：底图 + 已提交标注 + 进行中的标注 + 撤销/重做栈。
    // 底图始终是 CV_8UC4（BGRA），便于保存与复制。
    class AnnotatorDocument {
    public:
        AnnotatorDocument() = default;
        explicit AnnotatorDocument(cv::Mat base);

        void reset(cv::Mat base);

        const cv::Mat& base() const { return base_; }
        int width() const { return base_.cols; }
        int height() const { return base_.rows; }

        // —— 绘制交互 ——
        void begin(AnnoTool tool, const AnnoStyle& style, const cv::Point& point);
        void update(const cv::Point& point);            // 画笔追加轨迹，其余更新终点
        void setText(const std::string& text);          // 文字标注实时预览
        void commit();                                  // 提交到撤销栈（空标注自动丢弃）
        void cancel();                                  // 丢弃进行中的标注
        bool drawing() const { return drawing_; }
        const Annotation& active() const { return active_; }

        // —— 撤销 / 重做 ——
        bool canUndo() const { return !undoStack_.empty(); }
        bool canRedo() const { return !redoStack_.empty(); }
        void undo();
        void redo();
        int undoDepth() const { return static_cast<int>(undoStack_.size()); }

        // 已提交标注的渲染结果（不含进行中的标注）
        const cv::Mat& committedImage() const { return committedImage_; }

        // 底图 + 全部标注（含进行中的）
        cv::Mat flatten() const;

        // 用当前画面替换底图并清空标注与历史（裁剪/旋转/调色等一次性编辑）
        void applyEdit(cv::Mat newBase);

        // 裁剪：保留矩形区域（会先合成已有标注），并重置历史
        bool cropTo(const cv::Rect& rect);

        const std::vector<Annotation>& annotations() const { return annotations_; }

    private:
        void rebuildCommittedImage();
        cv::Rect clampRect(const cv::Rect& rect) const;

        mutable TextRenderer textDrawer_; // 仅用于文字标注，复用字体缓存
        cv::Mat base_;                 // CV_8UC4
        cv::Mat committedImage_;       // base_ + annotations_
        std::vector<Annotation> annotations_;
        std::vector<std::vector<Annotation>> undoStack_;
        std::vector<std::vector<Annotation>> redoStack_;

        Annotation active_;
        bool drawing_ = false;
    };

    // 把任意图像转成标注画布用的 BGRA（缺失 alpha 视为不透明）
    cv::Mat toAnnoCanvas(const cv::Mat& image);

    // 编码为 png/jpg 字节流（jpg 时自动铺白底），用于保存或复制到剪贴板
    bool encodeAnnotatedImage(const cv::Mat& image, const std::wstring& extension,
        std::vector<uint8_t>& output, int quality = 95);

} // namespace jark
