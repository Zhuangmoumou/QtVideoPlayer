#include "FileBrowser.h"
#include <QStandardItemModel>
#include <QMouseEvent>
#include <QStyle>

FileBrowser::FileBrowser(const QString &rootPath, QWidget *parent)
    : QWidget(parent), rootPath(rootPath), currentPath(rootPath)
{
    setWindowTitle("选择媒体文件");
    setLayout(new QVBoxLayout);

    model = new QFileSystemModel(this);
    model->setRootPath(rootPath);
    model->setNameFilters({"*.mp4","*.avi","*.mkv","*.mp3","*.wav"});
    model->setNameFilterDisables(false);

    proxyModel = new QStandardItemModel(this);
    proxyModel->setColumnCount(1); // 确保只有一列，显示图标和文字
    tree = new QTreeView(this);
    tree->setModel(proxyModel);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setHeaderHidden(true);
    tree->setIconSize(QSize(24, 24)); // 设置图标大小
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

    // 初始化显示
    refreshView(currentPath);

    connect(tree, &QTreeView::clicked, this, [&](const QModelIndex &idx){
        if (proxyModel->itemFromIndex(idx)->data(Qt::UserRole+1).toBool()) {
            // 返回上级目录
            if (currentPath != rootPath) {
                QDir dir(currentPath);
                dir.cdUp();
                currentPath = dir.absolutePath();
                refreshView(currentPath);
            }
            return;
        }
        QFileInfo info = proxyModel->itemFromIndex(idx)->data(Qt::UserRole).value<QFileInfo>();
        if (info.isDir()) {
            currentPath = info.absoluteFilePath();
            refreshView(currentPath);
        } else if (info.isFile()) {
            emit fileSelected(info.absoluteFilePath());
        }
    });
}

void FileBrowser::refreshView(const QString &path)
{
    proxyModel->clear();
    QStyle *style = this->style();
    // 添加返回上级目录项（非根目录时）
    if (path != rootPath) {
        QIcon backIcon = QIcon::fromTheme("go-previous");
        if (backIcon.isNull())
            backIcon = style->standardIcon(QStyle::SP_ArrowBack);
        QStandardItem *backItem = new QStandardItem(backIcon, ".. 返回上级目录");
        backItem->setData(true, Qt::UserRole+1); // 标记为返回项
        backItem->setEditable(false);
        proxyModel->appendRow(backItem);
    }
    QDir dir(path);

    // 1. 添加所有文件夹（不使用 nameFilters）
    QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : dirs) {
        QIcon folderIcon = QIcon::fromTheme("folder");
        if (folderIcon.isNull())
            folderIcon = style->standardIcon(QStyle::SP_DirIcon);
        QStandardItem *item = new QStandardItem(folderIcon, info.fileName());
        item->setData(QVariant::fromValue(info), Qt::UserRole);
        item->setData(false, Qt::UserRole+1);
        item->setEditable(false);
        proxyModel->appendRow(item);
    }

    // 2. 添加所有符合 nameFilters 的文件
    QFileInfoList files = dir.entryInfoList(model->nameFilters(), QDir::Files, QDir::Name);
    for (const QFileInfo &info : files) {
        QIcon fileIcon = QIcon::fromTheme("media-playback-start");
        if (fileIcon.isNull())
            fileIcon = style->standardIcon(QStyle::SP_FileIcon);
        QStandardItem *item = new QStandardItem(fileIcon, info.fileName());
        item->setData(QVariant::fromValue(info), Qt::UserRole);
        item->setData(false, Qt::UserRole+1);
        item->setEditable(false);
        proxyModel->appendRow(item);
    }
}