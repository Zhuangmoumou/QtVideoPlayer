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

    connect(tree, &QTreeView::doubleClicked, this, [&](const QModelIndex &idx){
        QString file = model->filePath(idx);
        emit fileSelected(file);
    });
}