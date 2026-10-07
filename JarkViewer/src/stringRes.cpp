#include "jarkUtils.h"

#include "Localization.h"

// 界面字符串表最大条目数
constexpr uint32_t STRING_MAX_NUM = 1024;

// 界面字符串表，列固定为：简体中文 / 繁體中文 / English / 日本語 / 한국어（见 jark::Language）
// 条目所在行号减 10 即为 stringID
// 因为索引是硬编码的，所以不要随意在中间增减条目，会打乱索引，只能在后面追加条目
//
// 注：还有一张 UIStringTableWide，供 Win32 API（窗口标题、消息框、菜单）使用，
// 两张表的同一个 ID 表示不同的文案，是历史遗留；新增文案请追加到对应表的末尾。
std::string_view UIStringTable[STRING_MAX_NUM][jark::kLanguageCount] = {
    {"NULL", "NULL", "NULL", "NULL", "NULL"},
    {"设置", "設定", "Settings", "設定", "설정"},
    {"常规", "一般", "General", "一般", "일반"},
    {"文件关联", "檔案關聯", "Association", "ファイル関連付け", "파일 연결"},
    {"帮助", "說明", "Help", "ヘルプ", "도움말"},
    {"关于", "關於", "About", "このアプリについて", "정보"},
    {"常见格式", "常見格式", "Common Formats", "よく使う形式", "일반 형식"},
    {"选择常用", "選擇常用", "Select Common", "よく使う形式を選択", "일반 형식 선택"},
    {"全选", "全選", "Select All", "すべて選択", "모두 선택"},
    {"全不选", "全部取消", "Clear All", "すべて解除", "모두 해제"},
    {"立即关联", "立即關聯", "Apply", "適用", "적용"}, // 10
    {"本软件原生绿色单文件，请把软件放置到合适位置再关联文件格式，若软件位置变化则需重新关联。\n若不再使用本软件，请点击【全不选】再点击【立即关联】即可移除所有关联关系。",
     "本軟體為原生綠色單一檔案，請先把軟體放到合適的位置再關聯檔案格式；若軟體位置有變動，需重新關聯。\n若不再使用本軟體，請點擊【全部取消】再點擊【立即關聯】，即可移除所有關聯。",
     "This software is a portable single file.  Please place the software in an appropriate location before associating file formats.\nIf you no longer to use this software,  please click \"Clear All\" and then click \"Apply\" to remove all associations.",
     "本ソフトは単一ファイルで動作します。先にソフトを適切な場所に置いてから関連付けを行ってください。場所を移動した場合は再関連付けが必要です。\n使用をやめる場合は【すべて解除】→【適用】で関連付けを削除できます。",
     "이 프로그램은 단일 파일로 동작합니다. 먼저 적절한 위치에 둔 뒤 파일 연결을 설정하세요. 위치가 바뀌면 다시 연결해야 합니다.\n더 이상 사용하지 않으면 [모두 해제] 후 [적용]을 눌러 연결을 제거할 수 있습니다."},
    {"旋转动画", "旋轉動畫", "Rotate Animation", "回転アニメーション", "회전 애니메이션"},
    {"缩放动画", "縮放動畫", "Zoom Animation", "ズームアニメーション", "확대/축소 애니메이션"},
    {"删除前提示", "刪除前提示", "Confirm Before Delete", "削除前に確認", "삭제 전 확인"},
    {"ICC色彩管理", "ICC 色彩管理", "ICC Color Management", "ICC カラーマネジメント", "ICC 색상 관리"},
    {"切换动画模式", "切換動畫模式", "Switch Animation Mode", "切替アニメーション", "전환 애니메이션 모드"},
    {"幻灯片顺序", "幻燈片順序", "Slideshow Order", "スライドショー順序", "슬라이드쇼 순서"},
    {"幻灯片间隔(秒)", "幻燈片間隔(秒)", "Slideshow Interval (seconds)", "スライドショー間隔（秒）", "슬라이드쇼 간격(초)"},
    {"编译时间 UTC+8", "編譯時間 UTC+8", "[Build time UTC+8]", "[ビルド日時 UTC+8]", "[빌드 시간 UTC+8]"}, // 19
    {"切图动画", "切圖動畫", "SwitchAnim", "切替アニメ", "전환 애니메이션"},
    {"无动画", "無動畫", "Off", "なし", "없음"},
    {"上下滑动", "上下滑動", "Vertical", "上下スライド", "상하 슬라이드"},
    {"左右滑动", "左右滑動", "Horizontal", "左右スライド", "좌우 슬라이드"},
    {"主题", "主題", "Theme", "テーマ", "테마"},
    {"跟随系统", "跟隨系統", "System", "システムに従う", "시스템 설정"},
    {"浅色", "淺色", "Light", "ライト", "라이트"},
    {"深色", "深色", "Dark", "ダーク", "다크"},
    {"语言", "語言", "Language", "言語", "언어"},
    {"打印", "列印", "Print", "印刷", "인쇄"}, // 29
    // 语言选项：各语言用自己的写法显示
    {"简体中文", "简体中文", "简体中文", "简体中文", "简体中文"},
    {"English", "English", "English", "English", "English"},
    {"使用Ctrl+O或拖入图像文件打开", "使用 Ctrl+O 或拖入圖像檔案開啟", "Use Ctrl+O or drag the image file to open.", "Ctrl+O または画像ファイルをドラッグして開く", "Ctrl+O 또는 이미지 파일을 끌어다 놓아 여세요"},
    {"图像格式不支持", "圖像格式不支援", "Image format not supported", "対応していない画像形式です", "지원하지 않는 이미지 형식입니다"},
    {"JarkViewer看图", "JarkViewer 看圖", "JarkViewer", "JarkViewer", "JarkViewer"},
    {"错误", "錯誤", "Error", "エラー", "오류"},
    {"鼠标右键", "滑鼠右鍵", "RightClick", "右クリック", "마우스 오른쪽"},
    {"菜单", "選單", "Menu", "メニュー", "메뉴"},
    {"退出程序", "結束程式", "Exit", "終了", "종료"},
    {"路径", "路徑", "Path", "パス", "경로"}, // 39
    {"大小", "大小", "FileSize", "サイズ", "크기"},
    {"分辨率", "解析度", "Resolution", "解像度", "해상도"},
    {"【按 C 键复制图像全部信息】", "【按 C 鍵複製影像全部資訊】", "[Press C to copy all image information]", "【C キーで全情報をコピー】", "[C 키로 모든 정보 복사]"},
    {"\n正提示词: ", "\n正提示詞: ", "\nPositive prompt: ", "\nポジティブプロンプト: ", "\n긍정 프롬프트: "},
    {"\n\n反提示词: ", "\n\n負提示詞: ", "\n\nNegative prompt: ", "\n\nネガティブプロンプト: ", "\n\n부정 프롬프트: "},
    {"\n\n参数: Steps:", "\n\n參數: Steps:", "\n\nParameter: Steps:", "\n\nパラメータ: Steps:", "\n\n매개변수: Steps:"},
    {"\n\nAI生图提示词:\n", "\n\nAI 生圖提示詞:\n", "\n\nAI-generated image prompt:\n", "\n\nAI 生成プロンプト:\n", "\n\nAI 생성 프롬프트:\n"},
    {"北纬 N", "北緯 N", "North Latitude", "北緯 N", "북위 N"},
    {"南纬 S", "南緯 S", "South Latitude", "南緯 S", "남위 S"},
    {"东经 E", "東經 E", "East Longitude", "東経 E", "동경 E"},
    {"西经 W", "西經 W", "West Longitude", "西経 W", "서경 W"}, // 50
    {"子图数量", "子圖數量", "Number of subImage", "サブ画像数", "하위 이미지 수"},
    {"\n\nAI生图提示词 ComfyUI工作流.json\n", "\n\nAI 生圖提示詞 ComfyUI 工作流.json\n", "\n\nAI-generated image prompt ComfyUI_workflow.json\n", "\n\nAI 生成プロンプト ComfyUI ワークフロー.json\n", "\n\nAI 생성 프롬프트 ComfyUI 워크플로.json\n"},
    {"\n方向: ", "\n方向: ", "\nExif.Image.Orientation: ", "\n方向: ", "\n방향: "},
    {"优先1:1显示", "優先 1:1 顯示", "Prefer 1:1 Display", "1:1 表示を優先", "1:1 표시 우선"},
    // 语言选项的母语写法（与 Language 枚举顺序一致：简中 / 繁中 / English / 日本語 / 한국어）
    {"繁體中文", "繁體中文", "繁體中文", "繁體中文", "繁體中文"},
    {"日本語", "日本語", "日本語", "日本語", "日本語"},
    {"한국어", "한국어", "한국어", "한국어", "한국어"},
    // —— 批量处理界面（追加在末尾，勿插入中间）——
    {"批量处理", "批次處理", "Batch", "バッチ処理", "일괄 처리"},
    {"选择要处理的文件", "選擇要處理的檔案", "Files to process", "処理するファイル", "처리할 파일"},
    {"转换/缩放", "轉換/縮放", "Convert/Resize", "変換/縮小", "변환/크기 조정"},
    {"重命名", "重新命名", "Rename", "名前変更", "이름 바꾸기"},
    {"旋转/翻转", "旋轉/翻轉", "Rotate/Flip", "回転/反転", "회전/뒤집기"},
    {"删除", "刪除", "Delete", "削除", "삭제"},
    {"输出格式", "輸出格式", "Output format", "出力形式", "출력 형식"},
    {"不变", "不變", "Keep", "変更なし", "유지"},
    {"彩色", "彩色", "Color", "カラー", "컬러"},
    {"黑白", "黑白", "Grayscale", "グレースケール", "흑백"},
    {"黑白文档", "黑白文件", "B/W document", "白黒文書", "흑백 문서"},
    {"黑白抖动", "黑白抖動", "B/W dither", "白黒ディザ", "흑백 디더"},
    {"开始处理", "開始處理", "Start", "開始", "시작"},
    {"取消", "取消", "Cancel", "キャンセル", "취소"},
    {"输出目录", "輸出資料夾", "Output folder", "出力先フォルダー", "출력 폴더"},
    {"选择目录", "選擇資料夾", "Choose folder", "フォルダーを選択", "폴더 선택"},
    {"与源文件同目录", "與來源檔案同資料夾", "Same folder as source", "元ファイルと同じ場所", "원본과 같은 폴더"},
    {"重命名前缀", "重新命名前綴", "Name prefix", "名前の接頭辞", "이름 접두사"},
    {"不旋转", "不旋轉", "No rotation", "回転なし", "회전 없음"},
    {"顺时针90°", "順時針90°", "90° clockwise", "時計回り90°", "시계 방향 90°"},
    {"逆时针90°", "逆時針90°", "90° counter-clockwise", "反時計回り90°", "시계 반대 90°"},
    {"水平翻转", "水平翻轉", "Flip horizontal", "左右反転", "좌우 반전"},
    {"垂直翻转", "垂直翻轉", "Flip vertical", "上下反転", "상하 반전"},
    {"覆盖已有文件", "覆蓋既有檔案", "Overwrite existing", "既存ファイルを上書き", "기존 파일 덮어쓰기"},
    {"请先选择要处理的文件", "請先選擇要處理的檔案", "Select files first", "処理するファイルを選択してください", "먼저 파일을 선택하세요"},
    {"正在处理...", "正在處理...", "Processing...", "処理中...", "처리 중..."},
    {"完成：成功 {}，跳过 {}，失败 {}", "完成：成功 {}，跳過 {}，失敗 {}", "Done: {} ok, {} skipped, {} failed", "完了: 成功 {}、スキップ {}、失敗 {}", "완료: 성공 {}, 건너뜀 {}, 실패 {}"},
    {"选择输出文件格式", "選擇輸出檔案格式", "Choose output format", "出力形式を選択", "출력 형식을 선택"},
    {"任务", "任務", "Task", "タスク", "작업"},
    {"应用图像调整", "套用影像調整", "Apply adjustments", "画像調整を適用", "이미지 조정 적용"},
    {"颜色模式", "色彩模式", "Color mode", "カラーモード", "색상 모드"},
    {"亮度", "亮度", "Brightness", "明るさ", "밝기"},
    {"对比度", "對比度", "Contrast", "コントラスト", "대비"},

    // —— 图像编辑与标注（追加在末尾，勿插入中间）——
    {"图像编辑与标注", "圖像編輯與標註", "Image editor", "画像編集・注釈", "이미지 편집·주석"},           // 91
    {"工具", "工具", "Tool", "ツール", "도구"},
    {"矩形", "矩形", "Rectangle", "矩形", "사각형"},
    {"椭圆", "橢圓", "Ellipse", "楕円", "타원"},
    {"箭头", "箭頭", "Arrow", "矢印", "화살표"},
    {"直线", "直線", "Line", "直線", "직선"},
    {"画笔", "畫筆", "Pen", "ペン", "펜"},
    {"马赛克", "馬賽克", "Mosaic", "モザイク", "모자이크"},
    {"文字", "文字", "Text", "テキスト", "텍스트"},
    {"裁剪", "裁剪", "Crop", "切り抜き", "자르기"},                                                   // 100
    {"颜色", "顏色", "Color", "色", "색상"},
    {"线宽", "線寬", "Width", "線幅", "선 굵기"},
    {"字号", "字號", "Font size", "文字サイズ", "글자 크기"},
    {"填充图形", "填滿圖形", "Filled", "塗りつぶし", "채우기"},
    {"撤销", "復原", "Undo", "元に戻す", "실행 취소"},
    {"重做", "重做", "Redo", "やり直し", "다시 실행"},
    {"编辑", "編輯", "Edit", "編集", "편집"},
    {"反相", "反相", "Invert", "階調の反転", "반전"},
    {"另存为", "另存新檔", "Save as", "名前を付けて保存", "다른 이름으로 저장"},
    {"复制", "複製", "Copy", "コピー", "복사"},                                                        // 110
    {"覆盖原文件", "覆蓋原檔案", "Overwrite", "上書き保存", "원본 덮어쓰기"},
    {"已复制到剪贴板", "已複製到剪貼簿", "Copied to clipboard", "クリップボードにコピーしました", "클립보드에 복사됨"},
    {"已保存", "已儲存", "Saved", "保存しました", "저장됨"},
    {"保存失败", "儲存失敗", "Save failed", "保存に失敗しました", "저장 실패"},
    {"在图像上拖动绘制，Ctrl+Z 撤销", "在圖像上拖曳繪製，Ctrl+Z 復原",
     "Drag to draw, Ctrl+Z to undo",
     "ドラッグで描画、Ctrl+Z で元に戻す",
     "드래그로 그리기, Ctrl+Z 실행 취소"},
    {"文字内容（回车放置）", "文字內容（Enter 放置）", "Text (Enter to place)", "テキスト（Enter で配置）", "텍스트 (Enter로 배치)"},
    {"完成", "完成", "Done", "完了", "완료"},
    {"放弃修改", "放棄修改", "Discard", "変更を破棄", "변경 사항 버리기"},
    {"未选中区域，请先在图像上框选", "未選取區域，請先在圖像上框選", "Select an area on the image first",
     "先に画像上で範囲を選択してください", "먼저 이미지에서 영역을 선택하세요"},
    {"应用裁剪", "套用裁剪", "Apply crop", "切り抜きを適用", "자르기 적용"},                          // 120
    {"顺序", "順序", "Forward", "順方向", "순방향"},                                                  // 121
    {"逆序", "逆序", "Backward", "逆方向", "역방향"},
    {"随机", "隨機", "Random", "ランダム", "무작위"},
    {"关闭", "關閉", "Close", "閉じる", "닫기"},                                                      // 126
    {"序号位数", "序號位數", "Digits", "桁数", "자릿수"},
    {"打印", "列印", "Print", "印刷", "인쇄"}, // 128

    // —— 帮助页（改成文字排版，不再用资源图）——
    {"快捷键与操作", "快速鍵與操作", "Shortcuts and controls", "ショートカットと操作", "단축키와 조작"},
    {"滚轮：缩放　　中键：EXIF 信息　　右键：菜单　　左键拖动：平移\n"
     "窗口左右边缘：上一张 / 下一张　　窗口四角：旋转 / 打印 / 设置\n"
     "Ctrl+O 打开　　Ctrl+B 批量处理　　Ctrl+E 编辑与标注\n"
     "Ctrl+S 保存动图帧　　Ctrl+C 复制图像　　Ctrl+P 打印　　Ctrl+W 退出\n"
     "Q / E 旋转　　A / W / S / D 平移　　F 适应窗口　　P 幻灯片播放\n"
     "J / K / L 动图上一帧 / 暂停 / 下一帧　　F11 全屏　　ESC 退出",
     "滚輪：縮放　　中鍵：EXIF 資訊　　右鍵：選單　　左鍵拖曳：平移\n"
     "視窗左右邊緣：上一張 / 下一張　　視窗四角：旋轉 / 列印 / 設定\n"
     "Ctrl+O 開啟　　Ctrl+B 批次處理　　Ctrl+E 編輯與標註\n"
     "Ctrl+S 儲存動圖影格　　Ctrl+C 複製影像　　Ctrl+P 列印　　Ctrl+W 結束\n"
     "Q / E 旋轉　　A / W / S / D 平移　　F 適應視窗　　P 幻燈片播放\n"
     "J / K / L 動圖上一格 / 暫停 / 下一格　　F11 全螢幕　　ESC 結束",
     "Wheel: zoom    Middle: EXIF    Right: menu    Left drag: pan\n"
     "Window edges: previous / next    Window corners: rotate / print / settings\n"
     "Ctrl+O open    Ctrl+B batch    Ctrl+E edit and annotate\n"
     "Ctrl+S save frames    Ctrl+C copy image    Ctrl+P print    Ctrl+W quit\n"
     "Q / E rotate    A / W / S / D pan    F fit    P slideshow\n"
     "J / K / L frame step / pause / next    F11 fullscreen    ESC quit",
     "ホイール：ズーム　　中ボタン：EXIF　　右ボタン：メニュー　　左ドラッグ：移動\n"
     "ウィンドウ左右端：前/次の画像　　四隅：回転 / 印刷 / 設定\n"
     "Ctrl+O 開く　　Ctrl+B バッチ処理　　Ctrl+E 編集と注釈\n"
     "Ctrl+S フレーム保存　　Ctrl+C 画像をコピー　　Ctrl+P 印刷　　Ctrl+W 終了\n"
     "Q / E 回転　　A / W / S / D 移動　　F ウィンドウに合わせる　　P スライドショー\n"
     "J / K / L コマ送り / 一時停止 / 次へ　　F11 全画面　　ESC 終了",
     "휠: 확대/축소　　가운데: EXIF　　오른쪽: 메뉴　　왼쪽 드래그: 이동\n"
     "창 좌우 가장자리: 이전 / 다음　　창 네 모서리: 회전 / 인쇄 / 설정\n"
     "Ctrl+O 열기　　Ctrl+B 일괄 처리　　Ctrl+E 편집 및 주석\n"
     "Ctrl+S 프레임 저장　　Ctrl+C 이미지 복사　　Ctrl+P 인쇄　　Ctrl+W 종료\n"
     "Q / E 회전　　A / W / S / D 이동　　F 창에 맞춤　　P 슬라이드쇼\n"
     "J / K / L 프레임 이동 / 일시정지 / 다음　　F11 전체 화면　　ESC 종료"},
};

