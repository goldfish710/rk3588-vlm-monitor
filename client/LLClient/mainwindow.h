#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QtConcurrent>
#include"VideoDecoder.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    VideoDecoder* h265decoder=nullptr;
    VideoDecoder* h264decoder=nullptr;
    VideoDecoder* currentDecoder=nullptr; //当前选中的解码器
    QImage currentFrame; //当前帧图片 用于截图
    QString screenshotDir; //截图保存目录
    bool isRecording = false;

private slots:
    //选择页面
    void onSelectMain();
    void onSelectSub();
    void onSettings();

    //视频页面
    void onConnect();
    void onDisconnect();
    void onScreenshot();
    void onRecord();
    void onBack();

    void onframeReady(const QImage &image);
    void onRecordFinished(const QString &filePath);

private:
    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
