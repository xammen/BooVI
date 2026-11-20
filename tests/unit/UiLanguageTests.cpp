// Interface language runtime (UiLanguage.h) against the real catalog.
#include "veyra/ui/UiLanguage.h"

#include <QCoreApplication>
#include <cstdio>

namespace {
int failures = 0;
void expect(const QString& got, const QString& want, const char* what) {
    if (got == want) return;
    ++failures;
    std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what, got.toUtf8().constData(), want.toUtf8().constData());
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace veyra::ui;

    expect(i18n::resolve(QStringLiteral("ja")), QStringLiteral("ja"), "explicit choice kept");
    if (!i18n::apply(QStringLiteral("en"))) { std::printf("FAIL catalog did not load\n"); return 1; }

    // Exact entry, through Qt's own translate() as qsTr()/tr() use it.
    expect(QCoreApplication::translate("Any", "导出完成"), QStringLiteral("Export finished"), "exact via translator");
    // Disambiguated entry, and the plain one beside it.
    expect(QCoreApplication::translate("Any", "关闭", "off"), QStringLiteral("Off"), "disambiguation");
    expect(QCoreApplication::translate("Any", "关闭"), QStringLiteral("Close"), "plain entry next to a disambiguated one");
    // std::format message from the engine: slots carried across.
    expect(i18n::text(QStringLiteral("主机搜索发生错误（5），详情见日志；可手填 IP。")),
           QStringLiteral("Host search hit an error (5); details in the log. You can enter the IP by hand."), "format pattern");
    // A message built from pieces joined by "；".
    expect(i18n::text(QStringLiteral("HDR 视频自动使用 HEVC Main10 编码；视频导出完成")),
           QStringLiteral("HDR video automatically uses HEVC Main10; Video export finished"), "segments");
    // A capture format label with a technical tag at the end.
    expect(i18n::text(QStringLiteral("1920 x 1080 @ 120.00 fps · NV12 · 原生 · 低延迟 [format 16]")),
           QStringLiteral("1920 x 1080 @ 120.00 fps · NV12 · Native · low latency [format 16]"), "tagged segment");
    // The presentation status (VideoPresenter) joins its pieces with " · "; each is its own entry.
    expect(i18n::text(QStringLiteral("英伟达低延迟：关 · 显示同步：自动 · 不撕裂，也不等待垂直同步（延迟最低）")),
           QStringLiteral("NVIDIA Low Latency: Off · Display sync: auto · no tearing and no VSync wait (lowest latency)"), "presentation status pieces");
    expect(i18n::text(QStringLiteral("英伟达低延迟不可用，当前使用低排队（详见日志） · 输出上限：关闭 · 垂直同步：屏幕队列 1 帧，读帧前等待刷新 · 显示同步：垂直同步")),
           QStringLiteral("NVIDIA Low Latency unavailable; using low queue depth (see the log) · Output cap: off · VSync: 1-frame display queue, waits for refresh before reading a frame · Display sync: VSync"),
           "presentation fallback status pieces");
    // Unknown text and paths come back unchanged.
    expect(i18n::text(QStringLiteral("D:/视频/我的片子.mkv")), QStringLiteral("D:/视频/我的片子.mkv"), "path untouched");

    // The fork's French-source QML strings (i18n/fork.json).
    expect(QCoreApplication::translate("SettingsPage", "réglages"), QStringLiteral("settings"), "fork string in English");
    expect(QCoreApplication::translate("Any", "couper le son · %1 %"), QStringLiteral("mute · %1 %"), "fork string keeps its slot");

    i18n::apply(QStringLiteral("fr"));
    expect(QCoreApplication::translate("SettingsPage", "réglages"), QStringLiteral("réglages"), "fork string in French is its source");
    expect(QCoreApplication::translate("Any", "导出完成").isEmpty() ? QStringLiteral("missing") : QStringLiteral("found"), QStringLiteral("found"), "French keeps the main catalog");

    // No Japanese column: English, never the French source.
    i18n::apply(QStringLiteral("ja"));
    expect(QCoreApplication::translate("SettingsPage", "réglages"), QStringLiteral("settings"), "fork string falls back to English");

    if (!i18n::apply(QStringLiteral("zh-CN"))) { ++failures; std::printf("FAIL zh-CN did not apply\n"); }
    expect(QCoreApplication::translate("Any", "导出完成"), QStringLiteral("导出完成"), "source language");
    expect(i18n::text(QStringLiteral("导出完成")), QStringLiteral("导出完成"), "source language text()");
    expect(QCoreApplication::translate("SettingsPage", "réglages"), QStringLiteral("settings"), "fork string in Simplified Chinese falls back to English");

    std::printf("ui i18n tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
