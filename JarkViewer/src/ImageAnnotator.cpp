#define NOMINMAX 1 // OpenCV 的 std::min/std::max 与 windows.h 的宏冲突

#include "ImageAnnotator.h"

#include "TextDrawer.h"
#include "jarkUtils.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace jark {
namespace {

    // 与 TextDrawer 的等宽字体模型一致：ASCII 半个字宽，非 ASCII 一个字宽
    int textUnits(const std::string& text) {
        int units = 0;
        int maxUnits = 0;
        for (const char ch : text) {
            if (ch == '\n') {
                maxUnits = std::max(maxUnits, units);
                units = 0;
            }
            else if ((static_cast<uint8_t>(ch) & 0xC0) != 0x80) {
                units += (static_cast<uint8_t>(ch) & 0x80) ? 2 : 1;
            }
        }
        return std::max(maxUnits, units);
    }

    int textLineCount(const std::string& text) {
        return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    }

    cv::Scalar opaqueColor(uint32_t color) {
        return cv::Scalar(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 255);
    }

    cv::Point clampPoint(const cv::Point& point, const cv::Mat& canvas) {
        return {
            std::clamp(point.x, -canvas.cols, canvas.cols * 2),
            std::clamp(point.y, -canvas.rows, canvas.rows * 2)
        };
    }

    // 两点框定矩形：向左上拖动时也要得到正的宽高
    cv::Rect rectFromPoints(const cv::Point& from, const cv::Point& to) {
        return { std::min(from.x, to.x), std::min(from.y, to.y),
            std::abs(to.x - from.x), std::abs(to.y - from.y) };
    }

    // 箭头：主线 + 实心三角箭头
    void drawArrow(cv::Mat& canvas, const cv::Point& from, const cv::Point& to, const cv::Scalar& color, int width) {
        const double dx = static_cast<double>(to.x - from.x);
        const double dy = static_cast<double>(to.y - from.y);
        const double length = std::hypot(dx, dy);
        if (length < 1.0)
            return;

        const double headLength = std::clamp(length * 0.25, width * 2.5, width * 8.0);
        const double headWidth = headLength * 0.7;

        const double unitX = dx / length;
        const double unitY = dy / length;
        const double baseX = to.x - unitX * headLength;
        const double baseY = to.y - unitY * headLength;

        const cv::Point shaftEnd(static_cast<int>(std::lround(baseX)), static_cast<int>(std::lround(baseY)));
        cv::line(canvas, from, shaftEnd, color, width, cv::LINE_AA);

        const double perpX = -unitY * headWidth / 2.0;
        const double perpY = unitX * headWidth / 2.0;
        const cv::Point head[3] = {
            to,
            { static_cast<int>(std::lround(baseX + perpX)), static_cast<int>(std::lround(baseY + perpY)) },
            { static_cast<int>(std::lround(baseX - perpX)), static_cast<int>(std::lround(baseY - perpY)) },
        };

        const cv::Point* headPtr = head;
        int headCount = 3;
        cv::fillPoly(canvas, &headPtr, &headCount, 1, color, cv::LINE_AA);
    }

    // 马赛克：对矩形区域做块状降采样再放大（读取的必须是尚未马赛克的像素）
    void drawMosaic(cv::Mat& canvas, const cv::Rect& rect, int block) {
        const cv::Rect area = rect & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (area.width < 2 || area.height < 2)
            return;

        block = std::clamp(block, 2, std::max(2, std::min(area.width, area.height)));
        const int smallWidth = std::max(1, area.width / block);
        const int smallHeight = std::max(1, area.height / block);

        // 注意：变量名不能叫 small —— rpcndr.h 里有 "#define small char"
        cv::Mat region = canvas(area);
        cv::Mat blocks;
        cv::resize(region, blocks, { smallWidth, smallHeight }, 0, 0, cv::INTER_AREA);
        cv::resize(blocks, region, { area.width, area.height }, 0, 0, cv::INTER_NEAREST);
    }

} // namespace

bool isAnnoCanvas(const cv::Mat& canvas) {
    return !canvas.empty() && (canvas.type() == CV_8UC4 || canvas.type() == CV_8UC3);
}