// 供 Win32 API（窗口标题/消息框/右键菜单）使用的字符串表，ID 与窄表相互独立
std::string_view UIStringTableWide[STRING_MAX_NUM][jark::kLanguageCount] = {
    {"NULL", "NULL", "NULL", "NULL", "NULL"},
    {"JarkViewer看图", "JarkViewer 看圖", "JarkViewer", "JarkViewer", "JarkViewer"},
    {"文件关联设置成功！", "檔案關聯設定成功！", "Association successful!", "関連付けを設定しました", "파일 연결을 설정했습니다!"},
    {"文件关联设置失败！", "檔案關聯設定失敗！", "Association failed!", "関連付けの設定に失敗しました", "파일 연결 설정에 실패했습니다!"},
    {"保存当前帧到图像文件", "儲存目前影格到圖像檔案", "Save the current frame to an image file", "現在のフレームを画像ファイルに保存", "현재 프레임을 이미지 파일로 저장"},
    {"是否要将此动图或实况图视频的全部帧批量保存到png图片文件？\n\n帧数：", "是否要將此動圖或實況圖影片的全部影格批次儲存為 PNG 圖片檔案？\n\n影格數：", "Batch save each frames of this animated image to PNG image files? \n\nFrames:", "このアニメーション画像（またはライブフォトの動画）の全フレームを PNG 画像として保存しますか？\n\nフレーム数: ", "이 애니메이션(또는 라이브 포토 동영상)의 모든 프레임을 PNG 이미지로 저장할까요?\n\n프레임 수: "},
    {"保存每一帧到原图所在文件夹", "將每一影格儲存到原圖所在資料夾", "Save each frame to the folder containing the current image", "各フレームを元画像と同じフォルダに保存", "각 프레임을 원본 이미지 폴더에 저장"},
    {"确定要将以下文件移至回收站吗？", "確定要將下列檔案移至資源回收筒嗎？", "Move the following files to the recycle bin?", "次のファイルをごみ箱に移動しますか？", "다음 파일을 휴지통으로 이동할까요?"},
    {"删除失败，错误码", "刪除失敗，錯誤碼", "Deletion failed, error code", "削除に失敗しました。エラーコード", "삭제 실패, 오류 코드"},
    {"逐帧浏览", "逐格瀏覽", "Frame by frame", "フレーム送り", "프레임 이동"},
    {"逆时针旋转90°", "逆時針旋轉 90°", "Rotate 90° counterclockwise", "反時計回りに 90° 回転", "시계 반대 방향으로 90° 회전"}, // 10
    {"顺时针旋转90°", "順時針旋轉 90°", "Rotate 90° clockwise", "時計回りに 90° 回転", "시계 방향으로 90° 회전"},
    {"旋转180°", "旋轉 180°", "Rotate 180°", "180° 回転", "180° 회전"},
    {"窗口创建失败！", "視窗建立失敗！", "Window creation failed!", "ウィンドウの作成に失敗しました", "창 생성 실패!"},
    {"错误", "錯誤", "Error", "エラー", "오류"},
    {"提示", "提示", "Tips", "ヒント", "알림"},
    {"图像分辨率太大，将缩放到", "影像解析度太大，將縮放到", "Resolution is too high, it will be scaled down to", "解像度が大きすぎるため、縮小します: ", "이미지 해상도가 너무 커서 축소합니다: "},
    {"图像为空，无法复制到剪贴板", "影像為空，無法複製到剪貼簿", "The image is empty and cannot be copied to the clipboard.", "画像が空のためクリップボードにコピーできません", "이미지가 비어 있어 클립보드에 복사할 수 없습니다"},
    {"图像通道:  ", "影像通道:  ", "Channel:  ", "チャンネル:  ", "채널:  "},
    {"不支持的图像格式", "不支援的圖像格式", "Unsupported image formats", "対応していない画像形式", "지원하지 않는 이미지 형식"},
    {"图像格式转换失败", "圖像格式轉換失敗", "Image format conversion failed", "画像形式の変換に失敗しました", "이미지 형식 변환 실패"}, // 20
    {"无法打开剪贴板", "無法開啟剪貼簿", "Unable to open clipboard", "クリップボードを開けません", "클립보드를 열 수 없습니다"},
    {"清空剪贴板失败", "清空剪貼簿失敗", "Clearing clipboard failed", "クリップボードの消去に失敗しました", "클립보드 비우기 실패"},
    {"保存到图像文件", "儲存到圖像檔案", "Save to image file", "画像ファイルに保存", "이미지 파일로 저장"},
    {"XX", "XX", "XX", "XX", "XX"},
    {"复制EXIF信息 (&E)", "複製 EXIF 資訊 (&E)", "Copy &EXIF info", "EXIF 情報をコピー (&E)", "EXIF 정보 복사 (&E)"},
    {"复制文件路径 (&P)", "複製檔案路徑 (&P)", "Copy file &path", "ファイルパスをコピー (&P)", "파일 경로 복사 (&P)"},
    {"复制图像数据 (&C)", "複製影像資料 (&C)", "&Copy image data", "画像データをコピー (&C)", "이미지 데이터 복사 (&C)"},
    {"显示EXIF信息 (&I)", "顯示 EXIF 資訊 (&I)", "Show EXIF &info", "EXIF 情報を表示 (&I)", "EXIF 정보 표시 (&I)"},
    {"打开所在位置 (&L)", "開啟所在位置 (&L)", "Open file &location", "ファイルの場所を開く (&L)", "파일 위치 열기 (&L)"},
    {"删除到回收站 (&D)", "刪除到資源回收筒 (&D)", "Move to recycle bin", "ごみ箱に移動 (&D)", "휴지통으로 이동 (&D)"}, // 30
    {"打印 (&P)", "列印 (&P)", "&Print", "印刷 (&P)", "인쇄 (&P)"},
    {"设置 (&S)", "設定 (&S)", "&Settings", "設定 (&S)", "설정 (&S)"},
    {"关于 (&A)", "關於 (&A)", "&About", "このアプリについて (&A)", "정보 (&A)"},
    {"退出 (&X)", "結束 (&X)", "E&xit", "終了 (&X)", "종료 (&X)"},
    {"打开新图像 (&O)", "開啟新圖像 (&O)", "&Open new image", "新しい画像を開く (&O)", "새 이미지 열기 (&O)"},
    {"文件属性 (&A)", "檔案屬性 (&A)", "File properties", "ファイルのプロパティ", "파일 속성"},
    {"帮助 (&H)", "說明 (&H)", "&Help", "ヘルプ (&H)", "도움말 (&H)"},
    {"全屏 (&F)", "全螢幕 (&F)", "&FullScreen", "全画面 (&F)", "전체 화면 (&F)"},
    {"设置", "設定", "Settings", "設定", "설정"}, // 39
    {"打印", "列印", "Print", "印刷", "인쇄"},
    {"文件关联已完成，但缩略图扩展注册失败。部分格式可能无法在资源管理器中显示 JarkViewer 缩略图。",
     "檔案關聯已完成，但縮圖擴充功能註冊失敗。部分格式可能無法在檔案總管中顯示 JarkViewer 縮圖。",
     "Association completed, but thumbnail extension registration failed. Some formats may not show JarkViewer thumbnails in File Explorer.",
     "関連付けは完了しましたが、サムネイル拡張の登録に失敗しました。一部の形式ではエクスプローラーのサムネイルが表示されない可能性があります。",
     "파일 연결은 완료되었지만 썸네일 확장 등록에 실패했습니다. 일부 형식은 탐색기에서 JarkViewer 썸네일이 표시되지 않을 수 있습니다."},
    {"批量处理", "批次處理", "Batch", "バッチ処理", "일괄 처리"},
    {"确定要把选中的 {} 个文件移到回收站吗？", "確定要將選取的 {} 個檔案移至資源回收筒嗎？", "Move the selected {} files to the recycle bin?", "選択した {} 個のファイルをごみ箱に移動しますか？", "선택한 {}개 파일을 휴지통으로 이동할까요?"},
    {"批量处理 (&B)", "批次處理 (&B)", "&Batch process", "バッチ処理 (&B)", "일괄 처리 (&B)"},
    {"图像编辑与标注", "圖像編輯與標註", "Image editor", "画像編集・注釈", "이미지 편집·주석"},
    {"确定要覆盖原文件吗？此操作不可撤销。", "確定要覆蓋原檔案嗎？此操作無法復原。",
     "Overwrite the original file? This cannot be undone.",
     "元のファイルを上書きしますか？この操作は取り消せません。",
     "원본 파일을 덮어쓸까요? 이 작업은 되돌릴 수 없습니다."},
    {"编辑与标注 (&E)", "編輯與標註 (&E)", "&Edit and annotate", "編集と注釈 (&E)", "편집 및 주석 (&E)"},
    {"幻灯片播放 (&S)", "幻燈片播放 (&S)", "&Slideshow", "スライドショー (&S)", "슬라이드쇼 (&S)"},
};

