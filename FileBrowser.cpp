#include "FileBrowser.h"

FileBrowser::FileBrowser(const QString &rootPath, QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("选择媒体文件");
    setLayout(new QVBoxLayout);
    model = new QFileSystemModel(this);
    model->setRootPath(rootPath);
    // 只显示常见媒体文件
    model->setNameFilters({"*.mp4","*.avi","*.mkv","*.mp3","*.wav"});
    model->setNameFilterDisables(false);

    tree = new QTreeView(this);
    tree->setModel(model);
    tree->setRootIndex(model->index(rootPath));
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setHeaderHidden(true);
    layout()->addWidget(tree);

    // 设置暗黑配色
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(40, 40, 40));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(30, 30, 30));
    darkPalette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Highlight, QColor(60, 120, 200));
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);

    this->setPalette(darkPalette);
    tree->setPalette(darkPalette);

    // 可选：设置滚动条样式和字体
    tree->setStyleSheet(
        "QTreeView { background-color: #282828; color: #f0f0f0; selection-background-color: transparent; }"
        "QTreeView::item:selected {"
        "   background: #3a6ea5;"
        "   border-radius: 10px;"
        "   margin: 4px;"
        "   color: #fff;"
        "}"
        "QTreeView::item {"
        "   margin: 4px;"
        "   padding: 8px 4px;"
        "   border-radius: 10px;"
        "}"
        "QScrollBar:vertical {"
        "   background: #222;"
        "   width: 18px;"
        "   margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: #444;"
        "   min-height: 40px;"
        "   border-radius: 8px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "   height: 0px;"
        "}"
        "QScrollBar::up-arrow:vertical, QScrollBar::down-arrow:vertical {"
        "   background: none;"
        "}"
        "QScrollBar:horizontal {"
        "   background: #222;"
        "   height: 18px;"
        "   margin: 0px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "   background: #444;"
        "   min-width: 40px;"
        "   border-radius: 8px;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "   width: 0px;"
        "}"
        "QScrollBar::left-arrow:horizontal, QScrollBar::right-arrow:horizontal {"
        "   background: none;"
        "}"
    );
    // 始终显示滚动条，便于触控
    tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(tree, &QTreeView::doubleClicked, this, [&](const QModelIndex &idx){
        QString file = model->filePath(idx);
        emit fileSelected(file);
    });
}