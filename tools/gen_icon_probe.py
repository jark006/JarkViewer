"""Temporarily replace JarkViewerApp::DrawUi with a labeled icon-codepoint browser.

Usage: python tools/gen_icon_probe.py          # install the probe
       python tools/gen_icon_probe.py --revert # restore the previous DrawUi body
"""
import io
import sys

MAIN = 'JarkViewer/src/main.cpp'

CANDIDATES = [
    0xE713, 0xE74D, 0xE70F, 0xE8C8, 0xE7A8, 0xE790, 0xE91B, 0xE8E5,
    0xE8B7, 0xE73E, 0xE8D2, 0xE929, 0xE7C3, 0xE8B0, 0xE7A9, 0xE74F,
    0xE759, 0xE75B, 0xE7E6, 0xE9D9, 0xEA37, 0xE8AF, 0xE8A7, 0xE70B,
    0xE718, 0xE71B, 0xE74C, 0xE785, 0xE7C9, 0xE8FD, 0xE945, 0xE7EE,
    0xE706, 0xE70D, 0xE70E, 0xE8EF, 0xE839, 0xE7C1, 0xE8B9, 0xEB9F,
    # 编辑标注工具行的图标：已选定普通几何符号 矩形 25A1 / 椭圆 25CB / 画笔 3030 / 裁剪 25A3
    # （这一组是当时的候选，含图标字体的私用区字形）
    0xE739, 0xE75B, 0xE7C4, 0xE7FB, 0xE7FC, 0xE8A9, 0xE8AA, 0xE7A5,
    0xEA3A, 0xE91F, 0xE7A6, 0xE7A7, 0x25A1, 0x25CB, 0x25EF, 0x25AF,
    0xE70F, 0xE718, 0xE7E6, 0xEB7F, 0xEB7E, 0xE929, 0x2712, 0x270D,
    0x270E, 0x270F, 0xE8AC, 0xE8AD, 0xE7A8, 0xE8B0, 0xE123, 0xE7C6,
    0x2702, 0xE8A0, 0xE74E, 0xE8D2, 0xE7EE, 0xE706, 0xE7C9, 0xE945,
]


def glyph_literal(cp):
    return ('(char)(0xE0|(%d>>12)), (char)(0x80|((%d>>6)&0x3F)), '
            '(char)(0x80|(%d&0x3F)), 0' % (cp, cp, cp))


def build_probe_body():
    # 每行 4 个：图标按钮 + 完整码位标签（表格的单元格会把标签截断，所以不用表格）
    lines = []
    for index, cp in enumerate(CANDIDATES):
        lines.append('        { const char g[4] = { %s }; ImGui::Button(g, ImVec2(52, 40)); '
                     'ImGui::SameLine(); ImGui::TextUnformatted("%s"); }'
                     % (glyph_literal(cp), '%04X' % cp))
        lines.append('        if (%d %% 4 != 3) ImGui::SameLine(0, 26.0f);' % index)
    return '\n'.join(lines)


def main():
    source = io.open(MAIN, encoding='utf-8').read()
    start = source.index('    void DrawUi() override {')
    end = source.index('    void DrawScene() {')

    if '--revert' in sys.argv:
        body = io.open('tools/.drawui_backup', encoding='utf-8').read()
    else:
        io.open('tools/.drawui_backup', 'w', encoding='utf-8', newline='').write(source[start:end])
        body = ('    void DrawUi() override {\n'
                '        // 临时：带标签的图标码位浏览（由 tools/gen_icon_probe.py 生成）\n'
                '        jark::ui::UiHost::instance().setUiVisible(true);\n'
                '        ImGui::SetNextWindowSize({ 620, 760 }, ImGuiCond_Always);\n'
                '        ImGui::Begin("icon candidates");\n'
                f'{build_probe_body()}\n'
                '        ImGui::End();\n'
                '    }\n\n')

    io.open(MAIN, 'w', encoding='utf-8', newline='').write(source[:start] + body + source[end:])
    print('DrawUi ' + ('reverted' if '--revert' in sys.argv else 'replaced with icon probe'))


if __name__ == '__main__':
    main()
