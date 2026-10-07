#pragma once

#include <cstdint>
#include <string>

const char* const getUIString(const uint32_t stringidx = 0);

// 宽字符文案：按值返回，自带缓冲区。
// 旧实现把所有文案转进同一个 thread_local 缓冲再返回它的指针，
// 于是 MessageBoxW(h, getUIStringW(a), getUIStringW(b), ...) 这种一条表达式里取两条文案的写法，
// 后一次转换会把前一次的指针内容冲掉（标题变成乱码），所以改成持有副本的值类型。
// 它能隐式转成 const wchar_t*，沿用原有调用点；需要长期保存请用 str() 取 std::wstring。
class UIStringWide {
public:
    UIStringWide() = default;
    explicit UIStringWide(std::wstring text) : text_(std::move(text)) {}

    const wchar_t* c_str() const { return text_.c_str(); }
    operator const wchar_t*() const { return text_.c_str(); }
    const std::wstring& str() const { return text_; }
    bool empty() const { return text_.empty(); }

private:
    std::wstring text_;
};

UIStringWide getUIStringW(const uint32_t stringidx = 0);