cv::Rect annotationBounds(const Annotation& anno) {
    if (anno.tool == AnnoTool::Text) {
        const int lineHeight = static_cast<int>(anno.style.fontSize * 1.1f);
        const int width = lineHeight * textUnits(anno.text) / 2;
        const cv::Point origin = anno.points.empty() ? cv::Point() : anno.points.front();
        return { origin.x, origin.y, std::max(width, 1), std::max(lineHeight * textLineCount(anno.text), 1) };
    }

    if (anno.points.empty())
        return {};

    cv::Rect box = cv::boundingRect(anno.points);
    const int margin = anno.tool == AnnoTool::Arrow ? anno.style.width * 4 : anno.style.width;
    return { box.x - margin, box.y - margin, box.width + margin * 2, box.height + margin * 2 };
}

void drawAnnotation(cv::Mat& canvas, const Annotation& anno, TextDrawer& textDrawer) {
    if (!isAnnoCanvas(canvas) || anno.points.empty())
        return;

    const cv::Scalar color = opaqueColor(anno.style.color);
    const int width = std::max(1, anno.style.width);

    switch (anno.tool) {
    case AnnoTool::Rect: {
        if (anno.points.size() < 2)
            break;
        const cv::Rect rect = rectFromPoints(anno.points.front(), anno.points.back()) &
            cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (rect.width > 0 && rect.height > 0)
            cv::rectangle(canvas, rect, color, anno.style.filled ? -1 : width, cv::LINE_AA);
    } break;

    case AnnoTool::Ellipse: {
        if (anno.points.size() < 2)
            break;
        const cv::Rect rect = rectFromPoints(anno.points.front(), anno.points.back());
        if (rect.width > 0 && rect.height > 0) {
            const cv::Point center(rect.x + rect.width / 2, rect.y + rect.height / 2);
            cv::ellipse(canvas, center, { rect.width / 2, rect.height / 2 }, 0, 0, 360,
                color, anno.style.filled ? -1 : width, cv::LINE_AA);
        }
    } break;

    case AnnoTool::Arrow: {
        if (anno.points.size() < 2)
            break;
        drawArrow(canvas, anno.points.front(), anno.points.back(), color, width);
    } break;

    case AnnoTool::Line: {
        if (anno.points.size() < 2)
            break;
        cv::line(canvas, anno.points.front(), anno.points.back(), color, width, cv::LINE_AA);
    } break;

    case AnnoTool::Pen: {
        if (anno.points.size() == 1) {
            cv::circle(canvas, anno.points.front(), width / 2, color, -1, cv::LINE_AA);
            break;
        }
        const cv::Point* points = anno.points.data();
        const int count = static_cast<int>(anno.points.size());
        cv::polylines(canvas, &points, &count, 1, false, color, width, cv::LINE_AA);
    } break;

    case AnnoTool::Mosaic: {
        // 用首尾两点框定区域（与矩形工具一致的手感）
        const cv::Point from = anno.points.front();
        const cv::Point to = anno.points.size() > 1 ? anno.points.back() : from;
        drawMosaic(canvas, rectFromPoints(from, to), anno.mosaicBlock);
    } break;

    case AnnoTool::Text: {
        if (anno.text.empty())
            break;
        textDrawer.setSize(std::max(8, anno.style.fontSize));
        textDrawer.putText(canvas, anno.points.front().x, anno.points.front().y,
            anno.text.c_str(), intUnion(anno.style.color), false);
    } break;

    case AnnoTool::Crop:
        break; // 裁剪不画东西，由编辑器应用
    }
}

// —— AnnotatorDocument ——

cv::Mat toAnnoCanvas(const cv::Mat& image) {
    if (image.empty())
        return {};

    cv::Mat canvas;
    if (image.type() == CV_8UC4) {
        canvas = image.clone();
    }
    else if (image.type() == CV_8UC3) {
        cv::cvtColor(image, canvas, cv::COLOR_BGR2BGRA);
    }
    else if (image.type() == CV_8UC1) {
        cv::cvtColor(image, canvas, cv::COLOR_GRAY2BGRA);
    }
    else {
        // 16 位图（如部分 RAW 解码结果）直接 convertTo 会整体截断成白色，需要按位深缩放
        const int channels = std::min(image.channels(), 4);
        const double scale = image.depth() == CV_16U ? 1.0 / 257.0 : 1.0;
        cv::Mat converted;
        image.convertTo(converted, CV_MAKETYPE(CV_8U, channels), scale);
        canvas = toAnnoCanvas(converted);
    }
    return canvas;
}