namespace {

    // 当前语言对应的列；越界回落到简体中文
    size_t languageColumn() noexcept {
        const size_t column = static_cast<size_t>(jark::currentLanguage());
        return column < jark::kLanguageCount ? column : jark::kSimplifiedChineseIndex;
    }

    // 取某一列的文案，为空时依次回退到英文、简体中文
    std::string_view pick(std::string_view const* row, size_t column) noexcept {
        if (!row[column].empty())
            return row[column];
        if (!row[jark::kEnglishIndex].empty())
            return row[jark::kEnglishIndex];
        return row[jark::kSimplifiedChineseIndex];
    }

} // namespace

// 获取字符串
const char* const getUIString(const uint32_t stringidx) {
    if (stringidx >= STRING_MAX_NUM)
        return "NULL";

    const auto text = pick(UIStringTable[stringidx], languageColumn());
    return text.empty() ? "" : text.data();
}

const wchar_t* const getUIStringW(const uint32_t stringidx) {
    if (stringidx >= STRING_MAX_NUM)
        return L"NULL";

    // 宽字符版本由 UTF-8 文案转换而来（线程局部缓冲，直接取用安全）
    static thread_local std::wstring buffer;
    buffer = jarkUtils::utf8ToWstring(pick(UIStringTableWide[stringidx], languageColumn()));
    return buffer.c_str();
}
