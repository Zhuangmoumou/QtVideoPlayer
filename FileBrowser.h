#pragma once
#include <QWidget>
#include <QFileSystemModel>
#include <QTreeView>
#include <QVBoxLayout>

class FileBrowser : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowser(const QString &rootPath, QWidget *parent = nullptr);

signals:
    void fileSelected(const QString &filePath);

private:
    QFileSystemModel *model;
    QTreeView *tree;
};