bool encodeAnnotatedImage(const cv::Mat& image, const std::wstring& extension,
    std::vector<uint8_t>& output, int quality) {
    if (image.empty())
        return false;

    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (!ext.empty() && ext.front() != L'.')
        ext.insert(ext.begin(), L'.');

    const bool isJpeg = ext == L".jpg" || ext == L".jpeg";
    cv::Mat source = image;

    // JPEG 不支持透明：透明区域铺白底
    if (isJpeg && image.channels() == 4) {
        cv::Mat white(image.rows, image.cols, CV_8UC4, cv::Scalar(255, 255, 255, 255));
        cv::Mat alpha;
        cv::extractChannel(image, alpha, 3);
        cv::Mat mask;
        cv::threshold(alpha, mask, 250, 255, cv::THRESH_BINARY);
        image.copyTo(white, mask);
        source = white;
    }

    const std::vector<int> params = isJpeg
        ? std::vector<int>{ cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100) }
        : std::vector<int>{};

    return cv::imencode(jarkUtils::wstringToUtf8(ext), source, output, params);
}

AnnotatorDocument::AnnotatorDocument(cv::Mat base) {
    reset(std::move(base));
}

void AnnotatorDocument::reset(cv::Mat base) {
    base_ = toAnnoCanvas(base);
    annotations_.clear();
    undoStack_.clear();
    redoStack_.clear();
    drawing_ = false;
    active_ = {};
    rebuildCommittedImage();
}

void AnnotatorDocument::rebuildCommittedImage() {
    committedImage_ = base_.clone();

    for (const auto& anno : annotations_)
        drawAnnotation(committedImage_, anno, textDrawer_);
}

cv::Rect AnnotatorDocument::clampRect(const cv::Rect& rect) const {
    return rect & cv::Rect(0, 0, base_.cols, base_.rows);
}

void AnnotatorDocument::begin(AnnoTool tool, const AnnoStyle& style, const cv::Point& point) {
    active_ = {};
    active_.tool = tool;
    active_.style = style;
    active_.points.push_back(clampPoint(point, base_));
    drawing_ = true;
}

void AnnotatorDocument::update(const cv::Point& point) {
    if (!drawing_)
        return;

    const cv::Point clamped = clampPoint(point, base_);

    if (active_.tool == AnnoTool::Pen) {
        // 轨迹点太密没有意义，且会拖慢绘制
        if (active_.points.empty() || cv::norm(clamped - active_.points.back()) >= 2.0)
            active_.points.push_back(clamped);
        return;
    }

    if (active_.points.size() < 2)
        active_.points.push_back(clamped);
    else
        active_.points.back() = clamped;
}

void AnnotatorDocument::setText(const std::string& text) {
    if (drawing_ && active_.tool == AnnoTool::Text)
        active_.text = text;
}

void AnnotatorDocument::commit() {
    if (!drawing_)
        return;

    drawing_ = false;

    bool meaningful = !active_.empty();
    if (active_.tool == AnnoTool::Text)
        meaningful = !active_.text.empty();
    else if (active_.tool != AnnoTool::Pen)
        meaningful = meaningful && active_.points.size() >= 2 &&
        active_.points.front() != active_.points.back();

    if (meaningful) {
        undoStack_.push_back(annotations_);
        redoStack_.clear();
        annotations_.push_back(active_);
        rebuildCommittedImage();
    }

    active_ = {};
}

void AnnotatorDocument::cancel() {
    drawing_ = false;
    active_ = {};
}

void AnnotatorDocument::undo() {
    if (undoStack_.empty())
        return;

    redoStack_.push_back(annotations_);
    annotations_ = undoStack_.back();
    undoStack_.pop_back();
    rebuildCommittedImage();
}

void AnnotatorDocument::redo() {
    if (redoStack_.empty())
        return;

    undoStack_.push_back(annotations_);
    annotations_ = redoStack_.back();
    redoStack_.pop_back();
    rebuildCommittedImage();
}

cv::Mat AnnotatorDocument::flatten() const {
    if (!drawing_)
        return committedImage_;

    cv::Mat result = committedImage_.clone();
    drawAnnotation(result, active_, textDrawer_);
    return result;
}

void AnnotatorDocument::applyEdit(cv::Mat newBase) {
    reset(std::move(newBase));
}

bool AnnotatorDocument::cropTo(const cv::Rect& rect) {
    const cv::Rect area = clampRect(rect);
    if (area.width < 2 || area.height < 2)
        return false;

    reset(flatten()(area).clone());
    return true;
}

} // namespace jark
