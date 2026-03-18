#include "mainwindow.h"
#include <QApplication>
#include <QScreen>
#include <QFont>
#include <QDebug>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 全局字体：适配正点原子 800×480 LCD
    QFont font;
    font.setFamily("Noto Sans CJK SC");
    font.setPixelSize(13);
    app.setFont(font);

    // 隐藏鼠标光标（嵌入式触摸屏）
    // app.setOverrideCursor(Qt::BlankCursor);

    MainWindow w;

#ifdef Q_OS_LINUX
    // 嵌入式全屏显示
    w.showFullScreen();
#else
    w.show();
#endif

    return app.exec();
}
