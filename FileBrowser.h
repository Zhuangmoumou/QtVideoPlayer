#pragma once
#include <QWidget>
#include <QFileSystemModel>
#include <QTreeView>
#include <QVBoxLayout>
#include <QStandardItemModel>
#include <QDir>
#include <QFileInfo>

class FileBrowser : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowser(const QString &rootPath, QWidget *parent = nullptr);

signals:
    void fileSelected(const QString &filePath);

private:
    QFileSystemModel *model;
    QTreeView *tree;
    QStandardItemModel *proxyModel;
    QString rootPath;
    QString currentPath;
    void refreshView(const QString &path);

    // 防误触相关成员
    QModelIndex lastClickedIndex;
    qint64 lastClickTime;
};